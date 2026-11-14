#!/usr/bin/env python3
"""Wrap a TUSB3200A EEPROM image (mkimage.py's output) as a DFU file: the image unchanged, plus the
16-byte suffix of the USB DFU 1.1 specification (appendix B), which dfu-util and fwupd read and strip.

    python3 mkdfu.py mbox2-open.bin mbox2-open.dfu [--vid 0x0DBA] [--pid 0xFFFF] [--header-version N]

Meant for Avid's own update mode on a stock Mbox 2 Mini: its firmware has a standard DFU interface (runtime
0DBA:3000, update mode 0DBA:3001, download only, 8-byte transfers), and Avid's updater images are the same
TI EEPROM format as ours. --pid 0xFFFF (any) lets the file match either ID.
--header-version rewrites the TI header's version byte (our images carry the firmware version, 27 for v0.27,
below every Avid version, 130-143); the boot ROM doesn't check it and the payload checksum doesn't cover it.
"""
import argparse
import struct
import sys
import zlib

SIGNATURE = 0x04513200
HEADER_SIZE = 12
SUFFIX_SIZE = 16


def check_image(image):
    if len(image) < HEADER_SIZE:
        raise ValueError("too short for a TUSB3200 image")
    sig, hsize, version, eeprom, dtype, size, csum = struct.unpack("<IBBBBHH", image[:HEADER_SIZE])
    if sig != SIGNATURE or hsize != HEADER_SIZE:
        raise ValueError(f"not a TUSB3200 image (signature {sig:#010x}, header size {hsize})")
    if len(image) != HEADER_SIZE + size:
        raise ValueError(f"header says {size} payload bytes, the file has {len(image) - HEADER_SIZE}")
    if sum(image[HEADER_SIZE:]) & 0xFFFF != csum:
        raise ValueError("payload checksum doesn't match the header")
    if HEADER_SIZE + size > 8192:
        raise ValueError("larger than the 8 KB EEPROM")
    return version, eeprom, dtype, size, csum


def dfu_suffix(data, vid, pid, bcd):
    body = struct.pack("<HHHH", bcd, pid, vid, 0x0100) + b"UFD" + bytes([SUFFIX_SIZE])
    crc = zlib.crc32(data + body) ^ 0xFFFFFFFF          # DFU's CRC-32 has no final inversion
    return body + struct.pack("<I", crc)


def read_dfu(blob):
    """Split a DFU file into payload and suffix fields, checking the suffix as dfu-util does."""
    if len(blob) < SUFFIX_SIZE or blob[-8:-5] != b"UFD" or blob[-5] != SUFFIX_SIZE:
        raise ValueError("no DFU suffix")
    crc = struct.unpack("<I", blob[-4:])[0]
    if zlib.crc32(blob[:-4]) ^ 0xFFFFFFFF != crc:
        raise ValueError("DFU suffix CRC doesn't match")
    bcd, pid, vid, bcd_dfu = struct.unpack("<HHHH", blob[-16:-8])
    return blob[:-SUFFIX_SIZE], {"vid": vid, "pid": pid, "bcdDevice": bcd, "bcdDFU": bcd_dfu, "crc": crc}


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("image")
    ap.add_argument("out")
    ap.add_argument("--vid", type=lambda s: int(s, 0), default=0x0DBA)
    ap.add_argument("--pid", type=lambda s: int(s, 0), default=0xFFFF)
    ap.add_argument("--bcd", type=lambda s: int(s, 0), default=0xFFFF, help="bcdDevice in the suffix (0xFFFF: any)")
    ap.add_argument("--header-version", type=int, help="rewrite the TI header's version byte (0-255)")
    args = ap.parse_args()

    image = bytearray(open(args.image, "rb").read())
    try:
        version, _, _, size, csum = check_image(image)
    except ValueError as e:
        sys.exit(f"{args.image}: {e}")
    if args.header_version is not None:
        image[5] = args.header_version & 0xFF
        version = image[5]
    blob = bytes(image) + dfu_suffix(bytes(image), args.vid, args.pid, args.bcd)

    payload, suffix = read_dfu(blob)                    # read it back the way the tools will
    assert payload == bytes(image)
    check_image(payload)
    with open(args.out, "wb") as f:
        f.write(blob)
    print(f"{args.out}: {len(payload)}-byte image (header version {version}, {size} bytes of code, checksum "
          f"{csum:#06x}) + DFU suffix (VID {suffix['vid']:04X}, PID {suffix['pid']:04X}, CRC {suffix['crc']:08X})")
    return 0


if __name__ == "__main__":
    sys.exit(main())
