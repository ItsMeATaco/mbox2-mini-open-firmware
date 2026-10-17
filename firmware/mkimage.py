#!/usr/bin/env python3
"""Turn SDCC's Intel HEX output into a TUSB3200A EEPROM image (TI SLES018A, Table 2-2 header).

    python3 mkimage.py firmware.ihx firmware.bin --version 1

Header: signature 0x04513200, header size 12, version, EEPROM type 0x0A (24C64), data type 0x01
(application code), payload size, 16-bit sum of the payload. The payload is the code image from
address 0, loaded by the boot ROM into the 8 KB program RAM.
"""
import argparse
import struct
import sys

SIGNATURE = 0x04513200
HEADER_SIZE = 12
EEPROM_SIZE = 8192
CODE_RAM = 8192


def load_ihx(path):
    image = bytearray()
    with open(path) as f:
        for line in f:
            line = line.strip()
            if not line.startswith(":"):
                continue
            record = bytes.fromhex(line[1:])
            if sum(record) & 0xFF:
                raise ValueError(f"bad checksum in {line}")
            count, address, kind = record[0], (record[1] << 8) | record[2], record[3]
            if kind == 1:
                break
            if kind != 0:
                raise ValueError(f"unsupported Intel HEX record type {kind}")
            end = address + count
            if end > len(image):
                image.extend(b"\x00" * (end - len(image)))
            image[address:end] = record[4:4 + count]
    return bytes(image)


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("ihx")
    ap.add_argument("out")
    ap.add_argument("--version", type=int, default=1, help="header version byte (0-255)")
    args = ap.parse_args()

    payload = load_ihx(args.ihx)
    limit = min(CODE_RAM, EEPROM_SIZE - HEADER_SIZE)
    if not payload or len(payload) > limit:
        print(f"payload is {len(payload)} bytes; it must be 1-{limit}")
        return 1
    header = struct.pack("<IBBBBHH", SIGNATURE, HEADER_SIZE, args.version & 0xFF, 0x0A, 0x01,
                         len(payload), sum(payload) & 0xFFFF)
    with open(args.out, "wb") as f:
        f.write(header + payload)
    print(f"{args.out}: {len(payload)} bytes of code, version {args.version}, checksum {sum(payload) & 0xFFFF:#06x}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
