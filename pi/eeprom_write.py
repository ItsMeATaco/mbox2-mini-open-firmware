#!/usr/bin/env python3
"""Write a firmware image to the Mbox 2's EEPROM (U24, AT24C64A) from a Raspberry Pi, with verification.

Runs on the Pi next to eeprom_dump.py, whose I2C and majority-read code it reuses.

    sudo python3 eeprom_write.py --self-test        prove writes work, on the chip's unused last page
    sudo python3 eeprom_write.py image.bin          write the pages that differ, then verify
    sudo python3 eeprom_write.py image.bin --all    rewrite every page the image covers

Safety:
  - the image must carry a valid TUSB3200 header (signature, 24C64 type, size, checksum), so a random
    or truncated file is refused;
  - the chip's current content is saved to eeprom-before-<time>.bin before anything is written;
  - afterwards the whole chip is read back by majority vote and any page that doesn't match is
    rewritten, for up to 5 rounds. The two-wire hookup misreads the odd bit, and a misread address
    byte could land a page in the wrong place, so nothing counts as written until that check passes.
"""
import argparse
import ctypes
import os
import struct
import sys
import time

import eeprom_dump as dump

PAGE = 32                 # AT24C64A page size; a page write must not cross a page boundary
WRITE_CYCLE = 0.005       # self-timed write cycle, 5 ms max
ROUNDS = 5
EXIT_VERIFY_FAILED, EXIT_BAD_IMAGE, EXIT_WRITE_PROTECTED = 1, 2, 3


def write_page(fd, addr, offset, data):
    """Page write, then wait out the internal write cycle by polling until the chip answers again."""
    buf = (ctypes.c_uint8 * (2 + len(data)))(offset >> 8, offset & 0xFF, *data)
    for attempt in range(5):
        try:
            dump.transfer(fd, [dump.I2cMsg(addr, 0, len(buf), buf)])
            break
        except OSError:
            if attempt == 4:
                raise
            time.sleep(WRITE_CYCLE)
    time.sleep(WRITE_CYCLE)
    for _ in range(25):
        try:
            dump.read_at(fd, addr, offset, 1)
            return
        except OSError:
            time.sleep(0.002)
    raise OSError(f"the chip stayed busy after writing offset {offset:#06x}")


def page_majority(fd, addr, offset, reads):
    copies = [dump.read_at(fd, addr, offset, PAGE) for _ in range(reads)]
    return bytes(max(set(column), key=column.count) for column in zip(*copies))


def read_chip(fd, addr, reads):
    """Majority-voted copy of the whole chip, or None if some byte had no clear majority."""
    image, _, unresolved, _, _ = dump.majority_read(fd, addr, dump.EEPROM_SIZE, reads)
    if unresolved:
        print("the chip can't be read reliably right now (no clear majority); nothing written. Re-run it.")
        return None
    return image


def check_image(image):
    if not 12 <= len(image) <= dump.EEPROM_SIZE:
        print(f"refused: {len(image)} bytes is not a TUSB3200 EEPROM image")
        return False
    if not dump.validate(image):
        print("refused: the image's TI header or checksum is invalid")
        return False
    if image[6] != 0x0A:
        print("refused: the header's EEPROM type isn't 0x0a (24C64)")
        return False
    return True


def verify_and_repair(fd, addr, target, reads):
    """Compare the whole chip with `target`, rewriting mismatched pages, for up to ROUNDS rounds."""
    for round_ in range(1, ROUNDS + 1):
        current = dump.majority_read(fd, addr, dump.EEPROM_SIZE, reads)[0]
        bad = [p for p in range(0, dump.EEPROM_SIZE, PAGE) if current[p:p + PAGE] != target[p:p + PAGE]]
        if not bad:
            repaired = f" after {round_ - 1} repair round(s)" if round_ > 1 else ""
            print(f"verify: the whole chip matches{repaired}")
            return True
        listed = ", ".join(f"{p:#06x}" for p in bad[:8]) + (" ..." if len(bad) > 8 else "")
        print(f"verify round {round_}: {len(bad)} page(s) differ ({listed}), rewriting")
        for p in bad:
            write_page(fd, addr, p, target[p:p + PAGE])
    print(f"verify failed: pages still differ after {ROUNDS} rounds")
    return False


