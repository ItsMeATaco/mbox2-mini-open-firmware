#!/usr/bin/env python3
"""Builds the experimental no-wires install bundle: the open firmware packaged for Avid's own USB update
(DFU) mode on a stock Mbox 2 Mini, for testers with fwupdtool or dfu-util on Linux, or with
mbox2-firstinstall.exe (src/firstinstall) on Windows.

    python tools\\release\\package-dfu-test.py [IMAGE]

IMAGE defaults to build\\firmware\\mbox2-open.bin (tools\\firmware\\build.ps1's output). Builds
mbox2-firstinstall.exe in the everyday build tree (build\\) first. Writes dist\\dfu-test-v<version>\\ and
dist\\Mbox2Mini-DFU-test-v<version>.zip, with install.sh executable in the zip. The bundle's install.sh,
README.md and fwupd quirk come from tools\\release\\dfu-test\\.
"""
import hashlib
import os
import shutil
import subprocess
import sys
import zipfile

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
SRC = os.path.join(ROOT, "tools", "release", "dfu-test")
MKDFU = os.path.join(ROOT, "firmware", "mkdfu.py")


def main():
    image = os.path.abspath(sys.argv[1] if len(sys.argv) > 1 else os.path.join(ROOT, "build", "firmware", "mbox2-open.bin"))
    header = open(image, "rb").read(12)
    version = f"v0.{header[5]:02d}" if header[5] < 100 else f"v{header[5] // 100}.{header[5] % 100:02d}"
    out = os.path.join(ROOT, "dist", f"dfu-test-{version}")
    if os.path.exists(out):
        shutil.rmtree(out)
    os.makedirs(out)

    for name, extra in (("mbox2-open.dfu", []), ("mbox2-open-hdr255.dfu", ["--header-version", "255"])):
        subprocess.run([sys.executable, MKDFU, image, os.path.join(out, name), *extra], check=True)
    subprocess.run(["cmake", "--build", os.path.join(ROOT, "build"), "--config", "Release", "--target",
                    "mbox2-firstinstall"], check=True, stdout=subprocess.DEVNULL)
    os.makedirs(os.path.join(out, "windows"))
    shutil.copyfile(os.path.join(ROOT, "build", "src", "firstinstall", "Release", "mbox2-firstinstall.exe"),
                    os.path.join(out, "windows", "mbox2-firstinstall.exe"))
    shutil.copyfile(image, os.path.join(out, "windows", "mbox2-open.bin"))
    shutil.copyfile(os.path.join(ROOT, "tools", "setup", "remove-avid-leftovers.ps1"),
                    os.path.join(out, "windows", "remove-avid-leftovers.ps1"))
    for name in ("install.sh", "mbox2-dfu.quirk"):
        data = open(os.path.join(SRC, name), "rb").read().replace(b"\r\n", b"\n")   # Linux line endings
        open(os.path.join(out, name), "wb").write(data)
    readme = open(os.path.join(SRC, "README.md"), encoding="utf-8").read().replace("{version}", version)
    open(os.path.join(out, "README.md"), "w", encoding="utf-8", newline="\n").write(readme)

    names = ["mbox2-open.dfu", "mbox2-open-hdr255.dfu", "mbox2-dfu.quirk", "install.sh",
             "windows/mbox2-firstinstall.exe", "windows/mbox2-open.bin", "windows/remove-avid-leftovers.ps1",
             "README.md"]
    sums = "".join(f"{hashlib.sha256(open(os.path.join(out, n), 'rb').read()).hexdigest()}  {n}\n"
                   for n in names if n != "README.md")
    open(os.path.join(out, "SHA256SUMS"), "w", newline="\n").write(sums)

    folder = f"Mbox2Mini-DFU-test-{version}"
    zpath = os.path.join(ROOT, "dist", f"{folder}.zip")
    with zipfile.ZipFile(zpath, "w", zipfile.ZIP_DEFLATED) as z:
        for n in names + ["SHA256SUMS"]:
            info = zipfile.ZipInfo(f"{folder}/{n}", date_time=(2026, 1, 1, 0, 0, 0))
            info.create_system = 3                          # Unix, so unzip applies the mode bits
            info.external_attr = (0o100755 if n == "install.sh" else 0o100644) << 16
            info.compress_type = zipfile.ZIP_DEFLATED
            z.writestr(info, open(os.path.join(out, n), "rb").read())
    print(f"\n{out}\n{zpath} ({os.path.getsize(zpath):,} bytes)")
    print(sums, end="")


if __name__ == "__main__":
    main()
