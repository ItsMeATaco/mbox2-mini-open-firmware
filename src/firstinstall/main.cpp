// mbox2-firstinstall.exe: installs the open firmware on a stock Mbox 2 Mini from Windows, with no wires, through
// the USB update (DFU) mode of Avid's own firmware. EXPERIMENTAL and untested on a real Mbox: see the README.
//
//   mbox2-firstinstall.exe [--header-255] [--image FILE]
//
// Steps: check the image, that exactly one Mbox 2 on Avid's firmware (0DBA:3000) is present and that Avid's
// driver is gone, and have the user confirm it's a Mini (nothing over USB tells it from a full-size Mbox 2); put
// Microsoft's WinUSB driver on its DFU interface (Device Manager, guided); DFU_DETACH and a port cycle; wait for
// update mode (0DBA:3001) and put WinUSB on that too; download the image; ask for a replug and say what came back.
// Nothing is written before update mode answers. Everything goes to mbox2-firstinstall-<time>.log beside the exe.
#include <windows.h>

#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "firstinstall/dfu.h"
#include "firstinstall/usb_win.h"

namespace {

const wchar_t kRuntime[] = L"USB\\VID_0DBA&PID_3000";         // Avid's firmware, normal mode
const wchar_t kRuntimeDfu[] = L"USB\\VID_0DBA&PID_3000&MI_00";  // its DFU interface
const wchar_t kUpdate[] = L"USB\\VID_0DBA&PID_3001";          // Avid's update mode
const wchar_t kUpdateDfu[] = L"USB\\VID_0DBA&PID_3001&MI_00";
const wchar_t kOpen[] = L"USB\\VID_1209&PID_0001";            // the open firmware
const wchar_t kTiBoot[] = L"USB\\VID_0451&PID_3200";          // TI's boot ROM: the EEPROM didn't load
constexpr uint16_t kMiniRevision = 0x0140;                    // the one Mini checked so far reports 1.40
constexpr uint32_t kSignature = 0x04513200;

FILE* g_log = nullptr;

void Say(const char* format, ...) {
  char line[4096];
  va_list args;
  va_start(args, format);
  vsnprintf(line, sizeof line, format, args);
  va_end(args);
  fputs(line, stdout);
  fflush(stdout);
  if (g_log) {
    SYSTEMTIME t;
    GetLocalTime(&t);
    fprintf(g_log, "%02d:%02d:%02d.%03d %s", t.wHour, t.wMinute, t.wSecond, t.wMilliseconds, line);
    fflush(g_log);
  }
}

void LogOnly(const std::string& text) {
  if (g_log) {
    fputs(text.c_str(), g_log);
    fflush(g_log);
  }
}

std::string Hex(const std::vector<uint8_t>& data) {
  std::string out;
  char buffer[8];
  for (size_t i = 0; i < data.size(); ++i) {
    if (i % 16 == 0) {
      snprintf(buffer, sizeof buffer, "  %04zx:", i);
      out += buffer;
    }
    snprintf(buffer, sizeof buffer, " %02x", data[i]);
    out += buffer;
    if (i % 16 == 15 || i + 1 == data.size()) out += "\n";
  }
  return out;
}

std::string Line() {
  char buffer[512] = {};
  if (!fgets(buffer, sizeof buffer, stdin)) return {};
  std::string text = buffer;
  while (!text.empty() && (text.back() == '\n' || text.back() == '\r' || text.back() == ' ')) text.pop_back();
  return text;
}

bool AskYes(const char* question) {
  Say("%s Type YES and press Enter: ", question);
  std::string answer = Line();
  LogOnly("answer: " + answer + "\n");
  return answer == "YES";
}

int Finish(int code) {
  Say("\nThe log is beside this program (mbox2-firstinstall-*.log). Press Enter to close.\n");
  Line();
  if (g_log) fclose(g_log);
  return code;
}

std::wstring ExeFolder() {
  wchar_t path[MAX_PATH] = {};
  GetModuleFileNameW(nullptr, path, MAX_PATH);
  std::wstring folder = path;
  return folder.substr(0, folder.find_last_of(L"\\/"));
}

bool Elevated() {
  HANDLE token = nullptr;
  TOKEN_ELEVATION elevation{};
  DWORD size = 0;
  bool ok = OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token) &&
            GetTokenInformation(token, TokenElevation, &elevation, sizeof elevation, &size) && elevation.TokenIsElevated;
  if (token) CloseHandle(token);
  return ok;
}

