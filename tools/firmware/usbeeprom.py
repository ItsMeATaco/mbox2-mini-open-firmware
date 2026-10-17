#!/usr/bin/env python3
"""Read and write the Mbox's firmware EEPROM over USB, through the running open firmware.

    sudo python3 usbeeprom.py check              read the image in the EEPROM and verify its checksum
    sudo python3 usbeeprom.py dump out.bin       read the whole 8 KB chip
    sudo python3 usbeeprom.py selftest           write, read back and restore the unused last page
    sudo python3 usbeeprom.py write image.bin    install a firmware image, verify it, reboot

On Windows, the same without sudo (python tools/firmware/usbeeprom.py write build/firmware/mbox2-open.bin):
from firmware v0.20 on, the requests go through the Mbox's WinUSB maintenance interface (winusb.py).

No wires needed: the TUSB3200's own I2C controller (TI SLES018A A.5.5, I2CADR/I2CDATI/I2CDATO/I2CCTL
at FFC3h-FFC0h) is driven register by register with the firmware's peek/poke vendor requests
(fwdebug.py), the same bus the boot ROM loads the firmware from. Slow (a few USB requests per byte)
but needs nothing from the firmware beyond peek/poke. The EEPROM is an AT24C64A at I2C address 50h
with 16-bit memory addresses and 32-byte pages. Found 2026-10-16: the read phase starts with a dummy
write to I2CDATO after I2CADR gets the read address.

Writing is ordered so an interruption can't leave a half-written image that boots: the header's
signature is erased first, the rest of the image is written and verified, and the signature goes back
last. Without a valid signature the boot ROM starts as TI's USB boot device (0451:3200) instead, and
the Pi's I2C wires (pi/flash.sh) remain the way back. Needs fwdebug.py alongside.
"""
import struct
import sys
import time

from fwdebug import Mbox

I2CCTL, I2CDATO, I2CDATI, I2CADR = 0xFFC0, 0xFFC1, 0xFFC2, 0xFFC3
RXF, ERR, TXE, STPRD, STPWR = 0x80, 0x20, 0x08, 0x02, 0x01
EEPROM = 0x50
EEPROM_SIZE = 8192
PAGE = 32                                           # a page write must not cross a page boundary
SIGNATURE = 0x04513200
HEADER_SIZE = 12


class I2CError(Exception):
    pass


class UsbI2C:
    def __init__(self, mbox):
        self.m = mbox
        self.read_trigger = None                    # how a read starts: found on the first read

    def ctl(self):
        return self.m.peek(I2CCTL)[0]

    def wait(self, bit, what, timeout=0.2):
        end = time.time() + timeout
        while True:
            c = self.ctl()
            if c & ERR:
                self.m.poke(I2CCTL, 0)
                raise I2CError(f"no acknowledge while {what} (I2CCTL {c:02X})")
            if c & bit:
                return
            if time.time() > end:
                raise I2CError(f"timed out while {what} (I2CCTL {c:02X})")

    def send(self, byte, what):
        self.m.poke(I2CDATO, byte)
        self.wait(TXE, what)

    def read(self, offset, n):
        """Random read of n bytes from the EEPROM at offset (a write of the address, then a read)."""
        m = self.m
        m.poke(I2CCTL, 0)                           # 100 kHz, no stop pending, clear ERR
        m.poke(I2CADR, EEPROM << 1)
        self.send(offset >> 8, "sending the address high byte")
        self.send(offset & 0xFF, "sending the address low byte")
        if n == 1:
            m.poke(I2CCTL, STPRD)
        m.poke(I2CADR, EEPROM << 1 | 1)
        self.start_read()
        out = bytearray()
        for i in range(n):
            if i:
                self.wait(RXF, f"receiving byte {i}")
            if i == n - 2:
                m.poke(I2CCTL, STPRD)               # stop after the next (last) byte
            out.append(m.peek(I2CDATI)[0])          # reading I2CDATI frees the receiver for the next byte
        m.poke(I2CCTL, 0)
        return bytes(out)

    def write(self, offset, data):
        """Page write: up to 32 bytes inside one page. Waits until the chip has finished writing."""
        assert 0 < len(data) <= PAGE and offset // PAGE == (offset + len(data) - 1) // PAGE
        m = self.m
        m.poke(I2CCTL, 0)
        m.poke(I2CADR, EEPROM << 1)
        self.send(offset >> 8, "sending the address high byte")
        self.send(offset & 0xFF, "sending the address low byte")
        for i, b in enumerate(data):
            if i == len(data) - 1:
                m.poke(I2CCTL, STPWR)               # stop after this byte
            self.send(b, f"writing byte {i}")
        m.poke(I2CCTL, 0)
        self.wait_ready()

    def wait_ready(self, timeout=0.1):
        """The chip ignores its address for up to 5 ms while it programs a page: poll until it answers."""
        end = time.time() + timeout
        while True:
            try:
                self.read(0, 1)
                return
            except I2CError:
                if time.time() > end:
                    raise

    def start_read(self):
        """The data sheet doesn't say what starts the read phase: try the address write alone, then a
        dummy write to I2CDATO; remember whichever worked."""
        if self.read_trigger in (None, "address"):
            try:
                self.wait(RXF, "waiting for the first byte", timeout=0.05)
                self.read_trigger = "address"
                return
            except I2CError:
                if self.read_trigger == "address":
                    raise
        self.m.poke(I2CDATO, 0x00)
        self.wait(RXF, "waiting for the first byte after a dummy write")
        self.read_trigger = "dummy write"


