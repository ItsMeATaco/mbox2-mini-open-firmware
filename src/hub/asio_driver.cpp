#include "hub/asio_driver.h"

#include <knownfolders.h>
#include <shlobj.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "asio/engine.h"
#include "asio/names.h"

using namespace mbox2::asio;

static_assert([] {
  for (int frames : kDriverBufferChoices) {
    if (frames < kMinFrames || frames > kMaxFrames || frames % kGranularity) return false;
  }
  return true;
}(), "the driver refuses that buffer size");

namespace {

constexpr DWORD kTailBytes = 64 * 1024;  // a few hundred lines: the last several opens

std::wstring RegString(HKEY root, const std::wstring& key, const wchar_t* name) {
  wchar_t value[1024] = {};
  DWORD size = sizeof value;
  return RegGetValueW(root, key.c_str(), name, RRF_RT_REG_SZ, nullptr, value, &size) == ERROR_SUCCESS ? value : L"";
}

bool Starts(const std::string& text, const char* prefix) { return text.compare(0, strlen(prefix), prefix) == 0; }

// The number right before `label` ("41 input underruns" -> 41), or -1.
long long NumberBefore(const std::string& text, const char* label) {
  const size_t at = text.find(label);
  if (at == std::string::npos) return -1;
  size_t begin = at;
  while (begin > 0 && isdigit(static_cast<unsigned char>(text[begin - 1]))) --begin;
  return begin < at ? strtoll(text.c_str() + begin, nullptr, 10) : -1;
}

// The number right after `label` ("latencies in 429" -> 429), or -1.
long long NumberAfter(const std::string& text, const char* label) {
  const size_t at = text.find(label);
  if (at == std::string::npos) return -1;
  const char* digits = text.c_str() + at + strlen(label);
  return isdigit(static_cast<unsigned char>(*digits)) ? strtoll(digits, nullptr, 10) : -1;
}

// The log names programs by GetModuleFileNameA, so in the ANSI code page; the hub shows them without ".exe".
std::wstring ProgramName(const std::string& exe) {
  std::wstring name;
  if (!exe.empty()) {
    int length = MultiByteToWideChar(CP_ACP, 0, exe.c_str(), static_cast<int>(exe.size()), nullptr, 0);
    name.resize(length);
    MultiByteToWideChar(CP_ACP, 0, exe.c_str(), static_cast<int>(exe.size()), name.data(), length);
  }
  if (name.size() > 4 && _wcsicmp(name.c_str() + name.size() - 4, L".exe") == 0) name.resize(name.size() - 4);
  return name;
}

std::wstring Widen(const std::string& text) {
  if (text.empty()) return {};
  int length = MultiByteToWideChar(CP_ACP, 0, text.c_str(), static_cast<int>(text.size()), nullptr, 0);
  std::wstring wide(length, L'\0');
  MultiByteToWideChar(CP_ACP, 0, text.c_str(), static_cast<int>(text.size()), wide.data(), length);
  return wide;
}

// Whether process `pid` is still running as `program` (process IDs get reused).
bool Running(DWORD pid, const std::wstring& program) {
  HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
  if (!process) return false;
  DWORD code = 0;
  wchar_t path[MAX_PATH] = {};
  DWORD length = MAX_PATH;
  bool running = GetExitCodeProcess(process, &code) && code == STILL_ACTIVE &&
                 QueryFullProcessImageNameW(process, 0, path, &length);
  CloseHandle(process);
  if (!running) return false;
  const wchar_t* slash = wcsrchr(path, L'\\');
  std::wstring name = slash ? slash + 1 : path;
  if (name.size() > 4 && _wcsicmp(name.c_str() + name.size() - 4, L".exe") == 0) name.resize(name.size() - 4);
  return _wcsicmp(name.c_str(), program.c_str()) == 0;
}

// "2026-10-17 03:21:05.934 [Ableton Live 12 Suite.exe 14060] init" (src/asio/log.cpp).
struct LogLine {
  SYSTEMTIME time{};
  std::string exe;
  DWORD pid = 0;
  std::string message;
};

bool ParseLine(const std::string& text, LogLine* line) {
  int year, month, day, hour, minute, second, ms;
  if (sscanf_s(text.c_str(), "%4d-%2d-%2d %2d:%2d:%2d.%3d", &year, &month, &day, &hour, &minute, &second, &ms) != 7) {
    return false;
  }
  const size_t open = text.find(" [");
  const size_t close = open == std::string::npos ? open : text.find("] ", open);
  if (close == std::string::npos) return false;
  const std::string who = text.substr(open + 2, close - open - 2);
  const size_t space = who.rfind(' ');
  if (space == std::string::npos) return false;
  line->time = {static_cast<WORD>(year), static_cast<WORD>(month), 0, static_cast<WORD>(day), static_cast<WORD>(hour),
                static_cast<WORD>(minute), static_cast<WORD>(second), static_cast<WORD>(ms)};
  line->exe = who.substr(0, space);
  line->pid = strtoul(who.c_str() + space + 1, nullptr, 10);
  line->message = text.substr(close + 2);
  return true;
}

}  // namespace