std::string WindowsVersion() {
  wchar_t name[128] = {}, build[32] = {};
  DWORD ubr = 0, size = sizeof name;
  const wchar_t* key = L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion";
  RegGetValueW(HKEY_LOCAL_MACHINE, key, L"ProductName", RRF_RT_REG_SZ, nullptr, name, &size);
  size = sizeof build;
  RegGetValueW(HKEY_LOCAL_MACHINE, key, L"CurrentBuild", RRF_RT_REG_SZ, nullptr, build, &size);
  size = sizeof ubr;
  RegGetValueW(HKEY_LOCAL_MACHINE, key, L"UBR", RRF_RT_REG_DWORD, nullptr, &ubr, &size);
  return fi::Narrow(name) + " build " + fi::Narrow(build) + "." + std::to_string(ubr);
}

// The TI EEPROM image (firmware/mkimage.py): 12-byte header, then the payload.
bool LoadImage(const std::wstring& path, std::vector<uint8_t>* image, std::string* error) {
  HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
  if (file == INVALID_HANDLE_VALUE) {
    *error = "can't open " + fi::Narrow(path) + ": " + fi::Win32Text(GetLastError());
    return false;
  }
  image->resize(16384);
  DWORD got = 0;
  BOOL ok = ReadFile(file, image->data(), static_cast<DWORD>(image->size()), &got, nullptr);
  CloseHandle(file);
  if (!ok) {
    *error = "can't read " + fi::Narrow(path);
    return false;
  }
  image->resize(got);
  const auto& b = *image;
  if (b.size() < 12 || (b[0] | b[1] << 8 | b[2] << 16 | static_cast<uint32_t>(b[3]) << 24) != kSignature || b[4] != 12) {
    *error = "not a TUSB3200 EEPROM image";
    return false;
  }
  const size_t payload = b[8] | b[9] << 8;
  const uint16_t checksum = static_cast<uint16_t>(b[10] | b[11] << 8);
  uint16_t sum = 0;
  for (size_t i = 12; i < b.size(); ++i) sum = static_cast<uint16_t>(sum + b[i]);
  if (payload + 12 != b.size() || sum != checksum || b.size() > 8192) {
    *error = "the image's size or checksum doesn't match its header";
    return false;
  }
  return true;
}

std::vector<fi::UsbNode> Nodes(const wchar_t* id) { return fi::FindNodes(id); }

const char kRemoveAvid[] =
    "  1. Settings > Apps > Installed apps: uninstall \"Avid Mbox 2 USB Drivers\" (on older PCs it may be called\n"
    "     \"Digidesign Mbox 2\"). If it isn't listed, go on with 2.\n"
    "  2. Unplug the Mbox. In this folder, right-click remove-avid-leftovers.ps1 > Run with PowerShell, and press\n"
    "     Enter when it has listed what it found.\n"
    "  3. Restart Windows, plug the Mbox back in (directly, no hub), and run this program again.\n";

void Describe(const char* what, const std::wstring& device) {
  std::vector<uint8_t> desc, config;
  std::string error;
  if (fi::DeviceDescriptorThroughHub(device, &desc, &error)) {
    LogOnly(std::string(what) + " device descriptor:\n" + Hex(desc));
  } else {
    LogOnly(std::string(what) + " device descriptor: " + error + "\n");
  }
  if (fi::ConfigDescriptorThroughHub(device, &config, &error)) {
    LogOnly(std::string(what) + " configuration descriptor (" + std::to_string(config.size()) + " bytes):\n" + Hex(config));
  } else {
    LogOnly(std::string(what) + " configuration descriptor: " + error + "\n");
  }
}

dfu::Functional FunctionalOf(const std::wstring& device) {
  std::vector<uint8_t> config;
  std::string error;
  if (!fi::ConfigDescriptorThroughHub(device, &config, &error)) return {};
  return dfu::FindFunctional(config.data(), config.size());
}

