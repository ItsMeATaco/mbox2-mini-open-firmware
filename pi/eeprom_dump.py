#!/usr/bin/env python3
"""Read-only backup of the Mbox 2's firmware EEPROM (U24, AT24C64A) from a Raspberry Pi.

Runs on the Pi (Raspberry Pi OS, I2C enabled), standard library only. It never writes to the
EEPROM: every I2C transaction is "send the 2-byte address, then read".

    python3 eeprom_dump.py --scan            list the I2C addresses that answer
    python3 eeprom_dump.py                   read all 8 KB 7 times, majority-vote, validate, save
    python3 eeprom_dump.py --out mini.bin    choose the output file
    python3 eeprom_dump.py --reads 9         more reads for a noisier bus

The majority vote is there because the two-wire hookup (ground returning through the USB cable)
occasionally misreads a bit: on 2026-10-16, 8 single-bit 0->1 flips in 6 reads of 8 KB.

See the README for the wiring; pi/flash.sh runs this. The Mbox must be powered from its USB cable, and the
Pi connects only SDA and SCL (never 3.3 V).
"""
import argparse
import ctypes
import fcntl
import hashlib
import os
import struct
import sys
import time

I2C_RDWR = 0x0707
I2C_M_RD = 0x0001
EEPROM_SIZE = 8192          # 24C64
CHUNK = 32                  # bytes per read transaction
SIGNATURE = 0x04513200      # TUSB3200 EEPROM header signature (TI SLES018A, Table 2-2)
EEPROM_TYPES = {0x09: "24C32", 0x0A: "24C64"}


class I2cMsg(ctypes.Structure):
    _fields_ = [("addr", ctypes.c_uint16), ("flags", ctypes.c_uint16),
                ("len", ctypes.c_uint16), ("buf", ctypes.POINTER(ctypes.c_uint8))]


class I2cRdwr(ctypes.Structure):
    _fields_ = [("msgs", ctypes.POINTER(I2cMsg)), ("nmsgs", ctypes.c_uint32)]


def transfer(fd, msgs):
    array = (I2cMsg * len(msgs))(*msgs)
    request = I2cRdwr(array, len(msgs))
    # Pass the struct itself as a buffer: a 64-bit address overflows ioctl's int argument.
    fcntl.ioctl(fd, I2C_RDWR, request)


def read_at(fd, addr, offset, length):
    """Random read: write the 16-bit word address, repeated start, read `length` bytes."""
    out = (ctypes.c_uint8 * 2)(offset >> 8, offset & 0xFF)
    into = (ctypes.c_uint8 * length)()
    transfer(fd, [I2cMsg(addr, 0, 2, out), I2cMsg(addr, I2C_M_RD, length, into)])
    return bytes(into)


def read_all(fd, addr, size):
    data = bytearray()
    for offset in range(0, size, CHUNK):
        for attempt in range(5):
            try:
                data += read_at(fd, addr, offset, min(CHUNK, size - offset))
                break
            except OSError as error:
                if attempt == 4:
                    raise OSError(f"reading offset {offset:#06x} failed: {error}") from error
                time.sleep(0.02)
    return bytes(data)


def majority_read(fd, addr, size, reads):
    """Read the chip `reads` times and keep each byte's most common value.

    Returns (image, bytes that varied, bytes without a strict majority, bits read high, bits read low).
    """
    images = [read_all(fd, addr, size) for _ in range(reads)]
    result = bytearray(size)
    varied = unresolved = read_high = read_low = 0
    for off in range(size):
        values = [image[off] for image in images]
        best = max(set(values), key=values.count)
        result[off] = best
        count = values.count(best)
        if count != reads:
            varied += 1
            if count <= reads // 2:
                unresolved += 1
            for value in values:
                read_high += bin(value & ~best & 0xFF).count("1")
                read_low += bin(best & ~value & 0xFF).count("1")
    return bytes(result), varied, unresolved, read_high, read_low


def scan(fd):
    found = []
    for addr in range(0x08, 0x78):
        probe = (ctypes.c_uint8 * 1)()
        try:
            transfer(fd, [I2cMsg(addr, I2C_M_RD, 1, probe)])
            found.append(addr)
        except OSError:
            pass
    print("I2C devices answering:", " ".join(f"0x{a:02x}" for a in found) or "none")
    if 0x50 in found:
        print("  0x50 = the firmware EEPROM (U24)")
    if any(a in found for a in (0x10, 0x11)):
        print("  0x10/0x11 = likely the CS4392 DAC in I2C control-port mode")


def validate(image):
    signature, header_size, version, eeprom_type, data_type, size, checksum = struct.unpack_from("<IBBBBHH", image)
    print(f"signature   {signature:#010x} {'OK' if signature == SIGNATURE else 'MISMATCH'}")
    print(f"header size {header_size}")
    print(f"version     {version} (firmware {version // 100}.{version % 100:02d})")
    print(f"EEPROM type {eeprom_type:#04x} ({EEPROM_TYPES.get(eeprom_type, 'unknown')})")
    print(f"data type   {data_type:#04x} ({'application code' if data_type == 1 else 'unknown'})")
    print(f"payload     {size} bytes")
    if signature != SIGNATURE or not 12 <= header_size <= 64 or header_size + size > len(image):
        print("header invalid: is the wiring right, and is this the Mbox's EEPROM?")
        return False
    computed = sum(image[header_size:header_size + size]) & 0xFFFF
    ok = computed == checksum
    print(f"checksum    stored {checksum:#06x}, computed {computed:#06x} {'OK' if ok else 'MISMATCH'}")
    return ok


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--bus", type=int, default=1, help="I2C bus number (default 1: GPIO2/GPIO3)")
    ap.add_argument("--addr", type=lambda s: int(s, 0), default=0x50, help="EEPROM address (default 0x50)")
    ap.add_argument("--scan", action="store_true", help="only list the devices on the bus")
    ap.add_argument("--out", help="output file (default mbox2-eeprom-<time>.bin)")
    ap.add_argument("--reads", type=int, default=7, help="full reads to majority-vote over (default 7)")
    args = ap.parse_args()

    fd = os.open(f"/dev/i2c-{args.bus}", os.O_RDWR)
    try:
        if args.scan:
            scan(fd)
            return 0
        first, varied, unresolved, read_high, read_low = majority_read(fd, args.addr, EEPROM_SIZE, max(args.reads, 3))
        if varied:
            print(f"{varied} bytes varied between the reads ({read_high} bits misread as 1, {read_low} as 0); "
                  "kept each byte's majority value")
        if unresolved:
            print(f"{unresolved} bytes had no clear majority: the bus is too noisy. Re-run it, or add a ground wire.")
            return 1
        ok = validate(first)
        out = args.out or time.strftime("mbox2-eeprom-%Y%m%d-%H%M%S.bin")
        with open(out, "wb") as f:
            f.write(first)
        print(f"saved {out} ({len(first)} bytes), SHA-256 {hashlib.sha256(first).hexdigest()}")
        return 0 if ok else 2
    finally:
        os.close(fd)


if __name__ == "__main__":
    sys.exit(main())
