#!/usr/bin/env python3
"""Talk to the Mbox open firmware's bring-up requests, from the Raspberry Pi or Windows (standard library only).

    sudo python3 fwdebug.py regs                  clock, codec port, DMA and endpoint registers
    sudo python3 fwdebug.py watch [N]             N samples of frame number, MCLK count, endpoint counts
    sudo python3 fwdebug.py peek ADDR [N]         read N bytes of external data memory (hex address)
    sudo python3 fwdebug.py poke ADDR VALUE       write one byte (hex)
    sudo python3 fwdebug.py ports                 read GPIO ports P1 and P3
    sudo python3 fwdebug.py setport 1|3 VALUE     write P1 or P3 (hex)
    sudo python3 fwdebug.py clock master|slave    whether the TUSB3200 drives LRCK/SCLK
    sudo python3 fwdebug.py codec 6|0 [CRESET]    codec port mode: 6 = I2S, 0 = general purpose with CRESET 0/1
    sudo python3 fwdebug.py mclk 256|384|512      master clock as a multiple of the sample rate
    sudo python3 fwdebug.py cpt C1 C2 C3 C4 CTL PSLOT RSLOT RATIO   whole codec port config at once (hex)
    sudo python3 fwdebug.py drift [SECONDS]       master clock error against USB frames, in ppm, at the current rate
    sudo python3 fwdebug.py lock on|off           run the clock lock, or freeze its trim
    sudo python3 fwdebug.py dma MODE [PR RR]      DMA buffers: 0 linear, 1 playback / 2 capture / 3 both wrap-around;
                                                  PR, RR: ring sizes (each of X and Y) in 8-byte units

Vendor requests (device recipient) as defined in firmware/src/main.c. On Windows (python fwdebug.py ...)
they go through the firmware's WinUSB maintenance interface (v0.19 and later, winusb.py).
"""
import ctypes
import glob
import os
import statistics
import sys
import time

VID, PID = 0x1209, 0x0001
VREQ_PEEK, VREQ_POKE, VREQ_PORTS, VREQ_SET_PORT, VREQ_CLOCK, VREQ_CODEC, VREQ_MCLK, VREQ_CPTRAW = 1, 2, 3, 4, 5, 6, 7, 8
VREQ_CLKSTAT, VREQ_LOCK, VREQ_DMAMODE = 9, 10, 11


class CtrlTransfer(ctypes.Structure):
    _fields_ = [("bRequestType", ctypes.c_uint8), ("bRequest", ctypes.c_uint8),
                ("wValue", ctypes.c_uint16), ("wIndex", ctypes.c_uint16), ("wLength", ctypes.c_uint16),
                ("timeout", ctypes.c_uint32), ("data", ctypes.c_void_p)]


USBDEVFS_CONTROL = (3 << 30) | (ctypes.sizeof(CtrlTransfer) << 16) | (ord("U") << 8)


def find_device():
    for path in glob.glob("/sys/bus/usb/devices/*"):
        try:
            ids = (int(open(f"{path}/idVendor").read(), 16), int(open(f"{path}/idProduct").read(), 16))
            if ids == (VID, PID):
                bus, dev = int(open(f"{path}/busnum").read()), int(open(f"{path}/devnum").read())
                return f"/dev/bus/usb/{bus:03d}/{dev:03d}"
        except OSError:
            continue
    sys.exit("the Mbox with the open firmware (1209:0001) isn't on USB")


