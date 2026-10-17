#!/usr/bin/env python3
"""Read the open firmware's diagnostic strings (firmware/src/diag.c) through fwdebug.Mbox: usbfs on the
Pi, the WinUSB maintenance interface on Windows (v0.20+).

    python diagstrings.py [INDEX ...]      hex indexes; default 40 49 4A 4B 4C 4D 60 61
"""
import sys

from fwdebug import Mbox

NAMES = {0x40: "status", 0x49: "counters", 0x4A: "arrival 0-15", 0x4B: "arrival 16-31", 0x4C: "pickup 0-15",
         0x4D: "pickup 16-31", 0x60: "slack 0-15", 0x61: "slack 16-31"}


def read(mbox, index):
    return mbox.control(0x80, 6, 0x0300 | index, 0x0409, 255)[2:].decode("utf-16-le")


def main():
    indexes = [int(a, 16) for a in sys.argv[1:]] or list(NAMES)
    mbox = Mbox()
    for i in indexes:
        print(f"{i:02X} {NAMES.get(i, ''):14s} {read(mbox, i)}")


if __name__ == "__main__":
    main()