def read_range(i2c, offset, n, chunk=32):
    data = bytearray()
    while len(data) < n:
        k = min(chunk, n - len(data))
        data += i2c.read(offset + len(data), k)
    return bytes(data)


def check(i2c):
    header = read_range(i2c, 0, HEADER_SIZE)
    print(f"read trigger: {i2c.read_trigger}")
    sig, hsize, version, eetype, dtype, size, csum = struct.unpack("<IBBBBHH", header)
    print(f"header {header.hex(' ')}: signature {sig:08X}, version {version}, {size} bytes, sum {csum:04X}")
    if sig != SIGNATURE or hsize != HEADER_SIZE or not 0 < size <= EEPROM_SIZE - HEADER_SIZE:
        sys.exit("not a valid TUSB3200 image header")
    t0 = time.time()
    payload = read_range(i2c, HEADER_SIZE, size)
    got = sum(payload) & 0xFFFF
    print(f"read {size} bytes in {time.time() - t0:.1f} s, payload sum {got:04X}: "
          f"{'matches' if got == csum else 'MISMATCH'}")
    return got == csum


def write_range(i2c, offset, data):
    """Write data from offset, split at page boundaries."""
    pos = 0
    while pos < len(data):
        here = offset + pos
        k = min(PAGE - here % PAGE, len(data) - pos)
        i2c.write(here, data[pos:pos + k])
        pos += k


def selftest(i2c):
    last = EEPROM_SIZE - PAGE
    header = read_range(i2c, 0, HEADER_SIZE)
    size = struct.unpack("<IBBBBHH", header)[5]
    if HEADER_SIZE + size > last:
        sys.exit("refused: the firmware in the chip reaches into the last page")
    original = read_range(i2c, last, PAGE)
    pattern = bytes((0x5A + 37 * i) & 0xFF for i in range(PAGE))
    print(f"last page ({last:#06x}) holds {original[:8].hex(' ')} ...; writing a test pattern")
    write_range(i2c, last, pattern)
    back = read_range(i2c, last, PAGE)
    write_range(i2c, last, original)
    restored = read_range(i2c, last, PAGE) == original
    print(f"pattern read back {'correctly' if back == pattern else 'WRONG: ' + back.hex(' ')}; "
          f"original {'restored' if restored else 'NOT restored'}")
    return back == pattern and restored


def verify(i2c, offset, data, what):
    back = read_range(i2c, offset, len(data))
    bad = [offset + i for i in range(len(data)) if back[i] != data[i]]
    if bad:
        raise I2CError(f"{what}: {len(bad)} byte(s) differ, first at {bad[0]:#06x}")


def install(i2c, path, reboot=True):
    image = open(path, "rb").read()
    sig, hsize, version, _, _, size, csum = struct.unpack("<IBBBBHH", image[:HEADER_SIZE])
    if sig != SIGNATURE or hsize != HEADER_SIZE or len(image) != HEADER_SIZE + size \
            or sum(image[HEADER_SIZE:]) & 0xFFFF != csum:
        sys.exit(f"{path} is not a valid TUSB3200 image")
    print(f"installing {path}: version {version}, {size} bytes")
    t0 = time.time()
    write_range(i2c, 0, b"\xFF\xFF\xFF\xFF")        # erase the signature: no boot until the end
    verify(i2c, 0, b"\xFF\xFF\xFF\xFF", "erasing the signature")
    write_range(i2c, 4, image[4:])
    verify(i2c, 4, image[4:], "verifying the image")
    print(f"image written and verified in {time.time() - t0:.0f} s; restoring the signature")
    write_range(i2c, 0, image[:4])
    verify(i2c, 0, image[:4], "verifying the signature")
    if not check(i2c):
        raise I2CError("the installed image doesn't check out")
    if reboot:
        print("rebooting the Mbox into the new firmware")
        try:
            i2c.m.control(0x80, 6, 0x0300 | 0x4F, 0x0409, 64)  # diag string 0x4F: reboot
        except OSError:
            pass
        # A reboot resets the TUSB3200 but not the converters, and after reboots the DAC has been found
        # stuck (distorted, or everything in one ear) until a power cycle (2026-10-16).
        print("done: unplug the Mbox and plug it back in before listening (a reboot can leave the DAC stuck)")
    return True


def main():
    if len(sys.argv) < 2 or sys.argv[1] not in ("check", "dump", "selftest", "write") \
            or sys.argv[1] in ("dump", "write") and len(sys.argv) < 3:
        sys.exit(__doc__)
    i2c = UsbI2C(Mbox())
    cmd = sys.argv[1]
    if cmd == "check":
        return 0 if check(i2c) else 1
    if cmd == "selftest":
        return 0 if selftest(i2c) else 1
    if cmd == "write":
        return 0 if install(i2c, sys.argv[2]) else 1
    data = read_range(i2c, 0, EEPROM_SIZE)
    with open(sys.argv[2], "wb") as f:
        f.write(data)
    print(f"wrote {sys.argv[2]} ({len(data)} bytes, read trigger: {i2c.read_trigger})")
    return 0


if __name__ == "__main__":
    sys.exit(main())