AsioDriver FindAsioDriver() {
  AsioDriver driver;
  const std::wstring clsid = RegString(HKEY_LOCAL_MACHINE, std::wstring(L"SOFTWARE\\ASIO\\") + kRegistryName, L"CLSID");
  if (clsid.empty()) return driver;
  if (CompareStringOrdinal(clsid.c_str(), -1, kClsidString, -1, TRUE) != CSTR_EQUAL) {
    driver.state = AsioDriver::State::OtherClass;
    return driver;
  }
  driver.dll = RegString(HKEY_CLASSES_ROOT, std::wstring(L"CLSID\\") + kClsidString + L"\\InprocServer32", nullptr);
  if (driver.dll.empty()) {
    driver.state = AsioDriver::State::NoServer;
  } else if (GetFileAttributesW(driver.dll.c_str()) == INVALID_FILE_ATTRIBUTES) {
    driver.state = AsioDriver::State::DllMissing;
  } else {
    driver.state = AsioDriver::State::Installed;
  }
  return driver;
}

// As the driver's PreferredFrames() (src/asio/driver.cpp) reads it.
int ReadDriverBuffer(bool* set) {
  DWORD value = 0, size = sizeof value;
  const bool found = RegGetValueW(HKEY_CURRENT_USER, kSettingsKey, L"BufferFrames", RRF_RT_REG_DWORD, nullptr, &value,
                                  &size) == ERROR_SUCCESS;
  if (set) *set = found;
  if (!found) return kDefaultFrames;
  const long frames = std::clamp(static_cast<long>(value), static_cast<long>(kMinFrames), static_cast<long>(kMaxFrames));
  return static_cast<int>(frames / kGranularity * kGranularity);
}

bool WriteDriverBuffer(int frames) {
  const DWORD value = static_cast<DWORD>(frames);
  return RegSetKeyValueW(HKEY_CURRENT_USER, kSettingsKey, L"BufferFrames", REG_DWORD, &value, sizeof value) ==
         ERROR_SUCCESS;
}

std::wstring AsioInstallPath() {
  PWSTR base = nullptr;
  std::wstring path;
  if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_ProgramFiles, 0, nullptr, &base))) {
    path = std::wstring(base) + L"\\Mbox2Asio\\mbox2asio.dll";
  }
  CoTaskMemFree(base);
  return path;
}

namespace {

// Runs regsvr32 silently on `dll` (`unregister`: /u) and returns its exit code, or -1 if it didn't start.
long Regsvr32(const std::wstring& dll, bool unregister) {
  wchar_t system[MAX_PATH] = {};
  GetSystemDirectoryW(system, MAX_PATH);
  const std::wstring exe = std::wstring(system) + L"\\regsvr32.exe";
  std::wstring command = L"\"" + exe + L"\" /s " + (unregister ? L"/u " : L"") + L"\"" + dll + L"\"";
  STARTUPINFOW startup{sizeof startup};
  PROCESS_INFORMATION process{};
  if (!CreateProcessW(exe.c_str(), command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr,
                      &startup, &process)) {
    return -1;
  }
  WaitForSingleObject(process.hProcess, 30000);
  DWORD code = static_cast<DWORD>(-1);
  GetExitCodeProcess(process.hProcess, &code);
  CloseHandle(process.hThread);
  CloseHandle(process.hProcess);
  return static_cast<long>(code);
}

std::string Failed(const char* what, DWORD code) {
  char text[128];
  snprintf(text, sizeof text, "%s (error %lu)", what, code);
  return text;
}

// Copies a DAW still had loaded were renamed aside (mbox2asio.dll.old-*); deletes the ones that are free now.
void DeleteOldCopies(const std::wstring& folder, bool after_restart) {
  WIN32_FIND_DATAW found;
  HANDLE search = FindFirstFileW((folder + L"\\mbox2asio.dll.old-*").c_str(), &found);
  if (search == INVALID_HANDLE_VALUE) return;
  do {
    const std::wstring path = folder + L"\\" + found.cFileName;
    if (!DeleteFileW(path.c_str()) && after_restart) MoveFileExW(path.c_str(), nullptr, MOVEFILE_DELAY_UNTIL_REBOOT);
  } while (FindNextFileW(search, &found));
  FindClose(search);
}

}  // namespace