class Mbox:
    """The firmware's vendor requests: through usbfs on Linux, through WinUSB on Windows."""

    def __init__(self):
        if sys.platform == "win32":
            import winusb
            self.win = winusb.open_mbox()
        else:
            self.win = None
            self.fd = os.open(find_device(), os.O_RDWR)

    def control(self, request_type, request, value=0, index=0, length=0, data=b""):
        if data:
            length = len(data)
            # Firmware up to v0.17 can drop a control write's data if it follows a control read too
            # closely (a stale endpoint-0 interrupt is taken for the data); a short pause avoids it.
            time.sleep(0.03)
        if self.win:
            return self.win.control(request_type, request, value, index, length, data)
        import fcntl
        buf = (ctypes.c_uint8 * max(length, 1))(*data)
        xfer = CtrlTransfer(request_type, request, value, index, length, 1000, ctypes.cast(buf, ctypes.c_void_p))
        count = fcntl.ioctl(self.fd, USBDEVFS_CONTROL, xfer)
        return bytes(buf[:count]) if request_type & 0x80 else b""

    def peek(self, address, length=1):
        return self.control(0xC0, VREQ_PEEK, address, 0, length)

    def poke(self, address, value):
        self.control(0x40, VREQ_POKE, address, value)


REGISTERS = [  # (name, address)
    ("USBCTL", 0xFFFC), ("USBFADR", 0xFFFF), ("GLOBCTL", 0xFFB1), ("MEMCFG", 0xFFB0),
    ("ACGFRQ2", 0xFFE5), ("ACGFRQ1", 0xFFE6), ("ACGFRQ0", 0xFFE7), ("ACGDCTL", 0xFFE2), ("ACGCTL", 0xFFE1),
    ("CPTCNF1", 0xFFE0), ("CPTCNF2", 0xFFDF), ("CPTCNF3", 0xFFDE), ("CPTCNF4", 0xFFDD), ("CPTCTL", 0xFFDC),
    ("DMACTL0", 0xFFE8), ("DMATSH0", 0xFFE9), ("DMATSL0", 0xFFEA),
    ("DMACTL1", 0xFFEE), ("DMATSH1", 0xFFEF), ("DMATSL1", 0xFFF0),
    ("DMACTL2", 0xFFF4), ("DMATSH2", 0xFFF5), ("DMATSL2", 0xFFF6),
    ("OEPCNF1", 0xFFA0), ("OEPDCNTX1", 0xFFA3), ("OEPDCNTY1", 0xFFA7),
    ("IEPCNF2", 0xFF58), ("IEPDCNTX2", 0xFF5B), ("IEPDCNTY2", 0xFF5F),
    ("OEPBSIZ1", 0xFFA2), ("IEPBSIZ2", 0xFF5A),
]


def count_text(byte):
    return f"{byte & 0x7F:3d}{'N' if byte & 0x80 else ' '}"


def regs(mbox):
    for name, address in REGISTERS:
        print(f"{name:10s} {address:04X}h = {mbox.peek(address)[0]:02x}")


def watch(mbox, samples):
    print("frame  MCLK/frame  EP1-out X  Y  ovf   EP2-in X  Y   (count, N = NACK set)")
    rates = []
    last = None
    for _ in range(samples):
        frame = int.from_bytes(mbox.peek(0xFFFA, 2), "big") & 0x7FF   # USBFNH, USBFNL
        cap = int.from_bytes(mbox.peek(0xFFE3, 2), "big")              # ACGCAPH, ACGCAPL
        out = mbox.peek(0xFFA0, 8)
        inp = mbox.peek(0xFF58, 8)
        rate = ""
        if last:
            frames = (frame - last[0]) % 2048
            if 1 <= frames <= 5:
                per_frame = ((cap - last[1]) % 65536) / frames
                rates.append(per_frame)
                rate = f"{per_frame:8.0f}"
        last = (frame, cap)
        print(f"{frame:5d}  {rate:>10s}   {count_text(out[3])} {count_text(out[7])} {'Y' if out[0] & 0x20 else '-'}"
              f"     {count_text(inp[3])} {count_text(inp[7])}")
        time.sleep(0.003)
    if rates:
        mhz = statistics.median(rates) / 1000
        print(f"MCLK about {mhz:.3f} MHz (median of {len(rates)} frame-to-frame counts)")