// Gets WinUSB, with kDfuInterfaceGuid, onto the devnode with this hardware ID: writes the interface GUID, then
// opens Device Manager on it and waits while the user picks "WinUsb Device".
bool EnsureWinUsb(const wchar_t* hardware_id, const char* what, fi::UsbNode* out) {
  auto nodes = Nodes(hardware_id);
  if (nodes.empty()) {
    Say("%s isn't there any more.\n", what);
    return false;
  }
  fi::UsbNode node = nodes[0];
  Say("%s: %s\n  driver: %s, problem code %lu\n", what, fi::Narrow(node.instance).c_str(),
      node.service.empty() ? "none" : fi::Narrow(node.service).c_str(), node.problem);
  if (node.has_interface) {
    *out = node;
    return true;
  }
  std::string error;
  if (!fi::SetInterfaceGuid(node.instance, &error)) {
    Say("Couldn't prepare it for WinUSB: %s\n", error.c_str());
    return false;
  }
  bool restarted = false;
  if (_wcsicmp(node.service.c_str(), L"WINUSB") == 0) {
    Say("WinUSB is already on it: restarting it so WinUSB picks up the interface.\n");
    if (!fi::RestartNode(node.instance, &error)) Say("  (restart failed: %s)\n", error.c_str());
    restarted = true;
  } else {
    Say("\nWindows needs Microsoft's WinUSB driver on %s. Its properties window is opening now. In it:\n"
        "  1. Driver tab > Update driver\n"
        "  2. Browse my computer for drivers\n"
        "  3. Let me pick from a list of available drivers on my computer\n"
        "  4. Universal Serial Bus devices, then Next\n"
        "  5. Manufacturer \"WinUsb Device\", model \"WinUsb Device\", then Next\n"
        "  6. Windows warns that it can't verify the driver is compatible: Yes\n"
        "  7. Close the windows once it says the driver was updated.\n"
        "This program carries on by itself as soon as the driver is on. (WinUSB is part of Windows and signed by\n"
        "Microsoft; nothing else gets installed.)\n\n",
        what);
    fi::OpenProperties(node.instance);
  }
  int winusb_seconds = 0;
  for (int second = 0; second < 900; ++second) {
    auto now = Nodes(hardware_id);
    if (now.empty()) {
      Say("%s disappeared while waiting for the driver.\n", what);
      return false;
    }
    if (now[0].has_interface) {
      *out = now[0];
      Say("WinUSB is on %s.\n", what);
      return true;
    }
    // WinUSB on but no interface: the value may have been reset by the install, so write it again and restart.
    if (_wcsicmp(now[0].service.c_str(), L"WINUSB") == 0 && ++winusb_seconds == 5 && !restarted) {
      Say("WinUSB is on, but without the device interface yet: setting it again and restarting the device.\n");
      if (!fi::SetInterfaceGuid(now[0].instance, &error)) Say("  (%s)\n", error.c_str());
      if (!fi::RestartNode(now[0].instance, &error)) Say("  (restart failed: %s)\n", error.c_str());
      restarted = true;
    }
    if (second % 30 == 29) Say("  still waiting for WinUSB on %s...\n", what);
    Sleep(1000);
  }
  Say("Gave up waiting for WinUSB on %s after 15 minutes.\n", what);
  return false;
}

