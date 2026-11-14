# Mbox 2 Mini open firmware {version}: first install without wires (EXPERIMENTAL)

This bundle installs the open firmware on a **stock Mbox 2 Mini** (still on Avid's firmware) from Linux
or Windows, through the USB update mode that Avid's own firmware has. No case opening, no soldering, no
Raspberry Pi wiring.

## Status: untested

**Nobody has tried this on a real Mbox yet.** You'd be the first. What's known:

- Avid's firmware has a standard USB DFU (Device Firmware Upgrade) interface. That's how Avid's own
  updater installed new firmware. Its runtime ID is 0dba:3000 and its update-mode ID 0dba:3001. It can
  only download, in 8-byte blocks.
- Avid's firmware files are plain TUSB3200 EEPROM images, the same format as the open firmware's
  `mbox2-open.bin`. This bundle sends that image unchanged. On Linux it carries the standard DFU suffix
  that fwupdtool and dfu-util read and strip. On Windows, `mbox2-firstinstall.exe` sends it.

What isn't known is whether Avid's update mode accepts an image that isn't Avid's. It may check the
version or the contents. So there are three possible outcomes:

| Outcome | What you see | Then |
|---|---|---|
| It works | The Mbox restarts as "Mbox 2 Mini", USB ID 1209:0001 | Use it (below) |
| Avid's update mode refuses the file | An error, and the Mbox comes back on Avid's firmware (0dba:3000) and works as before | Send the log; maybe try the second file (below) |
| It takes the file but the result doesn't start | The Mbox shows up as 0451:3200 (TI's boot mode), or not at all | Only the wired install (two soldered wires and a Raspberry Pi, see the main README) brings it back |

**Only try this if you could live with the third outcome.**

## What you need

- A Mbox 2 **Mini**: the small one, with no MIDI or S/PDIF ports. This firmware isn't for the
  full-size Mbox 2, but over USB the two look the same: the same USB ID and the same Avid firmware.
  So the installers can't tell them apart, and they ask you to confirm it's a Mini.
  - **Any Avid firmware version should do.** The one Mini checked so far had 1.40, but every Avid
    version (1.30 to 1.43) has the same update mode.
  - **The installers show the version they find**, and add a note when it isn't 1.40. Please mention
    the version when you report back.
- One of these:
  - a Linux PC, or a Raspberry Pi, with `usbutils` and `fwupd` (for fwupdtool) or `dfu-util`. On
    Debian, Ubuntu or Raspberry Pi OS: `sudo apt install usbutils fwupd dfu-util`;
  - a Windows 10 or 11 PC, with an administrator account.
- The Mbox plugged **directly** into the computer (no hub), and left plugged in until the installer
  says it's done.

## Install on Linux

1. Unzip the bundle and open a terminal in its folder.
2. Run one of these:
   - `sudo ./install.sh` uses fwupdtool;
   - `sudo ./install.sh --dfu-util` uses dfu-util.

   The script does three things:
   - checks the files against `SHA256SUMS`;
   - checks that exactly one Mbox 2 on Avid's firmware is plugged in, and shows its version;
   - asks you to type YES, confirming it's a Mini, before writing anything.

   It then installs, waits for the Mbox to restart, and tells you which firmware it came back with.
   Everything goes to a `mbox2-dfu-<time>.log` file in the same folder.
3. If it says DONE: unplug the Mbox, wait 15 s, and plug it back in. It's now a standard USB audio
   device ("Mbox 2 Mini") that works with no driver on Windows 10/11, Linux and macOS.

### With fwupdtool

Before installing, the script does two things:
- copies `mbox2-dfu.quirk` to `/var/lib/fwupd/quirks.d/`, which tells fwupd that 0dba:3000 and 0dba:3001
  are the same device;
- stops the fwupd service.

Afterwards it removes the quirk and restarts the service. If fwupdtool asks which device, choose the
Mbox 2.

fwupdtool may report an error at the very end, because the Mbox comes back with a different USB ID
(1209:0001) than the one it was waiting for. The script's own check after that is what counts.

### By hand, without the script

- **dfu-util:**
  - `sudo dfu-util -l` shows the Mbox's update interface;
  - then `sudo dfu-util -d 0dba:3000,0dba:3001 -a 0 -D mbox2-open.dfu -R`.
- **fwupdtool:**
  - copy `mbox2-dfu.quirk` into `/var/lib/fwupd/quirks.d/`;
  - then `sudo fwupdtool install-blob --plugins dfu mbox2-open.dfu`.

## Install on Windows