def save_backup(image):
    name = time.strftime("eeprom-before-%Y%m%d-%H%M%S.bin")
    with open(name, "wb") as f:
        f.write(image)
    print(f"saved the chip's current content to {name}")


def write_image(fd, addr, path, rewrite_all, reads):
    with open(path, "rb") as f:
        image = f.read()
    if not check_image(image):
        return EXIT_BAD_IMAGE
    before = read_chip(fd, addr, reads)
    if before is None:
        return EXIT_VERIFY_FAILED
    save_backup(before)
    target = bytearray(before)
    target[:len(image)] = image
    end = (len(image) + PAGE - 1) // PAGE * PAGE
    pages = [p for p in range(0, end, PAGE) if rewrite_all or before[p:p + PAGE] != target[p:p + PAGE]]
    print(f"writing {len(pages)} of the {end // PAGE} pages the image covers ...")
    start = time.monotonic()
    for p in pages:
        write_page(fd, addr, p, target[p:p + PAGE])
    print(f"wrote in {time.monotonic() - start:.1f} s")
    return 0 if verify_and_repair(fd, addr, bytes(target), reads) else EXIT_VERIFY_FAILED


def self_test(fd, addr, reads):
    """Write a pattern to the last page (outside any firmware), read it back, then restore it."""
    before = read_chip(fd, addr, reads)
    if before is None:
        return EXIT_VERIFY_FAILED
    last = dump.EEPROM_SIZE - PAGE
    signature, header_size, _, _, _, size, _ = struct.unpack_from("<IBBBBHH", before)
    if signature == dump.SIGNATURE and header_size + size > last:
        print("refused: the firmware in the chip reaches into the last page")
        return EXIT_BAD_IMAGE
    save_backup(before)
    original = before[last:]
    pattern = bytes((0x5A + 37 * i) & 0xFF for i in range(PAGE))
    if pattern == original:
        pattern = bytes(b ^ 0xFF for b in pattern)
    print(f"last page ({last:#06x}) holds {original[:8].hex(' ')} ...; writing a test pattern")
    write_page(fd, addr, last, pattern)
    got = page_majority(fd, addr, last, reads)
    if got == original:
        print("writes are ignored: U24's write-protect pin (pin 7) is probably tied high. Nothing changed.")
        return EXIT_WRITE_PROTECTED
    stuck = got == pattern
    print("test pattern read back " + ("correctly" if stuck else f"WRONG: {got.hex(' ')}"))
    write_page(fd, addr, last, original)
    restored = verify_and_repair(fd, addr, before, reads)
    if stuck and restored:
        print("self-test passed: U24 is writable, and the last page is back to its original content")
        return 0
    return EXIT_VERIFY_FAILED


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("image", nargs="?", help="TUSB3200 EEPROM image to write")
    ap.add_argument("--all", action="store_true", help="rewrite every page, not just the ones that differ")
    ap.add_argument("--self-test", action="store_true", help="check that the chip accepts writes")
    ap.add_argument("--bus", type=int, default=1, help="I2C bus number (default 1: GPIO2/GPIO3)")
    ap.add_argument("--addr", type=lambda s: int(s, 0), default=0x50, help="EEPROM address (default 0x50)")
    ap.add_argument("--reads", type=int, default=7, help="reads per majority vote (default 7)")
    args = ap.parse_args()
    if not args.self_test and not args.image:
        ap.error("give an image file, or --self-test")

    fd = os.open(f"/dev/i2c-{args.bus}", os.O_RDWR)
    try:
        if args.self_test:
            return self_test(fd, args.addr, max(args.reads, 3))
        return write_image(fd, args.addr, args.image, args.all, max(args.reads, 3))
    finally:
        os.close(fd)


if __name__ == "__main__":
    sys.exit(main())