std::string InstallAsioDriver(const std::wstring& dll) {
  const std::wstring target = AsioInstallPath();
  if (target.empty()) return "Program Files wasn't found";
  if (GetFileAttributesW(dll.c_str()) == INVALID_FILE_ATTRIBUTES) return "mbox2asio.dll isn't next to the hub";
  const std::wstring folder = target.substr(0, target.find_last_of(L'\\'));
  CreateDirectoryW(folder.c_str(), nullptr);
  DeleteOldCopies(folder, false);
  if (CompareStringOrdinal(dll.c_str(), -1, target.c_str(), -1, TRUE) != CSTR_EQUAL &&
      !CopyFileW(dll.c_str(), target.c_str(), FALSE)) {
    // A loaded DLL can't be overwritten but can be renamed: a running DAW keeps its copy until it restarts.
    SYSTEMTIME now;
    GetLocalTime(&now);
    wchar_t suffix[32];
    swprintf_s(suffix, L".old-%04u%02u%02u%02u%02u%02u", now.wYear, now.wMonth, now.wDay, now.wHour, now.wMinute,
               now.wSecond);
    if (!MoveFileExW(target.c_str(), (target + suffix).c_str(), MOVEFILE_REPLACE_EXISTING) ||
        !CopyFileW(dll.c_str(), target.c_str(), FALSE)) {
      return Failed("copying the driver to Program Files failed", GetLastError());
    }
  }
  const long code = Regsvr32(target, false);
  if (code != 0) return Failed("registering the driver failed", static_cast<DWORD>(code));
  const AsioDriver installed = FindAsioDriver();
  if (installed.state != AsioDriver::State::Installed ||
      CompareStringOrdinal(installed.dll.c_str(), -1, target.c_str(), -1, TRUE) != CSTR_EQUAL) {
    return "the registry doesn't match what was installed";
  }
  return {};
}

std::string UninstallAsioDriver() {
  const std::wstring target = AsioInstallPath();
  if (target.empty()) return "Program Files wasn't found";
  if (GetFileAttributesW(target.c_str()) != INVALID_FILE_ATTRIBUTES) Regsvr32(target, true);
  // Whatever regsvr32 left, or everything if the DLL was already gone.
  RegDeleteTreeW(HKEY_LOCAL_MACHINE, (std::wstring(L"SOFTWARE\\Classes\\CLSID\\") + kClsidString).c_str());
  RegDeleteTreeW(HKEY_LOCAL_MACHINE, (std::wstring(L"SOFTWARE\\ASIO\\") + kRegistryName).c_str());
  if (FindAsioDriver().state != AsioDriver::State::NotInstalled) return "the driver's registry keys are still there";
  // A DAW that still has it loaded keeps the file until Windows restarts.
  const std::wstring folder = target.substr(0, target.find_last_of(L'\\'));
  if (!DeleteFileW(target.c_str()) && GetLastError() != ERROR_FILE_NOT_FOUND) {
    MoveFileExW(target.c_str(), nullptr, MOVEFILE_DELAY_UNTIL_REBOOT);
  }
  DeleteOldCopies(folder, true);
  if (!RemoveDirectoryW(folder.c_str())) MoveFileExW(folder.c_str(), nullptr, MOVEFILE_DELAY_UNTIL_REBOOT);
  return {};
}

