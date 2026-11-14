#!/bin/bash
# Installs the Mbox 2 Mini open firmware through Avid's own USB update (DFU) mode: no wires, no soldering.
# EXPERIMENTAL AND UNTESTED: read README.md first.
#
#   sudo ./install.sh              with fwupdtool (fwupd)
#   sudo ./install.sh --dfu-util   with dfu-util instead
#   sudo ./install.sh --header-255 the second file (README.md: only if the first was refused)
#
# Everything it prints also goes to mbox2-dfu-<time>.log in this folder.
set -u
here=$(cd "$(dirname "$0")" && pwd)
tool=fwupdtool
image="$here/mbox2-open.dfu"
for arg in "$@"; do
    case "$arg" in
        --dfu-util) tool=dfu-util ;;
        --fwupd) tool=fwupdtool ;;
        --header-255) image="$here/mbox2-open-hdr255.dfu" ;;
        -h|--help) sed -n '2,9p' "$0"; exit 0 ;;
        *) echo "Unknown option: $arg (see --help)"; exit 2 ;;
    esac
done

log="$here/mbox2-dfu-$(date +%Y%m%d-%H%M%S).log"
exec > >(tee -a "$log") 2>&1
echo "Mbox 2 Mini open firmware: install through Avid's update mode ($(date))"
echo "tool: $tool, file: $(basename "$image"), $(uname -srm)"

fail() { echo; echo "STOPPED: $*"; echo "Log: $log"; exit 1; }

[ "$(id -u)" -eq 0 ] || fail "run it with sudo"
command -v lsusb >/dev/null || fail "needs lsusb (package usbutils)"
command -v "$tool" >/dev/null || fail "needs $tool (package $([ "$tool" = dfu-util ] && echo dfu-util || echo fwupd))"
[ -f "$image" ] || fail "missing $(basename "$image")"
if command -v sha256sum >/dev/null && [ -f "$here/SHA256SUMS" ]; then
    (cd "$here" && sha256sum --quiet -c SHA256SUMS) || fail "a file doesn't match SHA256SUMS: download the bundle again"
fi

# 1. Exactly one Mbox, on Avid's firmware. Whether it's a Mini only the user can tell (step 2): the full-size
# Mbox 2 runs the same firmware with the same USB ID.
if lsusb -d 1209:0001 >/dev/null 2>&1; then
    lsusb -d 1209:0001
    fail "an Mbox already runs the open firmware: update it over USB instead (README.md)"
fi
lsusb -d 0451:3200 >/dev/null 2>&1 && fail "a TUSB3200 is in TI's boot mode (0451:3200): its firmware chip doesn't load; only the wired install fixes that"
count=$(lsusb -d 0dba:3000 2>/dev/null | wc -l)
[ "$count" -ge 1 ] || fail "no Mbox 2 on Avid's firmware (USB ID 0dba:3000) found. Plug it in directly, not through a hub"
[ "$count" -eq 1 ] || fail "$count Mbox 2s are plugged in: leave only the one to update"
echo
lsusb -v -d 0dba:3000 2>/dev/null | grep -E "idVendor|idProduct|bcdDevice|iManufacturer|iProduct|bInterfaceClass|bInterfaceSubClass|bInterfaceProtocol" | sed 's/^/  /'
bcd=$(lsusb -v -d 0dba:3000 2>/dev/null | awk '/bcdDevice/ {print $2; exit}')
echo "Avid firmware version: $bcd"
if [ "$bcd" != "1.40" ]; then
    echo "Note: the one Mbox 2 Mini checked so far had Avid firmware 1.40. Every Avid version has the same update"
    echo "mode, so this one should install the same way. But the full-size Mbox 2 runs the same firmware (1.30-1.43)"
    echo "with the same USB ID, so nothing over USB tells the two apart: make sure yours is the Mini."
fi

# 2. The go-ahead.
cat <<'EOF'

This firmware is for the Mbox 2 Mini only (the small one: no MIDI or S/PDIF ports), not the full-size Mbox 2.
It writes new firmware into the Mbox through Avid's update mode, and it has NOT been tested on a real Mbox
yet. If Avid's update mode takes the file but the result doesn't start, the Mbox stops working until its
firmware chip is rewritten with the wired install (two soldered wires and a Raspberry Pi). Don't unplug
the Mbox until this script says it's done.
EOF
printf '\nType YES if this is a Mbox 2 Mini and it should be installed: '
read -r answer < /dev/tty
[ "$answer" = YES ] || fail "not installed"

# 3. Install.
echo
case "$tool" in
dfu-util)
    dfu-util -l
    echo
    # Detach from Avid's runtime (0dba:3000), wait for update mode (0dba:3001), download, reset.
    dfu-util -d 0dba:3000,0dba:3001 -a 0 -D "$image" -R
    status=$?
    ;;
fwupdtool)
    quirk=/var/lib/fwupd/quirks.d/mbox2-dfu.quirk
    mkdir -p /var/lib/fwupd/quirks.d && cp "$here/mbox2-dfu.quirk" "$quirk" || fail "couldn't install the quirk file"
    daemon=no
    if systemctl is-active --quiet fwupd 2>/dev/null; then
        daemon=yes
        systemctl stop fwupd
    fi
    fwupdtool get-devices --plugins dfu
    echo
    # Choose the Mbox 2 if fwupdtool asks which device.
    fwupdtool install-blob --plugins dfu "$image"
    status=$?
    rm -f "$quirk"
    [ "$daemon" = yes ] && systemctl start fwupd
    ;;
esac
echo
echo "$tool exit status: $status"

# 4. Did it come back as the open firmware?
echo "Waiting up to 30 s for the Mbox to restart..."
for _ in $(seq 30); do
    lsusb -d 1209:0001 >/dev/null 2>&1 && break
    sleep 1
done
echo
if lsusb -d 1209:0001 >/dev/null 2>&1; then
    lsusb -v -d 1209:0001 2>/dev/null | grep -E "idVendor|idProduct|bcdDevice|iManufacturer|iProduct" | sed 's/^/  /'
    echo
    echo "DONE: the Mbox runs the open firmware. Unplug it, wait 15 s, and plug it back in before listening."
elif lsusb -d 0dba:3000 >/dev/null 2>&1; then
    echo "The Mbox is back on Avid's firmware: the update didn't take. It still works as before."
    echo "Please send the log. README.md says when to try --header-255."
elif lsusb -d 0dba:3001 >/dev/null 2>&1; then
    echo "The Mbox is still in Avid's update mode (0dba:3001). Unplug it, wait 15 s, plug it back in, and run"
    echo "lsusb to see which firmware it starts. Please send the log and that lsusb output."
elif lsusb -d 0451:3200 >/dev/null 2>&1; then
    echo "The Mbox is in TI's boot mode (0451:3200): its firmware chip no longer holds a startable firmware."
    echo "Only the wired install brings it back. Please send the log."
else
    echo "The Mbox doesn't show up. Unplug it, wait 15 s, plug it back in, and run lsusb. Please send the log."
fi
echo "Log: $log"