// Waits for the Mbox to reappear after a replug, and says which firmware it started.
int ReportAfterReplug() {
  Say("\nUnplug the Mbox's USB cable now, wait 15 seconds, and plug it back in. Waiting for it (5 minutes)...\n");
  bool gone = false;
  for (int second = 0; second < 300; ++second) {
    const bool update = !Nodes(kUpdate).empty(), runtime = !Nodes(kRuntime).empty();
    if (!update && !runtime && Nodes(kOpen).empty()) gone = true;
    if (gone || second > 20) {
      if (!Nodes(kOpen).empty()) {
        Sleep(3000);  // let Windows bind its drivers
        auto open = Nodes(kOpen);
        Say("\nDONE: the Mbox started the open firmware (%s, firmware %x.%02x).\n",
            open.empty() ? "1209:0001" : fi::Narrow(open[0].instance).c_str(),
            open.empty() ? 0 : open[0].revision >> 8, open.empty() ? 0 : open[0].revision & 0xFF);
        Say("It's now a standard USB audio device (\"Mbox 2 Mini\"). Mbox Hub, from the project's GitHub releases,\n"
            "shows its meters and installs later updates over USB.\n");
        return 0;
      }
      if (gone && runtime) {
        Say("\nThe Mbox came back on Avid's firmware (0DBA:3000): Avid's update mode didn't keep the new firmware.\n"
            "It works as before. Please send the log.\n");
        return 2;
      }
      if (!Nodes(kTiBoot).empty()) {
        Say("\nThe Mbox shows up as TI's boot mode (0451:3200): its firmware chip doesn't hold a firmware that starts.\n"
            "Only the wired install (see the project's README) brings it back. Please send the log.\n");
        return 3;
      }
    }
    Sleep(1000);
  }
  Say("\nThe Mbox didn't come back as anything recognisable within 5 minutes. Please send the log, and say what\n"
      "Device Manager shows for it.\n");
  return 4;
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
  bool header255 = false;
  std::wstring image_path = ExeFolder() + L"\\mbox2-open.bin";
  for (int i = 1; i < argc; ++i) {
    if (_wcsicmp(argv[i], L"--header-255") == 0) {
      header255 = true;
    } else if (_wcsicmp(argv[i], L"--image") == 0 && i + 1 < argc) {
      image_path = argv[++i];
    } else {
      printf("usage: mbox2-firstinstall.exe [--header-255] [--image FILE]\n");
      return 2;
    }
  }

  SYSTEMTIME t;
  GetLocalTime(&t);
  wchar_t log_name[64];
  swprintf(log_name, 64, L"\\mbox2-firstinstall-%04d%02d%02d-%02d%02d%02d.log", t.wYear, t.wMonth, t.wDay, t.wHour,
           t.wMinute, t.wSecond);
  _wfopen_s(&g_log, (ExeFolder() + log_name).c_str(), L"w");

  Say("Mbox 2 Mini open firmware: first install through Avid's update mode (EXPERIMENTAL)\n");
  Say("%s\n\n", WindowsVersion().c_str());
  if (!Elevated()) {
    Say("This needs administrator rights: right-click it and choose Run as administrator.\n");
    return Finish(1);
  }

  // The image.
  std::vector<uint8_t> image;
  std::string error;
  if (!LoadImage(image_path, &image, &error)) {
    Say("Firmware image: %s\n", error.c_str());
    return Finish(1);
  }
  if (header255) image[5] = 255;
  Say("Firmware image: %s, %zu bytes, header version %u\n", fi::Narrow(image_path).c_str(), image.size(), image[5]);

  // What's plugged in.
  if (!Nodes(kOpen).empty()) {
    Say("An Mbox already runs the open firmware (1209:0001): update it with Mbox Hub instead.\n");
    return Finish(0);
  }
  if (!Nodes(kTiBoot).empty()) {
    Say("A TUSB3200 is in TI's boot mode (0451:3200): its firmware chip doesn't load. Only the wired install\n"
        "fixes that.\n");
    return Finish(1);
  }
  const bool resume = !Nodes(kUpdate).empty();
  if (resume) {
    Say("The Mbox is already in Avid's update mode (0DBA:3001), maybe from an earlier attempt: going on from there.\n");
  } else {
    auto runtime = Nodes(kRuntime);
    if (runtime.empty()) {
      Say("No Mbox 2 on Avid's firmware (USB ID 0DBA:3000) found. Plug it in directly (no hub) and run this again.\n");
      return Finish(1);
    }
    if (runtime.size() > 1) {
      Say("%zu Mbox 2s are plugged in: leave only the one to update.\n", runtime.size());
      return Finish(1);
    }
    Say("Found: %s, Avid firmware %x.%02x, driver %s\n", fi::Narrow(runtime[0].instance).c_str(),
        runtime[0].revision >> 8, runtime[0].revision & 0xFF,
        runtime[0].service.empty() ? "none" : fi::Narrow(runtime[0].service).c_str());
    Describe("normal mode", runtime[0].instance);
    if (runtime[0].revision != kMiniRevision) {
      Say("Note: the one Mbox 2 Mini checked so far had Avid firmware 1.40. Every Avid version has the same update\n"
          "mode, so this one should install the same way. But the full-size Mbox 2 runs the same firmware (1.30-1.43)\n"
          "with the same USB ID, so nothing over USB tells the two apart: make sure yours is the Mini.\n");
    }
  }

  // Avid's driver, if it was ever installed, must be gone first (usb_win.h, UpdateModeDriverPackages).
  auto avid_packages = fi::UpdateModeDriverPackages();
  if (!avid_packages.empty()) {
    std::string names;
    for (const auto& name : avid_packages) names += (names.empty() ? "" : ", ") + name;
    Say("\nAvid's Mbox 2 driver is installed: its update-mode driver is in Windows' driver store (%s). It has to go\n"
        "first, or Windows would put it on the Mbox's update mode and Device Manager wouldn't offer WinUSB.\n"
        "Nothing has been changed yet. To remove it:\n%s"
        "(Without the app: in a command prompt opened as administrator, run\n"
        "pnputil /delete-driver <name> /uninstall /force for each of %s, then go on with 2.)\n",
        names.c_str(), kRemoveAvid, names.c_str());
    return Finish(1);
  }
  for (const wchar_t* id : {kRuntimeDfu, kUpdate}) {
    for (const auto& node : Nodes(id)) {
      if (!node.service.empty() && _wcsicmp(node.service.c_str(), L"WINUSB") != 0) {
        Say("\nWindows still has the driver %s on %s, left over from Avid's driver.\n"
            "Nothing has been changed yet. To clear it:\n%s",
            fi::Narrow(node.service).c_str(), fi::Narrow(node.instance).c_str(), kRemoveAvid);
        return Finish(1);
      }
    }
  }

  Say("\nThis writes new firmware into the Mbox through Avid's update mode. It has NOT been tested on a real Mbox\n"
      "yet. If Avid's update mode takes the file but the result doesn't start, the Mbox stops working until its\n"
      "firmware chip is rewritten with the wired install (two soldered wires and a Raspberry Pi). Keep the Mbox\n"
      "plugged in until this program asks you to unplug it.\n\n");
  if (!AskYes("Is this a Mbox 2 Mini (the small one: no MIDI or S/PDIF ports), and should it be installed?")) {
    Say("Not installed.\n");
    return Finish(1);
  }

  if (!resume) {
    // 1. WinUSB on the DFU interface of the normal mode, then DFU_DETACH.
    auto runtime = Nodes(kRuntime);
    if (runtime.empty()) return Finish(1);
    const std::wstring device = runtime[0].instance;
    dfu::Functional f = FunctionalOf(device);
    Say("\nIts DFU interface: %s, interface %u, attributes %02x, detach timeout %u ms, %u-byte transfers\n",
        f.found ? "found" : "NOT found", f.interface_number, f.attributes, f.detach_timeout_ms, f.transfer_size);
    if (!f.found) {
      Say("Without a DFU interface there's no update mode to use.\n");
      return Finish(1);
    }
    if (Nodes(kRuntimeDfu).empty()) {
      Say("Windows doesn't list the DFU interface (%s) as its own device.\n", fi::Narrow(kRuntimeDfu).c_str());
      return Finish(1);
    }
    fi::UsbNode dfu_node;
    if (!EnsureWinUsb(kRuntimeDfu, "the Mbox's update interface", &dfu_node)) return Finish(1);
    fi::WinUsbDfu runtime_usb;
    if (!runtime_usb.Open(dfu_node.instance, f.interface_number, &error)) {
      Say("Opening it through WinUSB: %s\n", error.c_str());
      return Finish(1);
    }
    dfu::Status s;
    if (dfu::GetStatus(runtime_usb, &s, &error)) {
      Say("Normal mode answers GETSTATUS: %s, %s\n", dfu::StateName(s.state), dfu::StatusName(s.status));
    } else {
      Say("Normal mode doesn't answer GETSTATUS (optional in DFU 1.0): %s\n", error.c_str());
    }
    const uint16_t timeout = f.detach_timeout_ms ? f.detach_timeout_ms : 5000;
    Say("Asking it to switch to update mode (DFU_DETACH, %u ms)...\n", timeout);
    if (!dfu::Detach(runtime_usb, timeout, &error)) {
      Say("  DFU_DETACH reported: %s (going on: some devices leave the bus at once)\n", error.c_str());
    }
    runtime_usb.Close();

    // 2. Update mode starts after a USB reset (this device doesn't detach by itself: attribute bit 3 is clear).
    bool update = false;
    for (int i = 0; i < 8 && !update; ++i) {
      Sleep(250);
      update = !Nodes(kUpdate).empty();
    }
    if (!update) {
      Say("Resetting its USB port so it restarts in update mode...\n");
      if (!fi::CyclePort(device, &error)) Say("  %s\n", error.c_str());
      for (int i = 0; i < 60 && !update; ++i) {
        Sleep(250);
        update = !Nodes(kUpdate).empty();
      }
    }
    if (!update) {
      Say("The Mbox didn't come up in update mode (0DBA:3001)%s. Nothing was written. Unplug it, wait 15 s, plug it\n"
          "back in, and please send the log.\n",
          Nodes(kRuntime).empty() ? "" : "; it's back in its normal mode");
      return Finish(1);
    }
    Say("The Mbox is in update mode (0DBA:3001).\n");
  }

  // 3. Update mode: its descriptors, WinUSB, the download.
  Sleep(1000);
  auto update = Nodes(kUpdate);
  if (update.empty()) {
    Say("Update mode disappeared.\n");
    return Finish(1);
  }
  const std::wstring update_device = update[0].instance;
  Describe("update mode", update_device);
  dfu::Functional uf = FunctionalOf(update_device);
  Say("Update mode's DFU interface: %s, interface %u, protocol %u, attributes %02x, %u-byte transfers\n",
      uf.found ? "found" : "not found (using normal mode's 8-byte transfers)", uf.interface_number, uf.protocol,
      uf.attributes, uf.transfer_size);
  const uint16_t transfer_size = uf.found && uf.transfer_size ? uf.transfer_size : 8;
  const uint8_t interface_number = uf.found ? uf.interface_number : 0;
  const wchar_t* update_id = Nodes(kUpdateDfu).empty() ? kUpdate : kUpdateDfu;
  fi::UsbNode update_node;
  if (!EnsureWinUsb(update_id, "the Mbox in update mode", &update_node)) {
    Say("Nothing was written. Unplug the Mbox, wait 15 s, and plug it back in: it starts Avid's firmware again.\n");
    return Finish(1);
  }

  for (int attempt = 1;; ++attempt) {
    fi::WinUsbDfu usb;
    if (!usb.Open(update_node.instance, interface_number, &error)) {
      Say("Opening update mode through WinUSB: %s\nNothing was written.\n", error.c_str());
      return Finish(1);
    }
    Say("\nWriting the firmware (%zu bytes in %u-byte blocks). Don't unplug the Mbox.\n", image.size(), transfer_size);
    size_t shown = 0;
    dfu::DownloadResult r = dfu::Download(
        usb, image, transfer_size,
        [&shown](size_t done, size_t total) {
          if (done == total || done - shown >= total / 20) {
            shown = done;
            printf("\r  %5zu / %zu bytes (%3zu %%)", done, total, done * 100 / total);
            fflush(stdout);
          }
        },
        [](const std::string& line) { Say("\n  %s\n", line.c_str()); });
    if (r.ok) {
      Say("\nThe firmware was written%s.\n",
          r.left_in_manifest ? ", and the Mbox restarted by itself while finishing" : " and update mode finished");
      usb.Close();
      return Finish(ReportAfterReplug());
    }
    Say("\nWriting failed: %s\n", r.error.c_str());
    dfu::Status s;
    std::string status_error;
    const bool answers = dfu::GetStatus(usb, &s, &status_error);
    usb.Close();
    if (!answers) {
      Say("Update mode no longer answers (%s). Don't give up on it yet: please send the log before unplugging.\n",
          status_error.c_str());
      return Finish(1);
    }
    Say("Update mode still answers (%s, %s).\n", dfu::StateName(s.state), dfu::StatusName(s.status));
    if (attempt >= 3 || !AskYes("Write it again from the start?")) {
      Say("Stopped. Please send the log before unplugging the Mbox.\n");
      return Finish(1);
    }
  }
}