void DriverLog::Refresh() {
  if (path_.empty()) {
    PWSTR base = nullptr;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &base))) {
      path_ = std::wstring(base) + L"\\" + kLogFolder + L"\\driver.log";
    }
    CoTaskMemFree(base);
  }
  WIN32_FILE_ATTRIBUTE_DATA info{};
  if (path_.empty() || !GetFileAttributesExW(path_.c_str(), GetFileExInfoStandard, &info)) {
    size_ = 0;
    time_ = {};
    session_ = {};
    failure_ = {};
    return;
  }
  const ULONGLONG size = static_cast<ULONGLONG>(info.nFileSizeHigh) << 32 | info.nFileSizeLow;
  if (size != size_ || CompareFileTime(&info.ftLastWriteTime, &time_) != 0) {
    size_ = size;
    time_ = info.ftLastWriteTime;
    Parse();
  }
  // A program that crashed never logs its close.
  session_.open_now = session_.found && !session_.closed && Running(session_.pid, session_.program);
}

void DriverLog::Parse() {
  session_ = {};
  failure_ = {};
  HANDLE file = CreateFileW(path_.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                            OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (file == INVALID_HANDLE_VALUE) return;
  LARGE_INTEGER size{};
  GetFileSizeEx(file, &size);
  LARGE_INTEGER start{};
  start.QuadPart = std::max<LONGLONG>(0, size.QuadPart - kTailBytes);
  SetFilePointerEx(file, start, nullptr, FILE_BEGIN);
  std::string data(static_cast<size_t>(size.QuadPart - start.QuadPart), '\0');
  DWORD read = 0;
  if (!ReadFile(file, data.data(), static_cast<DWORD>(data.size()), &read, nullptr)) read = 0;
  CloseHandle(file);
  data.resize(read);

  size_t pos = 0;
  if (start.QuadPart > 0) {  // skip the partial first line
    pos = data.find('\n');
    pos = pos == std::string::npos ? data.size() : pos + 1;
  }
  while (pos < data.size()) {
    size_t end = data.find('\n', pos);
    if (end == std::string::npos) end = data.size();
    std::string text = data.substr(pos, end - pos);
    pos = end + 1;
    if (!text.empty() && text.back() == '\r') text.pop_back();
    LogLine line;
    if (!ParseLine(text, &line)) continue;
    const std::string& m = line.message;
    auto fail = [&](const std::string& error) {
      failure_ = {true, line.time, ProgramName(line.exe), Widen(error)};
    };

    // Messages from src/asio/driver.cpp and engine.cpp.
    if (Starts(m, "createBuffers: Buffer size")) {
      fail(m.substr(strlen("createBuffers: ")));
    } else if (Starts(m, "createBuffers: ")) {
      const long long frames = NumberBefore(m, " frames (");
      if (frames <= 0) continue;
      session_ = {};
      session_.found = true;
      session_.time = line.time;
      session_.program = ProgramName(line.exe);
      session_.pid = line.pid;
      session_.frames = static_cast<int>(frames);
      const long long rate = NumberBefore(m, " Hz");
      session_.rate = rate > 0 ? static_cast<int>(rate) : 48000;
      session_.input_latency = static_cast<int>(NumberAfter(m, "latencies in "));
      session_.output_latency = static_cast<int>(NumberAfter(m, ", out "));
      failure_ = {};
    } else if (Starts(m, "open ") && m.find(" failed: ") != std::string::npos) {
      fail(m.substr(m.find(" failed: ") + strlen(" failed: ")));
    } else if (Starts(m, "open ") && m.find("no audio events") != std::string::npos) {
      fail("The Mbox didn't start streaming. Unplug it and plug it in again.");
    } else if (Starts(m, "init: ")) {
      fail(m.substr(strlen("init: ")));
    } else if (session_.found && line.pid == session_.pid && ProgramName(line.exe) == session_.program) {
      if (Starts(m, "stop after ") || Starts(m, "close: ")) {
        session_.underruns = NumberBefore(m, " input underruns");
        session_.late = std::max(NumberBefore(m, " late,"), NumberBefore(m, " late periods"));
        session_.capture_gaps = NumberBefore(m, " capture gaps");
        if (Starts(m, "close: ")) session_.closed = true;
      } else if (m == "driver released") {
        session_.closed = true;
      } else if (Starts(m, "device lost")) {
        session_.device_lost = true;
      }
    }
  }
}
