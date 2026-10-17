#!/bin/sh
# Builds the Mbox 2 Mini open firmware with SDCC and wraps it in TI's EEPROM header.
# Runs on Linux, e.g. a Raspberry Pi (sdcc from Debian); tools/firmware/build.ps1 does the same on Windows.
#   sh build.sh [version]
# BOOT_CPT=0x08,0x25,... (the 8 bytes of a fwdebug.py cpt request) bakes in a codec port setting as
# the boot default: general-purpose mode can't be switched to at run time (see audio.c).
# BOOT_DMA_MODE=0..3 sets the DMA buffer mode at boot (audio.h DMA_WAB_*: 1 playback, 2 capture wrap-around).
set -e
cd "$(dirname "$0")"
VERSION=${1:-1}
CFLAGS="-mmcs51 --model-small --opt-code-size${BOOT_CPT:+ -DBOOT_CPT=$BOOT_CPT}${BOOT_DMA_MODE:+ -DBOOT_DMA_MODE=$BOOT_DMA_MODE}"
rm -rf out
mkdir -p out
for f in src/*.c; do
    sdcc $CFLAGS -c "$f" -o out/
done
# main.rel first: it holds main(). No external RAM exists outside the chip's 2 KB register/buffer window.
sdcc $CFLAGS --code-size 0x2000 --iram-size 256 --xram-size 0 out/main.rel out/descriptors.rel out/audio.rel out/diag.rel -o out/mbox2fw.ihx
sed -n '/Internal RAM layout/,$p' out/mbox2fw.mem 2>/dev/null | tail -n 12 || true
python3 mkimage.py out/mbox2fw.ihx out/mbox2-open.bin --version "$VERSION"