def clock_status(mbox):
    """VREQ_CLKSTAT: frame, MCLK capture, N trim, lock phase, sample rate (Hz; firmware before v0.24 sends 8
    bytes and runs 48 kHz only) and the streaming interfaces (bit 0 playback, bit 1 capture; None before v0.24)."""
    r = mbox.control(0xC0, VREQ_CLKSTAT, 0, 0, 12)
    return dict(frame=(r[0] | r[1] << 8) & 0x7FF, capture=r[2] | r[3] << 8,
                trim=int.from_bytes(r[4:6], "little", signed=True), phase=int.from_bytes(r[6:8], "little", signed=True),
                rate=int.from_bytes(r[8:11], "little") if len(r) >= 11 else 48000, streams=r[11] if len(r) >= 12 else None)


def drift(mbox, seconds, nominal=None):
    """MCLK cycles per USB frame from two (frame, capture) readings `seconds` apart, unwrapped. The nominal
    count is 256 x the sample rate the firmware reports: 12288 at 48 kHz, 11289.6 at 44.1 kHz."""
    def sample():
        s = clock_status(mbox)
        sample.lock = (s["trim"], s["phase"])
        sample.rate = s["rate"]
        return s["frame"], s["capture"], time.monotonic()
    f0, c0, t0 = sample()
    time.sleep(seconds)
    f1, c1, t1 = sample()
    rate = sample.rate
    nominal = nominal or 256 * rate / 1000
    frames = round((t1 - t0) * 1000)
    frames += ((f1 - f0) - frames + 1024) % 2048 - 1024             # exact frame count near the wall-clock estimate
    expected = round(frames * nominal)
    counts = expected + ((c1 - c0 - expected + 32768) % 65536) - 32768
    per_frame = counts / frames
    ppm = (per_frame / nominal - 1) * 1e6
    print(f"{frames} frames at {rate} Hz: {per_frame:.3f} MCLK cycles per frame (nominal {nominal:g}): {ppm:+.1f} ppm "
          f"= {per_frame * 1000 / 1e6:.6f} MHz; a stream drifts {ppm * rate / 1e6:+.2f} samples/s"
          f"   [lock: N trim {sample.lock[0]:+d} LSB, phase {sample.lock[1]:+d} cycles]")


def main():
    if len(sys.argv) < 2:
        sys.exit(__doc__)
    mbox = Mbox()
    command, args = sys.argv[1], sys.argv[2:]
    if command == "regs":
        regs(mbox)
    elif command == "watch":
        watch(mbox, int(args[0]) if args else 20)
    elif command == "peek":
        data = mbox.peek(int(args[0], 16), int(args[1]) if len(args) > 1 else 1)
        print(data.hex(" "))
    elif command == "poke":
        mbox.poke(int(args[0], 16), int(args[1], 16))
    elif command == "ports":
        p1, p3, master = mbox.control(0xC0, VREQ_PORTS, 0, 0, 3)
        print(f"P1 = {p1:02x} ({p1:08b})  P3 = {p3:02x} ({p3:08b})  codec port clock master: {bool(master)}")
    elif command == "setport":
        mbox.control(0x40, VREQ_SET_PORT, int(args[1], 16), int(args[0]))
    elif command == "clock":
        mbox.control(0x40, VREQ_CLOCK, 1 if args and args[0] == "master" else 0)
    elif command == "codec":
        mbox.control(0x40, VREQ_CODEC, int(args[0]), int(args[1]) if len(args) > 1 else 0)
    elif command == "cpt":
        mbox.control(0x40, VREQ_CPTRAW, data=bytes(int(a, 16) for a in args[:8]))
    elif command == "lock":
        mbox.control(0x40, VREQ_LOCK, 1 if args and args[0] == "on" else 0)
    elif command == "drift":
        for _ in range(3):
            drift(mbox, float(args[0]) if args else 2.0)
    elif command == "dma":
        rings = (int(args[1]) if len(args) > 1 else 0) | (int(args[2]) if len(args) > 2 else 0) << 8
        mbox.control(0x40, VREQ_DMAMODE, int(args[0]), rings)
    elif command == "mclk":
        mbox.control(0x40, VREQ_MCLK, {"256": 0, "384": 1, "512": 2}[args[0]])
    else:
        sys.exit(__doc__)


if __name__ == "__main__":
    main()