Windows has no driver for Avid's update mode. So the installer has you attach Microsoft's own WinUSB
driver to it in Device Manager, using the procedure Microsoft documents for devices like this. WinUSB
is part of Windows and signed by Microsoft. Nothing else gets installed: no certificate, no third-party
driver.

**Before you start, if Avid's Mbox 2 driver was ever installed on this PC,** remove it. Otherwise
Windows puts Avid's update driver on the Mbox's update mode, and Device Manager won't offer WinUSB for
it. The installer checks for this and stops, with these steps, if it finds Avid's driver:
1. Settings > Apps > Installed apps: uninstall "Avid Mbox 2 USB Drivers" (on older PCs it may be
   called "Digidesign Mbox 2").
2. Unplug the Mbox. In the bundle's `windows` folder, right-click `remove-avid-leftovers.ps1` > Run with
   PowerShell. Press Enter when it has listed what it found.
3. Restart Windows.

Avid's driver doesn't load with Memory Integrity on anyway, and the open firmware doesn't use it.

1. Unzip the bundle. In its `windows` folder, run `mbox2-firstinstall.exe`. It asks for administrator
   rights. Windows SmartScreen may warn about an unrecognised program: choose More info, then Run
   anyway.
2. It checks three things:
   - the firmware file;
   - that exactly one Mbox 2 on Avid's firmware is plugged in, and shows its version;
   - that Avid's driver is gone.

   Then it asks you to type YES, confirming it's a Mini.
3. **First driver step.** It opens Device Manager's window for the Mbox's update interface and lists
   the clicks:
   1. Driver tab > Update driver.
   2. Browse my computer for drivers.
   3. Let me pick from a list of available drivers on my computer.
   4. Universal Serial Bus devices.
   5. WinUsb Device.
   6. Yes to Windows' compatibility warning.

   The installer carries on by itself as soon as the driver is on.
4. It switches the Mbox to update mode. The Mbox reappears as a new device (0DBA:3001).
5. **Second driver step:** the same clicks once more, for update mode.
6. It writes the firmware, with progress shown. If a write fails while the Mbox is still in update mode,
   it offers to start the write again from the beginning.
7. It asks you to unplug the Mbox, wait 15 s and plug it back in. Then it tells you which firmware
   came back.

Everything goes to a `mbox2-firstinstall-<time>.log` file beside the program.

The WinUSB driver stays attached to those two Avid update-mode devices. That does no harm. To remove it,
use Device Manager > View > Show hidden devices, then uninstall them.

## If Avid's update mode refused the file

`mbox2-open-hdr255.dfu` is the same firmware with one byte changed: the image header's version byte is
set to 255. The normal file carries 27 (for {version}), lower than any Avid version (130-143), in case
Avid's update mode only accepts newer versions. The TI chip itself ignores that byte.

Try it **only if** the first attempt ended with the Mbox back on Avid's firmware, still working (`lsusb`
shows 0dba:3000, or Mbox Hub and Device Manager show the Avid Mbox 2):
- **Linux:** `sudo ./install.sh --header-255`;
- **Windows:** in a command prompt opened as administrator, in the `windows` folder, run
  `mbox2-firstinstall.exe --header-255`.

## Please send back

- the `mbox2-dfu-*.log` (Linux) or `mbox2-firstinstall-*.log` (Windows) file(s);
- the output of `lsusb` after unplugging and replugging the Mbox;
- if the Mbox shows up at all, `lsusb -v -d <its ID>` (Linux), or what Device Manager shows for it
  (Windows).

## After it works

Later updates go over USB through the open firmware itself:
- **Windows:** Mbox Hub's Install button;
- **Linux:** `sudo python3 tools/firmware/usbeeprom.py write mbox2-open.bin` from the repository.

There's no way back to Avid's firmware without a copy of an Avid image (Avid's update mode can't read
the old one out first), so keep that in mind too.

## Files

| File | What |
|---|---|
| `mbox2-open.dfu` | the open firmware {version} image (byte-identical to the release's `mbox2-open.bin`) plus the DFU suffix |
| `mbox2-open-hdr255.dfu` | the same with the header version byte 255 (see above) |
| `mbox2-dfu.quirk` | the fwupd quirk |
| `install.sh` | the Linux installer described above |
| `windows/mbox2-firstinstall.exe` | the Windows installer described above |
| `windows/mbox2-open.bin` | the open firmware {version} image the Windows installer writes (byte-identical to the release's) |
| `windows/remove-avid-leftovers.ps1` | removes what Avid's driver leaves after its uninstaller (see "Before you start") |
| `SHA256SUMS` | checksums of all of the above |
