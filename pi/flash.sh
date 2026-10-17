#!/usr/bin/env bash
# Installs the open firmware on an Mbox 2 Mini from a Raspberry Pi wired to its firmware chip (U24).
#
#   curl -fsSL https://raw.githubusercontent.com/ItsMeATaco/mbox2-mini-open-firmware/main/pi/flash.sh | sudo bash
#
# What it does: turns on the Pi's I2C bus, checks that the chip answers, backs the chip up into your home
# folder (two full majority-voted reads that must match), writes the firmware and verifies the whole chip,
# then waits for you to replug the Mbox and checks which firmware it starts.
set -euo pipefail

REPO=ItsMeATaco/mbox2-mini-open-firmware
RAW=https://raw.githubusercontent.com/$REPO/main/pi
FIRMWARE_URL=https://github.com/$REPO/releases/latest/download/mbox2-open.bin

usage() {
  cat <<'EOF'
Options (after "sudo bash -s --" when piped from curl, or after "sudo bash pi/flash.sh" in a clone):
  --scan            only check the wiring
  --image FILE      write FILE instead of the latest release's firmware
  --restore FILE    write a backup made by this script back (e.g. Avid's original firmware)
EOF
  exit 0
}

bold=$'\e[1m' green=$'\e[32m' yellow=$'\e[33m' red=$'\e[31m' reset=$'\e[0m'
step() { echo; echo "${bold}[$1/5] $2${reset}"; }
info() { echo "      $*"; }
warn() { echo "      ${yellow}$*${reset}"; }
fail() { echo; echo "${red}${bold}$*${reset}" >&2; exit 1; }

# Backups go to the home folder of whoever ran sudo, owned by them.
owner=${SUDO_USER:-root}
home=$(getent passwd "$owner" | cut -d: -f6)
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT

# How the Mbox shows up on the Pi's USB: "open <bcdDevice>", "avid", "rom" or "none".
mbox_state() {
  local d id
  for d in /sys/bus/usb/devices/*; do
    [[ -f $d/idVendor ]] || continue
    id="$(<"$d/idVendor"):$(<"$d/idProduct")"
    case $id in
      1209:0001) grep -qi mbox "$d/product" 2>/dev/null && { echo "open $(<"$d/bcdDevice")"; return; } ;;
      0dba:3000) echo avid; return ;;
      0451:3200) echo rom; return ;;
    esac
  done
  echo none
}

describe() {
  local bcd
  case $1 in
    open*) bcd=${1#open }; echo "the open firmware v$((10#${bcd:0:2})).${bcd:2:2}" ;;
    avid) echo "Avid's firmware" ;;
    rom) echo "TI's boot-ROM mode (0451:3200), which means it found no valid firmware in the chip" ;;
    none) echo "not found" ;;
  esac
}

# What a TUSB3200 image holds, from its USB device descriptor and header version byte:
# "open 0.25", "avid 1.40" or "unknown".
image_kind() {
  python3 - "$1" <<'EOF'
import sys
d = open(sys.argv[1], "rb").read()
kind = "open" if b"\x09\x12\x01\x00" in d else "avid" if b"\xba\x0d\x00\x30" in d else "unknown"
print(kind, f"{d[5] // 100}.{d[5] % 100:02d}" if len(d) > 5 else "")
EOF
}

main() {
  mode=install image=""
  while [[ $# -gt 0 ]]; do
    case "$1" in
      --scan) mode=scan ;;
      --image) mode=install; image=${2:?--image needs a file}; shift ;;
      --restore) mode=restore; image=${2:?--restore needs a file}; shift ;;
      -h|--help) usage ;;
      *) fail "Unknown option: $1 (try --help)" ;;
    esac
    shift
  done

  [[ $EUID -eq 0 ]] || fail "Run it with sudo: it needs the I2C bus."
  if [[ -n $image ]]; then
    [[ -f $image ]] || fail "No such file: $image"
    image=$(realpath "$image")
  fi

  echo "${bold}Mbox 2 Mini open firmware: Raspberry Pi installer${reset}"

  step 1 "Turning on the Pi's I2C bus"
  command -v python3 >/dev/null || fail "python3 is missing (it comes with Raspberry Pi OS): sudo apt install python3"
  if [[ ! -e /dev/i2c-1 ]]; then
    command -v raspi-config >/dev/null || fail "raspi-config not found: is this Raspberry Pi OS?"
    raspi-config nonint do_i2c 0
    modprobe i2c-dev || true
  fi
  [[ -e /dev/i2c-1 ]] || fail "I2C didn't come up (/dev/i2c-1 is missing). Reboot the Pi (sudo reboot) and run the command again."
  info "ok"

  # The Python helpers: next to this script in a clone of the repository, else from GitHub.
  # (Piped from curl, BASH_SOURCE holds "main", not a file.)
  here=""
  if [[ ${BASH_SOURCE[0]:-} == *.sh && -f ${BASH_SOURCE[0]} ]]; then here=$(dirname "$(realpath "${BASH_SOURCE[0]}")"); fi
  for f in eeprom_dump.py eeprom_write.py; do
    if [[ -n $here && -f $here/$f ]]; then
      cp "$here/$f" "$work/"
    else
      curl -fsSL -o "$work/$f" "$RAW/$f" || fail "Couldn't download $f from GitHub. Is the Pi online?"
    fi
  done

  step 2 "Looking for the Mbox and its firmware chip"
  before=$(mbox_state)
  info "Mbox on the Pi's USB: $(describe "$before")"
  # Keep Linux's USB audio driver away from the Mbox while we work on it.
  modprobe -r snd_usb_audio 2>/dev/null || true
  scan=$(cd "$work" && python3 eeprom_dump.py --scan)
  if ! grep -q 0x50 <<<"$scan"; then
    echo
    echo "${red}${bold}The firmware chip (U24) doesn't answer.${reset}"
    if [[ $before == none ]]; then
      echo "The Mbox isn't on the Pi's USB. Plug its USB cable into one of the Pi's USB ports: that powers"
      echo "the chip and connects the grounds."
    elif command -v pinctrl >/dev/null && [[ "$(pinctrl lev 2)$(pinctrl lev 3)" == 00 ]]; then
      echo "Both wires read low, so they're on grounded pins: U24's row next to the big TI chip is all"
      echo "ground. SDA and SCL are on the other row (see the photo in the README)."
    else
      echo "Check that the wire from U24's SDA pin goes to Pi pin 3 and the one from SCL to pin 5 (swapped"
      echo "wires look fine but nothing answers), then check the solder joints for gaps or bridges."
    fi
    echo "Shut the Pi down (sudo poweroff) before touching the wires."
    exit 1
  fi
  info "The firmware chip answers at I2C address 0x50"
  if [[ $mode == scan ]]; then
    echo; echo "${green}${bold}Wiring OK.${reset} Run the command without --scan to install the firmware."
    exit 0
  fi

  if [[ -z $image ]]; then
    image=$work/mbox2-open.bin
    curl -fsSL -o "$image" "$FIRMWARE_URL" || fail "Couldn't download the firmware ($FIRMWARE_URL). Is the Pi online?"
  fi

  step 3 "Backing up the chip (two full reads that must match)"
  read_twice() {
    local rc1=0 rc2=0
    rm -f "$work/a.bin" "$work/b.bin"
    # Exit code 2 = read fine, but the chip holds no valid image (blank, or an interrupted write).
    (cd "$work" && python3 eeprom_dump.py --out a.bin >dump.log 2>&1) || rc1=$?
    (cd "$work" && python3 eeprom_dump.py --out b.bin >/dev/null 2>&1) || rc2=$?
    [[ $rc1 != 1 && $rc2 != 1 ]] && cmp -s "$work/a.bin" "$work/b.bin"
  }
  if ! read_twice; then
    warn "The two reads differed; trying again ..."
    read_twice || fail "The reads keep differing: a wire has a weak contact. Shut the Pi down (sudo poweroff), redo the solder joints and try again."
  fi
  backup="$home/mbox2-backup-$(date +%Y%m%d-%H%M%S).bin"
  cp "$work/a.bin" "$backup"
  chown "$owner": "$backup"
  info "Saved $backup"
  backup_kind=unknown
  if grep -q "checksum.*OK" "$work/dump.log"; then
    read -r kind version <<<"$(image_kind "$backup")"
    backup_kind=$kind
    case $kind in
      avid) info "It holds Avid's firmware $version." ;;
      open) info "It holds the open firmware v$version." ;;
      *) info "It holds a valid firmware image (version byte $version)." ;;
    esac
  else
    warn "The chip doesn't hold a valid firmware image (blank, or an interrupted write). Saved anyway."
  fi
  info "SHA-256 $(sha256sum "$backup" | cut -d' ' -f1)"

  read -r kind version <<<"$(image_kind "$image")"
  case $kind in
    open) step 4 "Writing the open firmware v$version and verifying the whole chip" ;;
    avid) step 4 "Writing Avid's firmware $version and verifying the whole chip" ;;
    *) step 4 "Writing $(basename "$image") and verifying the whole chip" ;;
  esac
  rc=0
  (cd "$work" && python3 eeprom_write.py "$image") 2>&1 | sed 's/^/      /' || rc=${PIPESTATUS[0]}
  case $rc in
    0) ;;
    2) fail "That file isn't a firmware image for the Mbox (bad header or checksum). Nothing was written." ;;
    3) fail "The chip ignores writes (its write-protect pin is tied high). Nothing was changed." ;;
    *) fail "Writing failed, so the chip may hold a partial image. Run the command again. Your backup is $backup." ;;
  esac

  step 5 "Checking that the Mbox starts it"
  echo "      ${bold}Unplug the Mbox's USB cable from the Pi, wait 2 seconds, and plug it back in.${reset}"
  info "Waiting (up to 3 minutes) ..."
  gone=0 after=none
  for _ in $(seq 360); do
    state=$(mbox_state)
    if [[ $state == none ]]; then
      gone=1
    elif [[ $gone == 1 ]]; then
      sleep 3
      after=$(mbox_state)
      break
    fi
    sleep 0.5
  done
  if [[ $gone == 1 && $after != none ]]; then
    info "The Mbox started $(describe "$after")."
    [[ $after == rom ]] && fail "It couldn't load the firmware. Run the command again; if that doesn't help, write your backup back with --restore."
  elif [[ $gone == 0 ]]; then
    warn "Didn't see the Mbox unplugged. The write was verified, so you can check it on your PC instead."
  else
    warn "The Mbox didn't come back on USB. Plug it in again; if it still doesn't show up, run the command again."
  fi

  host=$(hostname)
  ip=$(hostname -I 2>/dev/null | awk '{print $1}')
  echo
  echo "${green}${bold}Done.${reset}"
  echo
  echo "Next:"
  if [[ $backup_kind == avid ]]; then
    echo "  1. Copy the backup to your PC and keep it: it's your way back to Avid's firmware. In a terminal"
    echo "     on the PC (it lands in the folder the terminal is in):"
    echo "       scp $owner@$host.local:$(basename "$backup") ."
    if [[ -n $ip ]]; then echo "     (or $owner@$ip:$(basename "$backup") if the name doesn't resolve)"; fi
  else
    echo "  1. The chip didn't hold Avid's firmware this time, so keep the backup you made the first time."
  fi
  echo "  2. Shut the Pi down (sudo poweroff), unplug everything and remove the two wires."
  if [[ $mode == restore ]]; then
    echo "  3. Close the Mbox. Avid's firmware needs Avid's driver, which needs Memory Integrity off."
  else
    echo "  3. Close the Mbox and plug it into your Windows PC. From now on, firmware updates go over USB"
    echo "     from Mbox Hub, so you won't need the Pi again."
  fi
}

main "$@"
