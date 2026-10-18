// Mbox Hub: live meters, loudness / spectrum / phase analysis, device health, a dropout log, firmware
// updates and the ASIO driver's state for the Mbox 2 Mini running the open USB Audio Class firmware. Windows
// plays to and records from the Mbox through its own driver; the hub only watches. Runs as a normal app (no
// kernel code); closing the window keeps it in the tray.
//
//   mbox2hub.exe                    open the window
//   mbox2hub.exe --tray             start hidden in the tray
//   mbox2hub.exe --snapshot x.png   run hidden for 3 s, save the window's image, exit
//   ... --view levels|loudness|spectrum|phase   open on that tab (for snapshots)
//   ... --panel open|closed         with the settings cog's side column open or closed (for snapshots)
//   ... --delay SECONDS             wait that long before the snapshot (default 3), e.g. for the clock to lock
//
// Run by the hub itself:
//   mbox2hub.exe --install-asio DLL --result FILE   elevated: install and register the ASIO driver
//   mbox2hub.exe --uninstall-asio --result FILE     elevated: remove it
//   mbox2hub.exe --after-update PID                 the updated hub, started by the old one before it quits

#include <windows.h>
#include <windowsx.h>
#include <dwmapi.h>
#include <knownfolders.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <wrl/client.h>

#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <ctime>
#include <cwctype>
#include <functional>
#include <iterator>
#include <mutex>
#include <string>
#include <vector>

#include "hub/analysis.h"
#include "hub/asio_driver.h"
#include "hub/device_link.h"
#include "hub/eeprom.h"
#include "hub/meters.h"
#include "hub/ui.h"
#include "hub/update.h"

namespace {

constexpr wchar_t kClassName[] = L"Mbox2HubWindow";
constexpr wchar_t kTitle[] = L"Mbox Hub";
constexpr wchar_t kSettingsKey[] = L"Software\\Mbox2Hub";
constexpr wchar_t kRunKey[] = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
constexpr wchar_t kStartupApprovedKey[] =
    L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\StartupApproved\\Run";
constexpr wchar_t kRunValue[] = L"Mbox2Hub";
constexpr wchar_t kHubVersion[] = L"" MBOX2_VERSION;  // the release this hub belongs to (CMake's project version)
constexpr UINT kMsgChanged = WM_APP + 1;  // the device link has news
constexpr UINT kMsgTray = WM_APP + 2;
constexpr UINT kMsgShow = WM_APP + 3;     // a second launch asks the running one to show itself
constexpr UINT kMsgDropout = WM_APP + 4;
constexpr UINT kMsgUpdate = WM_APP + 5;   // the updater has news
constexpr UINT_PTR kTimerFrame = 1;
constexpr UINT_PTR kTimerHousekeeping = 2;
constexpr UINT_PTR kTimerSnapshot = 3;
constexpr UINT_PTR kTimerUpdate = 4;
constexpr UINT kFrameMs = 33;
constexpr UINT kHousekeepingMs = 1000;
constexpr UINT kSnapshotDelayMs = 3000;
constexpr UINT kFirstCheckMs = 20000;    // the first look for a new release, after starting
constexpr UINT kCheckTimerMs = 3600000;  // then hourly, checking again once a day has passed
constexpr ULONGLONG kCheckIntervalMs = 24ull * 3600000;
// With the side column open; closed, the window is Ui::kSideWidth narrower and needs less height.
constexpr float kDefaultWidth = 1080.0f, kDefaultHeight = 940.0f;
constexpr float kCompactHeight = 840.0f;
constexpr int kSpectrumColumns = 320;  // log-spaced bands from 20 Hz to 20 kHz
constexpr ULONGLONG kNotifyIntervalMs = 60000;  // at most one dropout notification a minute
constexpr int kLockedPhase = 100;               // MCLK cycles (8 us) of accumulated lock error

// Meter ballistics.
constexpr float kReleaseDbPerSecond = 26.0f;
constexpr float kPeakHoldSeconds = 1.5f;
constexpr float kPeakFallDbPerSecond = 20.0f;
constexpr float kClipLevel = 0.9886f;  // -0.1 dBFS

enum MenuId : UINT {
  kMenuOpen = 1,
  kMenuStartup,
  kMenuLog,
  kMenuResetConverters,
  kMenuCheckUpdates,
  kMenuAutoUpdates,
  kMenuQuit
};

std::wstring LocalAppDataDir() {
  PWSTR base = nullptr;
  std::wstring dir;
  if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &base))) {
    dir = std::wstring(base) + L"\\Mbox2Hub";
    CreateDirectoryW(dir.c_str(), nullptr);
  }
  CoTaskMemFree(base);
  return dir;
}

class Logger {
 public:
  explicit Logger(const wchar_t* name) {
    std::wstring dir = LocalAppDataDir();
    if (!dir.empty()) {
      path_ = dir + L"\\" + name;
      file_ = _wfopen(path_.c_str(), L"a");
    }
  }
  ~Logger() {
    if (file_) fclose(file_);
  }
  void Write(const std::string& message) {
    std::lock_guard lock(mutex_);
    if (!file_) return;
    time_t now = time(nullptr);
    tm local{};
    localtime_s(&local, &now);
    char stamp[32];
    strftime(stamp, sizeof stamp, "%Y-%m-%d %H:%M:%S", &local);
    fprintf(file_, "%s %s\n", stamp, message.c_str());
    fflush(file_);
  }
  const std::wstring& Path() const { return path_; }

 private:
  std::mutex mutex_;
  std::wstring path_;
  FILE* file_ = nullptr;
};

DWORD ReadSetting(const wchar_t* name, DWORD fallback) {
  DWORD value = 0, size = sizeof value;
  return RegGetValueW(HKEY_CURRENT_USER, kSettingsKey, name, RRF_RT_REG_DWORD, nullptr, &value, &size) == ERROR_SUCCESS
             ? value
             : fallback;
}

void WriteSetting(const wchar_t* name, DWORD value) {
  RegSetKeyValueW(HKEY_CURRENT_USER, kSettingsKey, name, REG_DWORD, &value, sizeof value);
}

std::wstring ReadString(const wchar_t* key, const wchar_t* name) {
  wchar_t value[1024] = {};
  DWORD size = sizeof value;
  return RegGetValueW(HKEY_CURRENT_USER, key, name, RRF_RT_REG_SZ, nullptr, value, &size) == ERROR_SUCCESS ? value
                                                                                                            : L"";
}

void WriteString(const wchar_t* key, const wchar_t* name, const std::wstring& value) {
  RegSetKeyValueW(HKEY_CURRENT_USER, key, name, REG_SZ, value.c_str(),
                  static_cast<DWORD>((value.size() + 1) * sizeof(wchar_t)));
}

std::wstring Widen(const std::string& text) {
  if (text.empty()) return {};
  int length = MultiByteToWideChar(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()), nullptr, 0);
  std::wstring wide(length, L'\0');
  MultiByteToWideChar(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()), wide.data(), length);
  return wide;
}

std::string Narrow(const std::wstring& text) {
  if (text.empty()) return {};
  int length = WideCharToMultiByte(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
  std::string narrow(length, '\0');
  WideCharToMultiByte(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()), narrow.data(), length, nullptr, nullptr);
  return narrow;
}

std::wstring Printf(const wchar_t* format, ...) {
  wchar_t buffer[512];
  va_list args;
  va_start(args, format);
  vswprintf_s(buffer, format, args);
  va_end(args);
  return buffer;
}

std::wstring Clock(const SYSTEMTIME& t) { return Printf(L"%02u:%02u:%02u", t.wHour, t.wMinute, t.wSecond); }

// "03:21" today, "2026-10-16 03:21" before.
std::wstring When(const SYSTEMTIME& t) {
  SYSTEMTIME now;
  GetLocalTime(&now);
  if (t.wYear == now.wYear && t.wMonth == now.wMonth && t.wDay == now.wDay) return Clock(t).substr(0, 5);
  return Printf(L"%04u-%02u-%02u %02u:%02u", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute);
}

std::wstring Grouped(unsigned value) {
  return value >= 1000 ? Printf(L"%u,%03u", value / 1000, value % 1000) : std::to_wstring(value);
}

std::wstring Version(unsigned version) { return Printf(L"v%u.%02u", version / 100, version % 100); }
std::wstring Bcd(unsigned bcd) { return Printf(L"v%x.%02x", bcd >> 8, bcd & 0xFF); }

std::wstring ExePath() {
  wchar_t path[MAX_PATH] = {};
  GetModuleFileNameW(nullptr, path, MAX_PATH);
  return path;
}

std::wstring FileName(const std::wstring& path) {
  size_t slash = path.find_last_of(L"\\/");
  return slash == std::wstring::npos ? path : path.substr(slash + 1);
}

bool SamePath(const std::wstring& a, const std::wstring& b) {
  return CompareStringOrdinal(a.c_str(), -1, b.c_str(), -1, TRUE) == CSTR_EQUAL;
}

std::wstring Folder(const std::wstring& path) { return path.substr(0, path.find_last_of(L'\\')); }
std::wstring HubFolder() { return Folder(ExePath()); }
bool Exists(const std::wstring& path) { return GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES; }

std::wstring FullPath(const std::wstring& path) {
  wchar_t full[MAX_PATH] = {};
  return GetFullPathNameW(path.c_str(), MAX_PATH, full, nullptr) ? full : L"";
}

// Running from a build tree (build\src\hub\Release): the hub takes the firmware and the ASIO driver from the
// build, and doesn't replace itself with a release.
bool InBuildTree() {
  const std::wstring folder = HubFolder();
  for (const wchar_t* tail : {L"\\src\\hub\\Release", L"\\src\\hub\\Debug"}) {
    const size_t n = wcslen(tail);
    if (folder.size() > n && _wcsicmp(folder.c_str() + folder.size() - n, tail) == 0) return true;
  }
  return false;
}

std::wstring UpdatesFolder() { return LocalAppDataDir() + L"\\updates"; }
std::wstring DownloadedFirmwarePath() { return UpdatesFolder() + L"\\" + kFirmwareFile; }

// The firmware the hub offers: the newest open-firmware image among firmware\build.sh's output (in a build
// tree), the one next to the hub (the release zip) and the one the latest GitHub release brought.
std::wstring DefaultImagePath() {
  std::wstring best;
  int best_version = -1;
  const std::wstring candidates[] = {
      InBuildTree() ? FullPath(HubFolder() + L"\\..\\..\\..\\firmware\\mbox2-open.bin") : L"",
      HubFolder() + L"\\" + kFirmwareFile,
      DownloadedFirmwarePath(),
  };
  for (const std::wstring& candidate : candidates) {
    FirmwareImage image;
    if (!candidate.empty() && Exists(candidate) && LoadFirmwareImage(candidate, &image).empty() && !image.avid &&
        image.version > best_version) {
      best = candidate;
      best_version = image.version;
    }
  }
  return best;
}

// The ASIO driver the hub installs: the one next to it (the release zip), or in a build tree the build's.
std::wstring BundledDriverPath() {
  const std::wstring next = HubFolder() + L"\\" + kDriverFile;
  if (Exists(next)) return next;
  if (!InBuildTree()) return {};
  const std::wstring built = FullPath(HubFolder() + L"\\..\\..\\asio\\" + FileName(HubFolder()) + L"\\" + kDriverFile);
  return Exists(built) ? built : L"";
}

bool SameContents(const std::wstring& a, const std::wstring& b) {
  auto read = [](const std::wstring& path, std::string* data) {
    FILE* file = _wfopen(path.c_str(), L"rb");
    if (!file) return false;
    char buffer[65536];
    for (size_t n; (n = fread(buffer, 1, sizeof buffer, file)) > 0;) data->append(buffer, n);
    fclose(file);
    return true;
  };
  std::string x, y;
  return read(a, &x) && read(b, &y) && x == y;
}

bool FolderWritable(const std::wstring& folder) {
  HANDLE probe = CreateFileW((folder + L"\\.mbox2hub-write-test").c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                             FILE_ATTRIBUTE_TEMPORARY | FILE_FLAG_DELETE_ON_CLOSE, nullptr);
  if (probe == INVALID_HANDLE_VALUE) return false;
  CloseHandle(probe);
  return true;
}

// bcdDevice 0x0025 -> 25, the image header's version byte for the same firmware.
int FirmwareNumber(unsigned bcd) { return (bcd >> 8) * 100 + (bcd >> 4 & 15) * 10 + (bcd & 15); }

// The start of a release's notes as plain text, cut at a line end, for a message box.
std::wstring Excerpt(const std::wstring& notes, size_t limit) {
  const std::wstring text = PlainText(notes);
  if (text.size() <= limit) return text;
  const size_t cut = text.rfind(L'\n', limit);
  return text.substr(0, cut == std::wstring::npos || cut < limit / 2 ? limit : cut) + L"\n\x2026";
}

// "Start with Windows": HKCU Run entry (per user, no administrator rights). Task Manager's Startup apps
// page can turn it off separately (StartupApproved\Run, first byte odd = disabled).
enum class Startup { Off, On, OtherCopy, DisabledInTaskManager };

std::wstring StartupCommand() { return L"\"" + ExePath() + L"\" --tray"; }

Startup ReadStartup() {
  std::wstring command = ReadString(kRunKey, kRunValue);
  if (command.empty()) return Startup::Off;
  BYTE approved[12] = {};
  DWORD size = sizeof approved;
  if (RegGetValueW(HKEY_CURRENT_USER, kStartupApprovedKey, kRunValue, RRF_RT_REG_BINARY, nullptr, approved, &size) ==
          ERROR_SUCCESS &&
      size >= 1 && (approved[0] & 1)) {
    return Startup::DisabledInTaskManager;
  }
  return SamePath(command, StartupCommand()) ? Startup::On : Startup::OtherCopy;
}

void WriteStartup(bool on) {
  if (on) {
    WriteString(kRunKey, kRunValue, StartupCommand());
    RegDeleteKeyValueW(HKEY_CURRENT_USER, kStartupApprovedKey, kRunValue);  // a Task Manager "disabled"
  } else {
    RegDeleteKeyValueW(HKEY_CURRENT_USER, kRunKey, kRunValue);
  }
}

// The app icon (the hardware outline from the window header), rendered with 4x4 supersampling
// so no image files are needed.
bool IconShape(float u, float v) {
  auto rounded = [](float u, float v, float l, float t, float r, float b, float radius) {
    if (u < l || u > r || v < t || v > b) return false;
    float cx = std::clamp(u, l + radius, r - radius), cy = std::clamp(v, t + radius, b - radius);
    return (u - cx) * (u - cx) + (v - cy) * (v - cy) <= radius * radius;
  };
  if (rounded(u, v, 0.03f, 0.19f, 0.97f, 0.81f, 0.15f) && !rounded(u, v, 0.13f, 0.29f, 0.87f, 0.71f, 0.06f)) return true;
  auto dot = [&](float cx) { return (u - cx) * (u - cx) + (v - 0.5f) * (v - 0.5f) <= 0.105f * 0.105f; };
  return dot(0.33f) || dot(0.6f) || (u >= 0.74f && u <= 0.8f && v >= 0.39f && v <= 0.61f);
}

HICON MakeIcon(int size, COLORREF color) {
  BITMAPINFO info{};
  info.bmiHeader = {sizeof(BITMAPINFOHEADER), size, -size, 1, 32, BI_RGB};
  void* bits = nullptr;
  HDC screen = GetDC(nullptr);
  HBITMAP pixels = CreateDIBSection(screen, &info, DIB_RGB_COLORS, &bits, nullptr, 0);
  ReleaseDC(nullptr, screen);
  if (!pixels) return nullptr;
  auto* out = static_cast<uint32_t*>(bits);
  constexpr int kSamples = 4;
  for (int y = 0; y < size; ++y) {
    for (int x = 0; x < size; ++x) {
      int hits = 0;
      for (int sy = 0; sy < kSamples; ++sy) {
        for (int sx = 0; sx < kSamples; ++sx) {
          hits += IconShape((x + (sx + 0.5f) / kSamples) / size, (y + (sy + 0.5f) / kSamples) / size);
        }
      }
      uint32_t a = hits * 255 / (kSamples * kSamples);
      uint32_t r = GetRValue(color) * a / 255, g = GetGValue(color) * a / 255, b = GetBValue(color) * a / 255;
      out[y * size + x] = (a << 24) | (r << 16) | (g << 8) | b;  // premultiplied BGRA
    }
  }
  HBITMAP mask = CreateBitmap(size, size, 1, 1, nullptr);
  ICONINFO icon_info{TRUE, 0, 0, mask, pixels};
  HICON icon = CreateIconIndirect(&icon_info);
  DeleteObject(mask);
  DeleteObject(pixels);
  return icon;
}

class App {
 public:
  App()
      : log_(L"hub.log"),
        dropout_log_(L"dropouts.log"),
        updater_(std::wstring(L"Mbox2Hub/") + kHubVersion, [this] {
          if (HWND hwnd = hwnd_) PostMessageW(hwnd, kMsgUpdate, 0, 0);
        }) {}
  // view: the tab to open on (for --snapshot), or -1 for the last one used; panel likewise 0 closed, 1 open.
  // after_update: this hub was just installed by the one before it.
  int Run(HINSTANCE instance, bool start_in_tray, std::wstring snapshot_path, UINT snapshot_delay_ms, int view,
          int panel, bool after_update);

 private:
  static LRESULT CALLBACK Proc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam);
  LRESULT Handle(UINT message, WPARAM wparam, LPARAM lparam);

  void OnChanged();
  void OnFrame();
  void OnHousekeeping();
  void OnDropout();
  void UpdateModel();
  void Click(Hit hit);
  void ChooseImage();
  void SetImage(const std::wstring& path);
  void CheckImageFile();
  void Install();
  void ResetConverters();
  void RefreshDriver();
  void SetDriverBuffer(int frames);
  void RunElevated(const std::wstring& arguments, const wchar_t* doing);
  void FinishElevated();
  void CheckForUpdates(bool manual);
  void OnUpdate();
  void OfferUpdate();
  void ApplyHubUpdate(const std::wstring& folder);
  bool HubUpdateAvailable() const { return !release_.tag.empty() && CompareVersions(release_.tag, kHubVersion) > 0; }
  bool FirmwareUpdateAvailable() const;
  void ToggleStartup();
  void ToggleSidePanel();
  void OpenLog();
  void ShowMainWindow();
  void HideToTray();
  void MetersVisible();
  void Analyze(float peaks[4], float dt);
  void SetView(View view, int source);
  void AddTrayIcon();
  void UpdateTray();
  void Notify(const wchar_t* title, const std::wstring& text);
  void TrayMenu();
  void Repaint() { InvalidateRect(hwnd_, nullptr, FALSE); }
  bool Visible() const { return IsWindowVisible(hwnd_) && !IsIconic(hwnd_); }

  HINSTANCE instance_ = nullptr;
  HWND hwnd_ = nullptr;
  Logger log_;
  Logger dropout_log_;
  Ui ui_;
  UiModel model_;
  DeviceLink link_;
  DeviceLink::Status status_;
  EndpointMeters meters_;

  HICON icon_large_ = nullptr, icon_small_ = nullptr;
  NOTIFYICONDATAW tray_{};
  UINT taskbar_created_ = 0;
  std::wstring tray_tip_;
  bool told_about_tray_ = false;
  ULONGLONG last_notify_ = 0;
  int unnotified_dropouts_ = 0;
  SYSTEMTIME started_{};

  bool side_open_ = false;
  int grown_height_ = 0;  // pixels opening the side column added to the height, given back when it closes

  Hit hover_ = Hit::None;
  bool tracking_mouse_ = false;
  bool logged_paint_error_ = false;
  std::wstring snapshot_path_;
  LARGE_INTEGER frequency_{}, last_frame_{};

  // The ASIO driver (src/asio), read once a second while the window is open
  AsioDriver driver_;
  int driver_buffer_ = 0;
  DriverLog driver_log_;
  std::wstring bundled_driver_;    // the copy next to the hub, which Install / Update puts in Program Files
  bool driver_current_ = true;     // the installed driver is that copy
  std::wstring driver_compared_;   // the files' sizes and times when driver_current_ was worked out
  HANDLE elevated_ = nullptr;      // the elevated copy of the hub installing or removing the driver
  std::wstring elevated_doing_;    // "Installing", "Removing"
  std::wstring elevated_result_;   // the file it writes what went wrong to

  // New releases on GitHub
  Updater updater_;
  Updater::State update_state_ = Updater::State::Idle;
  Release release_;                // the latest release, once a check succeeded
  int release_firmware_ = -1;      // its firmware image's version, -1 if it brought none
  ULONGLONG last_check_ = 0;       // GetTickCount64() of the last successful check
  UINT snapshot_delay_ms_ = kSnapshotDelayMs;

  Startup startup_ = Startup::Off;
  std::wstring image_path_;
  FILETIME image_time_{};
  FirmwareImage image_;
  std::string image_problem_;
  DeviceLink::Install::State install_state_ = DeviceLink::Install::State::Idle;
  uint32_t reset_count_ = 0;  // converter resets already reported

  // Analysis views
  View view_ = View::Levels;
  int source_ = 0;     // spectrum and phase: 0 output, 1 inputs
  int smoothing_ = 0;  // 0 1/6 octave, 1 1/3 octave
  LoudnessMeter loudness_;
  SpectrumAnalyzer spectrum_;
  PhaseMeter phase_;
  uint32_t phase_rate_ = 0;  // the rate phase_ was reset for; 0 before its first audio
};

int App::Run(HINSTANCE instance, bool start_in_tray, std::wstring snapshot_path, UINT snapshot_delay_ms, int view,
             int panel, bool after_update) {
  instance_ = instance;
  snapshot_path_ = std::move(snapshot_path);
  snapshot_delay_ms_ = snapshot_delay_ms;
  QueryPerformanceFrequency(&frequency_);
  QueryPerformanceCounter(&last_frame_);
  GetLocalTime(&started_);
  log_.Write("---- Mbox Hub " + Narrow(kHubVersion) + " started" + (after_update ? " (just updated)" : ""));
  RegDeleteKeyValueW(HKEY_CURRENT_USER, kSettingsKey, L"BufferIndex");  // the placeholder's, before the driver
  RefreshDriver();
  view_ = static_cast<View>(view >= 0 ? std::min(view, 3) : std::min<DWORD>(ReadSetting(L"View", 0), 3));
  source_ = ReadSetting(L"AnalysisSource", 0) ? 1 : 0;
  smoothing_ = ReadSetting(L"Smoothing", 0) ? 1 : 0;
  side_open_ = panel >= 0 ? panel == 1 : ReadSetting(L"SidePanel", 0) != 0;
  startup_ = ReadStartup();
  std::wstring saved = ReadString(kSettingsKey, L"FirmwareImage");
  SetImage(!saved.empty() && GetFileAttributesW(saved.c_str()) != INVALID_FILE_ATTRIBUTES ? saved : DefaultImagePath());

  icon_large_ = MakeIcon(GetSystemMetrics(SM_CXICON), RGB(0xEC, 0xEA, 0xE4));
  icon_small_ = MakeIcon(GetSystemMetrics(SM_CXSMICON), RGB(0xEC, 0xEA, 0xE4));

  WNDCLASSEXW wc{sizeof wc};
  wc.lpfnWndProc = Proc;
  wc.hInstance = instance;
  wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
  wc.hbrBackground = CreateSolidBrush(RGB(0x0E, 0x0F, 0x11));
  wc.lpszClassName = kClassName;
  wc.hIcon = icon_large_;
  wc.hIconSm = icon_small_;
  RegisterClassExW(&wc);

  UINT dpi = GetDpiForSystem();
  const float width = side_open_ ? kDefaultWidth : kDefaultWidth - Ui::kSideWidth;
  const float height = side_open_ ? kDefaultHeight : kCompactHeight;
  RECT frame = {0, 0, MulDiv(static_cast<int>(width), dpi, 96), MulDiv(static_cast<int>(height), dpi, 96)};
  AdjustWindowRectExForDpi(&frame, WS_OVERLAPPEDWINDOW, FALSE, 0, dpi);
  hwnd_ = CreateWindowExW(0, kClassName, kTitle, WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT,
                          frame.right - frame.left, frame.bottom - frame.top, nullptr, nullptr, instance, this);
  if (!hwnd_) return 1;
  SetWindowTextW(hwnd_, (std::wstring(kTitle) + L" v" + kHubVersion).c_str());

  BOOL dark = TRUE;
  DwmSetWindowAttribute(hwnd_, DWMWA_USE_IMMERSIVE_DARK_MODE, &dark, sizeof dark);
  COLORREF caption = RGB(0x0E, 0x0F, 0x11);
  DwmSetWindowAttribute(hwnd_, DWMWA_CAPTION_COLOR, &caption, sizeof caption);

  if (FAILED(ui_.Init(hwnd_))) {
    log_.Write("Direct2D initialization failed");
    return 1;
  }

  HWND hwnd = hwnd_;
  // A --snapshot run is a second hub next to the one in the tray, which keeps the dropout log: it doesn't look
  // for dropouts.
  std::function<void(const DeviceLink::Dropout&)> on_dropout;
  if (snapshot_path_.empty()) {
    on_dropout = [this, hwnd](const DeviceLink::Dropout& dropout) {
      if (dropout.continuation) {
        dropout_log_.Write("dropout over: " + dropout.text);
      } else {
        dropout_log_.Write("dropout: " + dropout.text);
        PostMessageW(hwnd, kMsgDropout, 0, 0);
      }
    };
  }
  link_.Start([hwnd] { PostMessageW(hwnd, kMsgChanged, 0, 0); }, [this](const std::string& message) { log_.Write(message); },
              std::move(on_dropout));

  SetTimer(hwnd_, kTimerHousekeeping, kHousekeepingMs, nullptr);
  meters_.Refresh();
  UpdateModel();
  if (!snapshot_path_.empty()) {
    meters_.SetInputsWanted(true);
    meters_.SetOutputWanted(true);
    SetTimer(hwnd_, kTimerFrame, kFrameMs, nullptr);  // keep the meters moving while hidden
    SetTimer(hwnd_, kTimerSnapshot, snapshot_delay_ms_, nullptr);
  } else {
    taskbar_created_ = RegisterWindowMessageW(L"TaskbarCreated");
    AddTrayIcon();
    SetTimer(hwnd_, kTimerUpdate, kFirstCheckMs, nullptr);
    if (after_update) {
      Notify(L"Mbox Hub updated", std::wstring(L"Now running Mbox Hub v") + kHubVersion + L".");
    }
  }
  if (start_in_tray || !snapshot_path_.empty()) {
    told_about_tray_ = true;
  } else {
    ShowMainWindow();
  }

  MSG message;
  while (GetMessageW(&message, nullptr, 0, 0) > 0) {
    TranslateMessage(&message);
    DispatchMessageW(&message);
  }
  return 0;
}

LRESULT CALLBACK App::Proc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam) {
  if (message == WM_NCCREATE) {
    auto* app = static_cast<App*>(reinterpret_cast<CREATESTRUCTW*>(lparam)->lpCreateParams);
    app->hwnd_ = hwnd;
    SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(app));
  }
  auto* app = reinterpret_cast<App*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
  return app ? app->Handle(message, wparam, lparam) : DefWindowProcW(hwnd, message, wparam, lparam);
}

LRESULT App::Handle(UINT message, WPARAM wparam, LPARAM lparam) {
  if (message == taskbar_created_ && taskbar_created_) {
    AddTrayIcon();  // Explorer restarted
    return 0;
  }
  switch (message) {
    case WM_PAINT: {
      PAINTSTRUCT ps;
      BeginPaint(hwnd_, &ps);
      HRESULT hr = ui_.Paint(model_, hover_);
      EndPaint(hwnd_, &ps);
      if (FAILED(hr) && !logged_paint_error_) {
        logged_paint_error_ = true;
        char text[64];
        snprintf(text, sizeof text, "drawing the window failed: 0x%08lx", static_cast<unsigned long>(hr));
        log_.Write(text);
      }
      return 0;
    }
    case WM_ERASEBKGND:
      return 1;
    case WM_SIZE:
      ui_.Resize();
      if (wparam == SIZE_MINIMIZED) {
        KillTimer(hwnd_, kTimerFrame);
      } else {
        SetTimer(hwnd_, kTimerFrame, kFrameMs, nullptr);
      }
      MetersVisible();
      Repaint();
      return 0;
    case WM_GETMINMAXINFO: {
      UINT dpi = GetDpiForWindow(hwnd_);
      RECT frame = {0, 0, MulDiv(static_cast<int>(Ui::MinWidth(side_open_)), dpi, 96),
                    MulDiv(static_cast<int>(Ui::MinHeight(side_open_)), dpi, 96)};
      AdjustWindowRectExForDpi(&frame, WS_OVERLAPPEDWINDOW, FALSE, 0, dpi);
      auto* info = reinterpret_cast<MINMAXINFO*>(lparam);
      info->ptMinTrackSize = {frame.right - frame.left, frame.bottom - frame.top};
      return 0;
    }
    case WM_DPICHANGED: {
      ui_.SetDpi(HIWORD(wparam));
      const RECT* suggested = reinterpret_cast<RECT*>(lparam);
      SetWindowPos(hwnd_, nullptr, suggested->left, suggested->top, suggested->right - suggested->left,
                   suggested->bottom - suggested->top, SWP_NOZORDER | SWP_NOACTIVATE);
      return 0;
    }
    case WM_TIMER:
      if (wparam == kTimerFrame) OnFrame();
      if (wparam == kTimerHousekeeping) OnHousekeeping();
      if (wparam == kTimerUpdate) {
        SetTimer(hwnd_, kTimerUpdate, kCheckTimerMs, nullptr);
        if (ReadSetting(L"CheckForUpdates", 1) && (!last_check_ || GetTickCount64() - last_check_ >= kCheckIntervalMs)) {
          CheckForUpdates(false);
        }
      }
      if (wparam == kTimerSnapshot) {
        KillTimer(hwnd_, kTimerSnapshot);
        UpdateModel();
        RECT client;
        GetClientRect(hwnd_, &client);
        HRESULT hr = ui_.RenderToPng(model_, client.right - client.left, client.bottom - client.top,
                                     snapshot_path_.c_str());
        char text[64];
        snprintf(text, sizeof text, "snapshot: 0x%08lx", static_cast<unsigned long>(hr));
        log_.Write(text);
        DestroyWindow(hwnd_);
      }
      return 0;
    case kMsgChanged:
      OnChanged();
      return 0;
    case kMsgDropout:
      OnDropout();
      return 0;
    case kMsgUpdate:
      OnUpdate();
      return 0;
    case kMsgShow:
      ShowMainWindow();
      return 0;
    case WM_MOUSEMOVE: {
      POINT p = {GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
      if (!tracking_mouse_) {
        TRACKMOUSEEVENT track{sizeof track, TME_LEAVE, hwnd_, 0};
        tracking_mouse_ = TrackMouseEvent(&track);
      }
      Hit hit = ui_.HitTest(p);
      if (hit != hover_) {
        hover_ = hit;
        Repaint();
      }
      return 0;
    }
    case WM_MOUSELEAVE:
      tracking_mouse_ = false;
      if (hover_ != Hit::None) {
        hover_ = Hit::None;
        Repaint();
      }
      return 0;
    case WM_LBUTTONUP: {
      POINT p = {GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
      Click(ui_.HitTest(p));
      return 0;
    }
    case WM_SETCURSOR:
      if (LOWORD(lparam) == HTCLIENT && hover_ != Hit::None) {
        SetCursor(LoadCursorW(nullptr, IDC_HAND));
        return TRUE;
      }
      break;
    case WM_CLOSE:
      HideToTray();
      return 0;
    case kMsgTray:
      switch (LOWORD(lparam)) {
        case NIN_SELECT:
        case NIN_KEYSELECT:
        case NIN_BALLOONUSERCLICK:
          ShowMainWindow();
          break;
        case WM_CONTEXTMENU:
          TrayMenu();
          break;
      }
      return 0;
    case WM_COMMAND:
      switch (LOWORD(wparam)) {
        case kMenuOpen:
          ShowMainWindow();
          break;
        case kMenuStartup:
          ToggleStartup();
          break;
        case kMenuLog:
          OpenLog();
          break;
        case kMenuResetConverters:
          ResetConverters();
          break;
        case kMenuCheckUpdates:
          CheckForUpdates(true);
          break;
        case kMenuAutoUpdates:
          WriteSetting(L"CheckForUpdates", ReadSetting(L"CheckForUpdates", 1) ? 0 : 1);
          break;
        case kMenuQuit:
          if (status_.install.state == DeviceLink::Install::State::Running) {
            MessageBoxW(hwnd_, L"A firmware install is running. Quit once it has finished (under a minute).", kTitle,
                        MB_OK | MB_ICONINFORMATION);
            break;
          }
          DestroyWindow(hwnd_);
          break;
      }
      return 0;
    case WM_DESTROY:
      KillTimer(hwnd_, kTimerFrame);
      KillTimer(hwnd_, kTimerHousekeeping);
      KillTimer(hwnd_, kTimerUpdate);
      link_.Stop();
      meters_.SetInputsWanted(false);
      Shell_NotifyIconW(NIM_DELETE, &tray_);
      log_.Write("---- Mbox Hub stopped");
      PostQuitMessage(0);
      return 0;
  }
  return DefWindowProcW(hwnd_, message, wparam, lparam);
}

void App::OnChanged() {
  status_ = link_.GetStatus();
  using State = DeviceLink::Install::State;
  State state = status_.install.state;
  if (state != install_state_) {
    install_state_ = state;
    if (state == State::Done) {
      std::wstring text =
          L"Firmware " + Version(status_.install.version) + L" is installed and the Mbox rebooted into it.";
      if (status_.install.replug) {
        text += L" Unplug the Mbox and plug it back in before listening: a reboot can leave its DAC stuck.";
        Notify(L"Replug the Mbox", text);
      } else {
        text += L" Sound is back once Windows has found it again, in a few seconds.";
        Notify(L"Firmware installed", text);
      }
      if (Visible()) MessageBoxW(hwnd_, text.c_str(), kTitle, MB_OK | MB_ICONINFORMATION);
    } else if (state == State::Failed) {
      std::wstring text = L"The firmware install failed: " + Widen(status_.install.error) +
                          L"\n\nKeep the Mbox plugged in and install again. If it restarts without a complete image, "
                          L"it comes up in TI's boot mode and the Raspberry Pi has to write it.";
      Notify(L"Firmware install failed", Widen(status_.install.error));
      if (Visible()) MessageBoxW(hwnd_, text.c_str(), kTitle, MB_OK | MB_ICONERROR);
    }
  }
  if (status_.reset.count != reset_count_) {
    reset_count_ = status_.reset.count;
    if (!status_.reset.error.empty()) {
      Notify(L"Converter reset failed", Widen(status_.reset.error));
    } else if (status_.reset.adc) {
      Notify(L"Audio converters reset", L"The Mbox reset its DAC and ADC; sound is back within 40 ms.");
    } else {
      Notify(L"DAC reset", L"The Mbox reset its DAC; sound is back within 40 ms. The inputs' ADC can be reset too "
                           L"once the Mbox has been connected for 15 s.");
    }
  }
  UpdateModel();
  UpdateTray();
  if (Visible()) Repaint();
}

void App::OnHousekeeping() {
  startup_ = ReadStartup();  // Task Manager or another copy may change it
  CheckImageFile();
  meters_.Refresh();
  if (elevated_ && WaitForSingleObject(elevated_, 0) == WAIT_OBJECT_0) FinishElevated();
  if (Visible() || !snapshot_path_.empty()) RefreshDriver();
  OnChanged();
}

void App::OnDropout() {
  status_ = link_.GetStatus();
  if (status_.recent.empty()) return;
  const DeviceLink::Dropout& last = status_.recent.back();
  ULONGLONG now = GetTickCount64();
  if (last_notify_ && now - last_notify_ < kNotifyIntervalMs) {
    ++unnotified_dropouts_;
    return;
  }
  std::wstring text = Clock(last.time) + L": " + Widen(last.text) + L".";
  if (unnotified_dropouts_) text += Printf(L" (%d more since the last notice.)", unnotified_dropouts_);
  Notify(L"Mbox dropout", text);
  last_notify_ = now;
  unnotified_dropouts_ = 0;
}

void App::OnFrame() {
  LARGE_INTEGER now;
  QueryPerformanceCounter(&now);
  float dt = std::min(0.25f, static_cast<float>(now.QuadPart - last_frame_.QuadPart) / frequency_.QuadPart);
  last_frame_ = now;

  float peaks[4];
  Analyze(peaks, dt);
  for (int i = 0; i < 4; ++i) {
    MeterView& meter = model_.meters[i];
    float db = peaks[i] > 0.0f ? 20.0f * std::log10(peaks[i]) : -120.0f;
    meter.level_db = std::max(db, meter.level_db - kReleaseDbPerSecond * dt);
    if (db >= meter.peak_db) {
      meter.peak_db = db;
      meter.hold_seconds = kPeakHoldSeconds;
    } else if (meter.hold_seconds > 0.0f) {
      meter.hold_seconds -= dt;
    } else {
      meter.peak_db = std::max(meter.level_db, meter.peak_db - kPeakFallDbPerSecond * dt);
    }
    if (peaks[i] >= kClipLevel) meter.clip = true;
  }
  if (Visible()) Repaint();
}

void App::UpdateModel() {
  using Presence = DeviceLink::Presence;
  using Maintenance = DeviceLink::Maintenance;
  using State = DeviceLink::Install::State;
  const DeviceLink::Status& s = status_;
  const bool open = s.presence == Presence::OpenFirmware;
  const bool ready = open && s.maintenance == Maintenance::Ready;

  // ---- Header
  model_.rate.clear();
  model_.rate_kind = 0;
  switch (s.presence) {
    case Presence::OpenFirmware:
      model_.status = s.audio_ok ? L"Connected" : L"Needs attention";
      model_.status_kind = s.audio_ok ? 1 : 2;
      model_.subtitle = L"Open firmware " + Bcd(s.firmware) + L" \x00B7 USB Audio Class on Windows' own driver";
      if (!s.audio_ok) {
        model_.subtitle = Printf(L"Firmware %s, but Windows' audio driver isn't running on it (code %lu): replug "
                                 L"the Mbox.",
                                 Bcd(s.firmware).c_str(), s.audio_problem);
      } else if (s.maintenance == Maintenance::NotLoaded) {
        model_.subtitle += L" \x00B7 replug to load the maintenance link";
      }
      if (s.audio_ok) {
        // The clock's rate (both directions); without the maintenance link, firmware before v0.24 or before the
        // first read, the rate Windows plays at.
        const int rate = s.rate_reported ? s.rate : s.windows_play_rate ? s.windows_play_rate : s.rate;
        model_.rate = rate == 44100 ? L"44.1 kHz" : rate ? Printf(L"%d kHz", rate / 1000) : L"";
        model_.rate_kind = 0;
        if (s.windows_play_rate && s.windows_rec_rate && s.windows_play_rate != s.windows_rec_rate) {
          // One clock serves both directions: whichever starts second fails while the other runs.
          model_.rate_kind = 2;
          model_.subtitle = Printf(L"Windows plays at %.1f kHz but records at %.1f kHz: set both the same (Sound "
                                   L"settings \x203A Mbox \x203A Advanced)",
                                   s.windows_play_rate / 1000.0, s.windows_rec_rate / 1000.0);
        }
      }
      break;
    case Presence::AvidFirmware:
      model_.status = L"Avid firmware";
      model_.status_kind = 2;
      model_.subtitle = L"The Mbox runs Avid's firmware. Put the open firmware on with a Raspberry Pi (see the README).";
      break;
    case Presence::BootRom:
      model_.status = L"No firmware";
      model_.status_kind = 2;
      model_.subtitle = L"The Mbox started in TI's boot mode: its EEPROM has no valid image. Write one with the Pi.";
      break;
    default:
      model_.status = L"Not connected";
      model_.status_kind = 0;
      model_.subtitle = L"Plug in the Mbox to start.";
      break;
  }

  // A newer release on GitHub: of the hub (with its driver and firmware), or just firmware newer than the Mbox's.
  model_.update.clear();
  if (update_state_ == Updater::State::Downloading) {
    model_.update = L"Downloading " + release_.tag + L"\x2026";
  } else if (HubUpdateAvailable()) {
    model_.update = L"Update to " + release_.tag;
  } else if (FirmwareUpdateAvailable()) {
    model_.update = L"Firmware " + Version(release_firmware_);
  }

  // ---- Meters
  model_.inputs_live = meters_.InputsLive();
  model_.outputs_live = meters_.OutputsFound();
  model_.meters_note.clear();
  if (open && s.audio_ok) {
    if (!meters_.InputsNote().empty()) {
      model_.meters_note = L"Inputs: " + meters_.InputsNote();
    } else if (!model_.outputs_live) {
      model_.meters_note = L"The Mbox's speakers endpoint isn't active.";
    }
  }

  // ---- Tiles
  for (auto& attention : model_.tile_attention) attention = false;
  const wchar_t* no_link = L"";
  switch (s.maintenance) {
    case Maintenance::NotLoaded: no_link = L"replug to read it"; break;
    case Maintenance::InUse: no_link = L"maintenance link in use"; break;
    case Maintenance::Missing: no_link = L"needs firmware v0.19+"; break;
    default: no_link = L"maintenance link failed"; break;
  }
  if (!open) {
    model_.tile_value[0] = L"\x2014";
    model_.tile_sub[0] = L"not plugged in";
  } else if (s.clock_valid) {
    bool locked = std::abs(s.phase) <= kLockedPhase;
    model_.tile_value[0] = locked ? L"Locked" : L"Settling";
    model_.tile_attention[0] = !locked;
    model_.tile_sub[0] = Printf(L"%+.1f ppm \x00B7 phase %+d", s.clock_ppm, s.phase);
  } else {
    model_.tile_value[0] = L"\x2014";
    model_.tile_sub[0] = ready ? L"reading\x2026" : no_link;
  }

  model_.tile_value[1] = std::to_wstring(s.dropouts);
  if (!s.recent.empty()) {
    model_.tile_sub[1] = L"last at " + Clock(s.recent.back().time);
    SYSTEMTIME now;
    GetLocalTime(&now);
    FILETIME a, b;
    SystemTimeToFileTime(&s.recent.back().time, &a);
    SystemTimeToFileTime(&now, &b);
    ULONGLONG age = (reinterpret_cast<ULARGE_INTEGER&>(b).QuadPart - reinterpret_cast<ULARGE_INTEGER&>(a).QuadPart) / 10000000;
    model_.tile_attention[1] = age < 60;
  } else {
    model_.tile_sub[1] = L"none since " + Clock(started_).substr(0, 5);
  }

  if (!open || !s.counters_valid) {
    model_.tile_value[2] = L"\x2014";
    model_.tile_sub[2] = !open ? L"" : s.counters_old ? L"needs firmware v0.23+" : L"counters unavailable";
  } else if (!s.streaming || s.paused) {
    model_.tile_value[2] = L"\x2014";
    model_.tile_sub[2] = s.overflows ? Printf(L"no playback \x00B7 %u ovf.", s.overflows) : L"no playback";
  } else {
    model_.tile_value[2] = Printf(L"%.0f/s", s.missed_rate);
    model_.tile_sub[2] = Printf(L"usually %.0f/s \x00B7 %u overflow%s", s.missed_usual, s.overflows,
                                s.overflows == 1 ? L"" : L"s");
  }

  if (!open || !s.counters_valid) {
    model_.tile_value[3] = L"\x2014";
    model_.tile_sub[3] = open ? L"" : L"not plugged in";
  } else if (s.paused) {
    model_.tile_value[3] = L"Paused";
    model_.tile_sub[3] = s.playing ? L"open, but no packets" : L"Windows paused it";
    model_.tile_attention[3] = s.playing;
  } else if (s.streaming) {
    model_.tile_value[3] = L"Running";
    if (s.stream_seconds < 0) {
      model_.tile_sub[3] = L"since before the hub looked";
    } else if (s.stream_seconds < 60) {
      model_.tile_sub[3] = Printf(L"for %.0f s", s.stream_seconds);
    } else {
      model_.tile_sub[3] = Printf(L"for %d min", static_cast<int>(s.stream_seconds / 60));
    }
  } else {
    model_.tile_value[3] = L"Idle";
    model_.tile_sub[3] = L"nothing playing to it";
  }

  // ---- Converter reset (firmware v0.27+): the DAC alone until the Mbox has been connected for 15 s
  model_.reset_label.clear();
  if (open && s.firmware >= MaintenanceLink::kConvResetFirmware) {
    model_.reset_label = s.reset_adc ? L"Reset converters" : L"Reset DAC";
  }
  model_.reset_enabled = s.can_reset;

  // ---- Firmware
  model_.fw_installed = open ? Bcd(s.firmware) + L" installed" : L"";
  model_.fw_image = FileName(image_path_);
  model_.fw_image_ok = image_problem_.empty() && !image_path_.empty();
  if (image_path_.empty()) {
    model_.fw_image_info = L"Choose a firmware image (.bin)";
  } else if (!image_problem_.empty()) {
    model_.fw_image_info = L"Not usable: " + Widen(image_problem_);
  } else {
    model_.fw_image_info = (image_.avid ? L"Avid's firmware " : L"") + Version(image_.version) + L" \x00B7 " +
                           Grouped(image_.size) + L" bytes of code \x00B7 sum " + Printf(L"%04X OK", image_.checksum);
  }
  const int installed = open ? FirmwareNumber(s.firmware) : -1;
  model_.fw_busy = s.install.state == State::Running;
  model_.fw_can_install = ready && model_.fw_image_ok && !model_.fw_busy;
  model_.fw_progress = s.install.total ? static_cast<float>(s.install.done) / s.install.total : 0.0f;
  model_.fw_note_kind = 0;
  if (model_.fw_busy) {
    model_.fw_note = Widen(s.install.stage) + Printf(L" \x00B7 %d%%", s.install.total ? s.install.done * 100 / s.install.total : 0);
  } else if (s.install.state == State::Done && s.install.replug) {
    model_.fw_note = L"Installed " + Version(s.install.version) + L". Unplug the Mbox and plug it back in before "
                     L"listening: a reboot can leave the DAC stuck.";
    model_.fw_note_kind = 2;
  } else if (s.install.state == State::Done) {
    model_.fw_note = L"Installed " + Version(s.install.version) + L". The Mbox restarts into it; no replug needed.";
  } else if (s.install.state == State::Failed) {
    model_.fw_note = L"Failed: " + Widen(s.install.error) + L" Keep the Mbox plugged in and try again.";
    model_.fw_note_kind = 3;
  } else if (!open) {
    model_.fw_note = L"Plug in the Mbox with the open firmware to update it over USB.";
  } else if (s.maintenance == Maintenance::NotLoaded) {
    model_.fw_note = Printf(L"Needs the maintenance interface, which Windows didn't load (code %lu): unplug and "
                            L"replug the Mbox.",
                            s.maintenance_problem);
    model_.fw_note_kind = 2;
  } else if (s.maintenance == Maintenance::InUse) {
    model_.fw_note = L"Another program has the maintenance interface open.";
    model_.fw_note_kind = 2;
  } else if (s.maintenance == Maintenance::Missing) {
    model_.fw_note = L"This firmware has no maintenance interface (v0.19+): use the Raspberry Pi.";
    model_.fw_note_kind = 2;
  } else if (model_.fw_image_ok && image_.avid) {
    model_.fw_note = L"Avid's original firmware: afterwards the Mbox needs Avid's driver again, and this hub can't "
                     L"reach it.";
    model_.fw_note_kind = 2;
  } else if (model_.fw_image_ok && image_.version == installed) {
    model_.fw_note = image_.NeedsReplug() ? L"Same version as installed. Takes about 30 s; replug the Mbox afterwards."
                                          : L"Same version as installed. Takes about 30 s.";
  } else if (model_.fw_image_ok && image_.version > installed) {
    model_.fw_note = L"Newer than the Mbox's " + Bcd(s.firmware) + L". Takes about 30 s" +
                     (image_.NeedsReplug() ? L"; replug the Mbox afterwards." : L".");
    model_.fw_note_kind = 2;
  } else if (model_.fw_image_ok && !image_.NeedsReplug()) {
    model_.fw_note = L"Takes about 30 s; sound stops for a few seconds while the Mbox reboots.";
  } else {
    model_.fw_note = L"Takes about 30 s; sound stops when the Mbox reboots. Replug it afterwards.";
  }

  // ---- Dropout log
  model_.log_times.clear();
  model_.log_texts.clear();
  for (auto it = s.recent.rbegin(); it != s.recent.rend(); ++it) {
    model_.log_times.push_back(Clock(it->time));
    model_.log_texts.push_back(Widen(it->text));
  }
  model_.log_empty = L"No dropouts since the hub started at " + Clock(started_).substr(0, 5) + L".";
  if (open && s.counters_old) {
    model_.log_empty += L" Watching for them needs firmware v0.23 or later.";
  }
  model_.log_footer = L"Counters checked every second \x00B7 full log in " + FileName(dropout_log_.Path());

  // ---- DAW (ASIO)
  using Driver = AsioDriver::State;
  const bool can_install = !bundled_driver_.empty();
  const wchar_t* missing = L"mbox2asio.dll isn't next to mbox2hub.exe: unzip the release again.";
  model_.daw_path_attention = driver_.state != Driver::Installed || !driver_current_;
  model_.daw_badge = L"Needs repair";
  model_.daw_action.clear();
  model_.daw_action_enabled = true;
  model_.daw_remove = false;
  switch (driver_.state) {
    case Driver::Installed:
      model_.daw_badge = L"Installed";
      model_.daw_path = driver_.dll;
      if (!driver_current_ && can_install) {
        model_.daw_action = L"Update";
        model_.daw_path = L"This hub brought a newer driver";
      } else {
        model_.daw_remove = true;
      }
      break;
    case Driver::NotInstalled:
      model_.daw_badge = L"Not installed";
      model_.daw_action = can_install ? L"Install" : L"";
      model_.daw_path = can_install ? L"Install asks for administrator rights" : missing;
      break;
    case Driver::OtherClass:
      model_.daw_path = L"Its registry entry names another driver";
      break;
    case Driver::NoServer:
      model_.daw_path = L"Its COM registration is missing";
      break;
    case Driver::DllMissing:
      model_.daw_path = L"Its DLL is missing";
      break;
  }
  if (driver_.state != Driver::Installed && driver_.state != Driver::NotInstalled) {
    if (can_install) {
      model_.daw_action = L"Repair";
    } else {
      model_.daw_path = missing;
    }
  }
  if (elevated_) {
    model_.daw_action = elevated_doing_ + L"\x2026";
    model_.daw_action_enabled = false;
    model_.daw_remove = false;
  }

  model_.daw_buffer = -1;
  for (int i = 0; i < static_cast<int>(std::size(kDriverBufferChoices)); ++i) {
    if (kDriverBufferChoices[i] == driver_buffer_) model_.daw_buffer = i;
  }
  model_.daw_caption = L"The DAW picks the actual size in its audio settings. This sets the size the driver "
                       L"prefers (BufferFrames).";
  if (model_.daw_buffer < 0) {
    model_.daw_caption = Printf(L"The driver prefers %d samples (%d ms), set outside the hub. The DAW picks the "
                                L"actual size.",
                                driver_buffer_, driver_buffer_ / 48);
  }

  const DriverSession& session = driver_log_.Session();
  const DriverFailure& failure = driver_log_.Failure();
  std::wstring opened;
  if (session.found) {
    opened = Printf(L"%d samples (%.0f ms) at %s", session.frames, session.frames * 1000.0 / session.rate,
                    session.rate == 44100 ? L"44.1 kHz" : Printf(L"%d kHz", session.rate / 1000).c_str());
    if (session.input_latency >= 0 && session.output_latency >= 0) {
      opened += Printf(L" \x00B7 latency in %d, out %d", session.input_latency, session.output_latency);
    }
  }
  model_.daw_session_attention = false;
  if (session.open_now) {
    model_.daw_session = session.program + L" has it open, since " + When(session.time);
    model_.daw_session_detail = opened + L". Other programs can't play through the Mbox meanwhile.";
  } else if (failure.found) {
    model_.daw_session = When(failure.time) + L" " + failure.program + L" couldn't open it:";
    model_.daw_session_detail = failure.error;
    model_.daw_session_attention = true;
  } else if (session.found) {
    model_.daw_session = L"Last open " + When(session.time) + L" by " + session.program;
    std::wstring counts;
    if (session.underruns < 0 && session.late < 0) {
      counts = L"no close in the log";
    } else if (session.underruns <= 0 && session.late <= 0 && session.capture_gaps <= 0) {
      counts = L"no late periods or input underruns";
    } else {
      counts = Printf(L"%lld late period%s, %lld input underrun%s", std::max(0LL, session.late),
                      session.late == 1 ? L"" : L"s", std::max(0LL, session.underruns),
                      session.underruns == 1 ? L"" : L"s");
      if (session.capture_gaps > 0) counts += Printf(L", %lld capture gaps", session.capture_gaps);
      model_.daw_session_attention = true;
    }
    if (session.device_lost) {
      counts += L"; it lost the Mbox";
      model_.daw_session_attention = true;
    }
    model_.daw_session_detail = opened + L" \x00B7 " + counts;
  } else {
    model_.daw_session = L"No program has opened it yet";
    model_.daw_session_detail = L"Its log: %LOCALAPPDATA%\\Mbox2Asio\\driver.log";
  }

  // The input meters' shared-mode capture stream keeps the driver from opening the inputs exclusively.
  model_.daw_reminder_attention = model_.inputs_live;
  model_.daw_reminder =
      model_.inputs_live
          ? L"This window's input meters hold the Mbox inputs: close it first, or the DAW reports \"The Mbox "
            L"inputs are in use by another program\"."
          : L"While the input meters are on screen, the hub holds the Mbox inputs, and the DAW reports \"The "
            L"Mbox inputs are in use by another program\".";

  model_.startup = startup_ == Startup::On || startup_ == Startup::OtherCopy;
  switch (startup_) {
    case Startup::On: model_.startup_note = L"Starts in the tray when you sign in"; break;
    case Startup::OtherCopy: model_.startup_note = L"Set for another copy of the hub; toggle twice for this one"; break;
    case Startup::DisabledInTaskManager: model_.startup_note = L"Turned off in Task Manager's Startup apps"; break;
    default: model_.startup_note = L"In the tray, for this Windows user"; break;
  }

  // An install's progress or outcome, a broken or outdated driver, or newer firmware puts a dot on the closed cog.
  model_.side_open = side_open_;
  model_.side_attention = model_.fw_busy || (s.install.state == State::Done && s.install.replug) ||
                          s.install.state == State::Failed ||
                          (driver_.state != Driver::Installed && driver_.state != Driver::NotInstalled) ||
                          (driver_.state == Driver::Installed && !driver_current_ && can_install) ||
                          (model_.fw_image_ok && !image_.avid && installed >= 0 && image_.version > installed);

  model_.view = view_;
  model_.source = source_;
  model_.smoothing = smoothing_;
  if (view_ != View::Loudness && source_ == 1) {
    model_.analysis_note = !meters_.InputsNote().empty()
                               ? L"Inputs: " + meters_.InputsNote()
                               : L"Inputs 1 and 2 as Windows records them from \"Microphone (Mbox 2 Mini)\".";
  } else {
    model_.analysis_note = !meters_.OutputNote().empty()
                               ? L"Output: " + meters_.OutputNote()
                               : L"Output: what Windows plays to the Mbox, by loopback. A DAW using exclusive mode or "
                                 L"ASIO doesn't show here.";
  }
}

void App::Click(Hit hit) {
  switch (hit) {
    case Hit::ResetClips:
      for (auto& meter : model_.meters) meter.clip = false;
      break;
    case Hit::ChooseImage:
      ChooseImage();
      break;
    case Hit::Install:
      Install();
      break;
    case Hit::ResetConverters:
      ResetConverters();
      break;
    case Hit::OpenLog:
      OpenLog();
      break;
    case Hit::Startup:
      ToggleStartup();
      break;
    case Hit::Settings:
      ToggleSidePanel();
      break;
    case Hit::Update:
      OfferUpdate();
      break;
    case Hit::DawAction:
      if (!bundled_driver_.empty()) RunElevated(L"--install-asio \"" + bundled_driver_ + L"\"", L"Installing");
      break;
    case Hit::DawRemove:
      if (MessageBoxW(hwnd_,
                      L"Remove the ASIO driver?\n\nDAWs stop listing \"Mbox 2 Mini (open firmware)\". A DAW that has it "
                      L"open keeps its copy until it restarts.",
                      kTitle, MB_YESNO | MB_ICONQUESTION | MB_DEFBUTTON2) == IDYES) {
        RunElevated(L"--uninstall-asio", L"Removing");
      }
      break;
    case Hit::Buffer0:
    case Hit::Buffer1:
    case Hit::Buffer2:
    case Hit::Buffer3:
    case Hit::Buffer4:
      SetDriverBuffer(kDriverBufferChoices[static_cast<int>(hit) - static_cast<int>(Hit::Buffer0)]);
      break;
    case Hit::ViewLevels:
    case Hit::ViewLoudness:
    case Hit::ViewSpectrum:
    case Hit::ViewPhase:
      SetView(static_cast<View>(static_cast<int>(hit) - static_cast<int>(Hit::ViewLevels)), source_);
      break;
    case Hit::SourceOutput:
    case Hit::SourceInputs:
      SetView(view_, hit == Hit::SourceInputs ? 1 : 0);
      break;
    case Hit::ResetLoudness:
      loudness_.ResetMeasurement();
      log_.Write("Loudness measurement reset");
      break;
    case Hit::Smoothing6:
    case Hit::Smoothing3:
      smoothing_ = hit == Hit::Smoothing3 ? 1 : 0;
      WriteSetting(L"Smoothing", smoothing_);
      break;
    default:
      return;
  }
  UpdateModel();
  Repaint();
}

void App::SetImage(const std::wstring& path) {
  image_path_ = path;
  image_ = {};
  image_problem_.clear();
  image_time_ = {};
  if (path.empty()) return;
  WIN32_FILE_ATTRIBUTE_DATA info{};
  if (GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &info)) image_time_ = info.ftLastWriteTime;
  image_problem_ = LoadFirmwareImage(path, &image_);
}

// A rebuilt image replaces the file: read it again.
void App::CheckImageFile() {
  if (image_path_.empty()) return;
  WIN32_FILE_ATTRIBUTE_DATA info{};
  FILETIME time{};
  if (GetFileAttributesExW(image_path_.c_str(), GetFileExInfoStandard, &info)) time = info.ftLastWriteTime;
  if (CompareFileTime(&time, &image_time_) != 0) SetImage(image_path_);
}

void App::ChooseImage() {
  Microsoft::WRL::ComPtr<IFileOpenDialog> dialog;
  if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dialog)))) return;
  COMDLG_FILTERSPEC types[] = {{L"Firmware images (*.bin)", L"*.bin"}, {L"All files", L"*.*"}};
  dialog->SetFileTypes(2, types);
  dialog->SetTitle(L"Choose a firmware image for the Mbox");
  std::wstring start = !image_path_.empty() ? image_path_ : DefaultImagePath();
  if (!start.empty()) {
    std::wstring folder = start.substr(0, start.find_last_of(L'\\'));
    Microsoft::WRL::ComPtr<IShellItem> item;
    if (SUCCEEDED(SHCreateItemFromParsingName(folder.c_str(), nullptr, IID_PPV_ARGS(&item)))) {
      dialog->SetFolder(item.Get());
    }
    dialog->SetFileName(FileName(start).c_str());
  }
  if (FAILED(dialog->Show(hwnd_))) return;
  Microsoft::WRL::ComPtr<IShellItem> result;
  PWSTR path = nullptr;
  if (SUCCEEDED(dialog->GetResult(&result)) && SUCCEEDED(result->GetDisplayName(SIGDN_FILESYSPATH, &path))) {
    SetImage(path);
    WriteString(kSettingsKey, L"FirmwareImage", path);
    CoTaskMemFree(path);
  }
}

void App::Install() {
  SetImage(image_path_);  // what's in the file now
  if (!image_problem_.empty() || status_.maintenance != DeviceLink::Maintenance::Ready) {
    UpdateModel();
    Repaint();
    return;
  }
  std::wstring text = L"Install firmware " + Version(image_.version) + L" on the Mbox?\n\n" + FileName(image_path_) +
                      L", " + Grouped(image_.size) + L" bytes of code, replacing " + Bcd(status_.firmware) +
                      L".\n\nIt takes about 30 seconds. Keep the Mbox plugged in until the hub says it's done. Sound "
                      L"may crackle while it writes and " +
                      (image_.NeedsReplug() ? L"stops when the Mbox reboots; then unplug and replug it."
                                            : L"stops for a few seconds while the Mbox reboots.");
  if (image_.avid) {
    text = L"Put Avid's firmware " + Version(image_.version) + L" back on the Mbox?\n\n" + FileName(image_path_) +
           L" replaces the open firmware " + Bcd(status_.firmware) +
           L". Afterwards the Mbox needs Avid's driver again (which needs Memory Integrity off), and this hub can't "
           L"reach it any more: the open firmware can only go back on with the Raspberry Pi.\n\nIt takes about 30 "
           L"seconds. Keep the Mbox plugged in until the hub says it's done, then unplug and replug it.";
  }
  if (MessageBoxW(hwnd_, text.c_str(), kTitle, MB_YESNO | MB_ICONQUESTION | MB_DEFBUTTON2) != IDYES) return;
  if (link_.StartInstall(image_)) {
    install_state_ = DeviceLink::Install::State::Running;
    log_.Write("Install requested: " + Narrow(image_path_));
  }
  OnChanged();
}

// Vendor request 0x0C, for a silent output or stuck inputs: the outcome comes back as a notification.
void App::ResetConverters() {
  if (link_.ResetConverters()) log_.Write("Converter reset requested");
}

void App::RefreshDriver() {
  driver_ = FindAsioDriver();
  driver_buffer_ = ReadDriverBuffer();
  driver_log_.Refresh();
  // Whether the installed driver is the copy next to the hub: compared again only when either file changes.
  bundled_driver_ = BundledDriverPath();
  if (driver_.state != AsioDriver::State::Installed || bundled_driver_.empty()) {
    driver_current_ = true;
    driver_compared_.clear();
    return;
  }
  std::wstring key;
  for (const std::wstring& path : {driver_.dll, bundled_driver_}) {
    WIN32_FILE_ATTRIBUTE_DATA info{};
    GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &info);
    key += path + Printf(L"|%lu|%lu|%lu|", info.nFileSizeLow, info.ftLastWriteTime.dwLowDateTime,
                         info.ftLastWriteTime.dwHighDateTime);
  }
  if (key != driver_compared_) {
    driver_compared_ = key;
    driver_current_ = SameContents(driver_.dll, bundled_driver_);
  }
}

// Installing or removing the driver writes to Program Files and HKLM, so a copy of the hub does it with
// administrator rights (wWinMain: --install-asio / --uninstall-asio); Windows asks the user first.
void App::RunElevated(const std::wstring& arguments, const wchar_t* doing) {
  if (elevated_) return;
  elevated_result_ = LocalAppDataDir() + L"\\elevated-result.txt";
  DeleteFileW(elevated_result_.c_str());
  const std::wstring exe = ExePath();
  const std::wstring parameters = arguments + L" --result \"" + elevated_result_ + L"\"";
  SHELLEXECUTEINFOW info{sizeof info};
  info.fMask = SEE_MASK_NOCLOSEPROCESS;
  info.hwnd = hwnd_;
  info.lpVerb = L"runas";
  info.lpFile = exe.c_str();
  info.lpParameters = parameters.c_str();
  info.nShow = SW_HIDE;
  if (!ShellExecuteExW(&info) || !info.hProcess) {
    if (GetLastError() != ERROR_CANCELLED) {
      MessageBoxW(hwnd_, Printf(L"Couldn't start the driver setup (error %lu).", GetLastError()).c_str(), kTitle,
                  MB_OK | MB_ICONERROR);
    }
    return;
  }
  elevated_ = info.hProcess;
  elevated_doing_ = doing;
  log_.Write("ASIO driver: " + Narrow(doing) + " (elevated)");
  UpdateModel();
  Repaint();
}

void App::FinishElevated() {
  DWORD code = 1;
  GetExitCodeProcess(elevated_, &code);
  CloseHandle(elevated_);
  elevated_ = nullptr;
  std::string error;
  if (FILE* file = _wfopen(elevated_result_.c_str(), L"rb")) {
    char buffer[512];
    error.assign(buffer, fread(buffer, 1, sizeof buffer, file));
    fclose(file);
  }
  DeleteFileW(elevated_result_.c_str());
  const bool installing = elevated_doing_ == L"Installing";
  driver_compared_.clear();
  RefreshDriver();
  if (code != 0) {
    if (error.empty()) error = "exit code " + std::to_string(code);
    log_.Write("ASIO driver: failed: " + error);
    MessageBoxW(hwnd_,
                ((installing ? L"Installing the ASIO driver failed: " : L"Removing the ASIO driver failed: ") +
                 Widen(error) + L".")
                    .c_str(),
                kTitle, MB_OK | MB_ICONERROR);
  } else {
    log_.Write(installing ? "ASIO driver: installed" : "ASIO driver: removed");
    if (installing && Visible()) {
      MessageBoxW(hwnd_,
                  L"The ASIO driver is installed.\n\nIn your DAW's audio settings, choose the ASIO driver type and "
                  L"\"Mbox 2 Mini (open firmware)\". A DAW that was already open needs a restart to see it.",
                  kTitle, MB_OK | MB_ICONINFORMATION);
    }
  }
  UpdateModel();
  Repaint();
}

bool App::FirmwareUpdateAvailable() const {
  return release_firmware_ >= 0 && status_.presence == DeviceLink::Presence::OpenFirmware &&
         release_firmware_ > FirmwareNumber(status_.firmware);
}

void App::CheckForUpdates(bool manual) {
  if (updater_.Busy()) return;
  updater_.Check(DownloadedFirmwarePath(), manual);
}

void App::OnUpdate() {
  const Updater::Status u = updater_.GetStatus();
  const Updater::State previous = update_state_;
  update_state_ = u.state;
  using State = Updater::State;
  if (previous == State::Checking && u.state == State::Checked) {
    last_check_ = GetTickCount64();
    release_ = u.release;
    release_firmware_ = -1;
    FirmwareImage fresh;
    if (!u.firmware.empty() && LoadFirmwareImage(u.firmware, &fresh).empty()) {
      release_firmware_ = fresh.version;
      // Newer firmware from GitHub becomes the offered image, unless Avid's firmware was chosen to restore.
      const bool newer = image_path_.empty() || !image_problem_.empty() || fresh.version > image_.version;
      if ((newer && !image_.avid) || SamePath(image_path_, u.firmware)) {
        SetImage(u.firmware);
        RegDeleteKeyValueW(HKEY_CURRENT_USER, kSettingsKey, L"FirmwareImage");
      }
    }
    log_.Write("Update check: latest release " + Narrow(release_.tag) +
               (release_firmware_ >= 0 ? ", firmware " + Narrow(Version(release_firmware_)) : ""));
    const bool hub = HubUpdateAvailable(), firmware = FirmwareUpdateAvailable();
    if ((hub || firmware) && !u.manual && ReadString(kSettingsKey, L"NotifiedRelease") != release_.tag) {
      WriteString(kSettingsKey, L"NotifiedRelease", release_.tag);
      Notify(L"Mbox update available",
             hub ? L"Mbox Hub " + release_.tag + L" is out. Open the hub and click the update button at the top."
                 : L"Firmware " + Version(release_firmware_) + L" is out. Open the hub and click the update button at "
                   L"the top.");
    }
    if (u.manual) {
      if (hub || firmware) {
        if (!Visible()) ShowMainWindow();
        OfferUpdate();
      } else {
        MessageBoxW(hwnd_, (std::wstring(L"Mbox Hub v") + kHubVersion + L" is up to date (latest release: " +
                            release_.tag + L").")
                               .c_str(),
                    kTitle, MB_OK | MB_ICONINFORMATION);
      }
    }
  } else if (previous == State::Checking && u.state == State::Failed) {
    log_.Write("Update check failed: " + u.error);
    if (u.manual) {
      MessageBoxW(hwnd_, (L"Couldn't check GitHub for updates: " + Widen(u.error) + L".").c_str(), kTitle,
                  MB_OK | MB_ICONWARNING);
    }
  } else if (previous == State::Downloading && u.state == State::Downloaded) {
    ApplyHubUpdate(u.folder);
  } else if (previous == State::Downloading && u.state == State::Failed) {
    log_.Write("Update download failed: " + u.error);
    MessageBoxW(hwnd_, (L"Downloading the update failed: " + Widen(u.error) + L".").c_str(), kTitle,
                MB_OK | MB_ICONWARNING);
  }
  UpdateModel();
  Repaint();
}

// The header's update pill: a new hub replaces this one (it brings its driver and firmware along, offered on
// their cards afterwards); newer firmware alone goes straight to the Firmware card's install.
void App::OfferUpdate() {
  if (!HubUpdateAvailable()) {
    if (FirmwareUpdateAvailable()) {
      if (!side_open_) ToggleSidePanel();
      Install();
    }
    return;
  }
  if (updater_.Busy()) return;
  std::wstring text = L"Mbox Hub " + release_.tag + L" is available (this is v" + kHubVersion + L").";
  if (!release_.title.empty() && release_.title != release_.tag) text += L"\n\n" + release_.title;
  const std::wstring notes = Excerpt(release_.notes, 700);
  if (!notes.empty()) text += L"\n\n" + notes;
  const wchar_t* blocked = InBuildTree()                 ? L"it runs from a build folder"
                           : !FolderWritable(HubFolder()) ? L"Windows doesn't let it change its own folder"
                                                          : nullptr;
  if (blocked) {
    text += std::wstring(L"\n\nThis copy can't update itself: ") + blocked + L". Open the release page?";
    if (MessageBoxW(hwnd_, text.c_str(), kTitle, MB_YESNO | MB_ICONINFORMATION) == IDYES) {
      ShellExecuteW(hwnd_, L"open", release_.page.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    }
    return;
  }
  text += L"\n\nDownload and install it now? The hub restarts by itself. A newer ASIO driver or firmware that "
          L"comes with it is offered on its card afterwards.";
  if (MessageBoxW(hwnd_, text.c_str(), kTitle, MB_YESNO | MB_ICONQUESTION) != IDYES) return;
  if (status_.install.state == DeviceLink::Install::State::Running) {
    MessageBoxW(hwnd_, L"A firmware install is running: update the hub once it has finished.", kTitle,
                MB_OK | MB_ICONINFORMATION);
    return;
  }
  std::wstring tag = release_.tag;
  for (wchar_t& c : tag) {
    if (!iswalnum(c) && c != L'.' && c != L'-') c = L'_';
  }
  updater_.Download(UpdatesFolder() + L"\\" + tag);
  log_.Write("Hub update: downloading " + Narrow(release_.tag));
}

// Swaps the downloaded files in next to the running hub (a running exe can be renamed, not overwritten), starts
// the new hub and quits; the new one waits for this one to exit and deletes the old files.
void App::ApplyHubUpdate(const std::wstring& folder) {
  const std::wstring hub = HubFolder();
  std::vector<std::wstring> set_aside;
  std::wstring failure;
  for (const wchar_t* name : {kHubFile, kDriverFile, kFirmwareFile}) {
    const std::wstring source = folder + L"\\" + name;
    const std::wstring target = name == kHubFile ? ExePath() : hub + L"\\" + name;
    if (!Exists(source)) continue;
    if (Exists(target)) {
      if (!MoveFileExW(target.c_str(), (target + L".old").c_str(), MOVEFILE_REPLACE_EXISTING)) {
        failure = Printf(L"setting %s aside failed (error %lu)", FileName(target).c_str(), GetLastError());
        break;
      }
      set_aside.push_back(target);
    }
    if (!CopyFileW(source.c_str(), target.c_str(), FALSE)) {
      failure = Printf(L"copying %s failed (error %lu)", name, GetLastError());
      break;
    }
  }
  std::wstring command = L"\"" + ExePath() + L"\" --after-update " + std::to_wstring(GetCurrentProcessId()) +
                         (Visible() ? L"" : L" --tray");
  STARTUPINFOW startup{sizeof startup};
  PROCESS_INFORMATION process{};
  if (failure.empty() && !CreateProcessW(ExePath().c_str(), command.data(), nullptr, nullptr, FALSE, 0, nullptr,
                                         nullptr, &startup, &process)) {
    failure = Printf(L"starting the new hub failed (error %lu)", GetLastError());
  }
  if (!failure.empty()) {
    for (const std::wstring& target : set_aside) {
      DeleteFileW(target.c_str());
      MoveFileExW((target + L".old").c_str(), target.c_str(), MOVEFILE_REPLACE_EXISTING);
    }
    log_.Write("Hub update failed: " + Narrow(failure));
    MessageBoxW(hwnd_, (L"Updating the hub failed: " + failure + L". Nothing was changed.").c_str(), kTitle,
                MB_OK | MB_ICONERROR);
    return;
  }
  CloseHandle(process.hThread);
  CloseHandle(process.hProcess);
  log_.Write("Hub update: installed " + Narrow(release_.tag) + ", restarting");
  DestroyWindow(hwnd_);
}

// The driver reads it in getBufferSize(), so the DAW sees it the next time it opens the driver or asks.
void App::SetDriverBuffer(int frames) {
  if (WriteDriverBuffer(frames)) {
    log_.Write("ASIO driver: preferred buffer " + std::to_string(frames));
  } else {
    log_.Write("ASIO driver: writing BufferFrames failed");
  }
  driver_buffer_ = ReadDriverBuffer();
}

void App::ToggleStartup() {
  bool on = !(startup_ == Startup::On || startup_ == Startup::OtherCopy);
  WriteStartup(on);
  startup_ = ReadStartup();
  log_.Write(on ? "Start with Windows: on" : "Start with Windows: off");
  UpdateModel();
  Repaint();
}

// The window grows to the right by the side column (and taller if the column needs it) and shrinks back, so the
// main panel keeps its size. Maximized, only the layout changes.
void App::ToggleSidePanel() {
  side_open_ = !side_open_;
  WriteSetting(L"SidePanel", side_open_ ? 1 : 0);
  if (!IsZoomed(hwnd_) && !IsIconic(hwnd_)) {
    const UINT dpi = GetDpiForWindow(hwnd_);
    RECT window, client;
    GetWindowRect(hwnd_, &window);
    GetClientRect(hwnd_, &client);
    const int side = MulDiv(static_cast<int>(Ui::kSideWidth), dpi, 96);
    int width = window.right - window.left + (side_open_ ? side : -side);
    int height = window.bottom - window.top;
    if (side_open_) {
      grown_height_ =
          std::max(0, MulDiv(static_cast<int>(kDefaultHeight), dpi, 96) - static_cast<int>(client.bottom - client.top));
      height += grown_height_;
    } else {
      height -= grown_height_;
      grown_height_ = 0;
    }
    MONITORINFO monitor{sizeof monitor};
    GetMonitorInfoW(MonitorFromWindow(hwnd_, MONITOR_DEFAULTTONEAREST), &monitor);
    const RECT& work = monitor.rcWork;
    width = std::min<int>(width, work.right - work.left);
    height = std::min<int>(height, work.bottom - work.top);
    const int left = std::max<int>(work.left, std::min<int>(window.left, work.right - width));
    const int top = std::max<int>(work.top, std::min<int>(window.top, work.bottom - height));
    SetWindowPos(hwnd_, nullptr, left, top, width, height, SWP_NOZORDER | SWP_NOACTIVATE);
  }
  if (side_open_) RefreshDriver();
}

void App::OpenLog() {
  const std::wstring& path = dropout_log_.Path();
  if (path.empty()) return;
  if (GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES) {
    ShellExecuteW(hwnd_, L"open", path.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
  } else {
    std::wstring folder = path.substr(0, path.find_last_of(L'\\'));
    ShellExecuteW(hwnd_, L"open", folder.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
  }
}

// The input meters hold a capture stream (hub/meters.h), so only while they're on screen: on the Levels tab,
// or as the spectrum or phase source. The output loopback runs while the window is open, so the loudness
// keeps measuring on every tab.
void App::MetersVisible() {
  if (!snapshot_path_.empty()) return;
  const bool visible = Visible();
  const bool inputs_shown =
      view_ == View::Levels || ((view_ == View::Spectrum || view_ == View::Phase) && source_ == 1);
  meters_.SetInputsWanted(visible && inputs_shown);
  meters_.SetOutputWanted(visible);
}

// Drains the streams (peaks for the level meters) and feeds the analysis: loudness from the output whenever the
// window is open, the spectrum and phase meter only on their own tabs.
void App::Analyze(float peaks[4], float dt) {
  const bool spectrum = view_ == View::Spectrum, phase = view_ == View::Phase;
  auto feed_view = [&](const float* stereo, size_t frames, uint32_t rate) {
    if (spectrum) {
      if (spectrum_.Rate() != rate) spectrum_.Reset(rate);
      spectrum_.Push(stereo, frames);
    } else if (phase) {
      if (rate != phase_rate_) {
        phase_.Reset(rate);
        phase_rate_ = rate;
      }
      phase_.Push(stereo, frames);
    }
  };
  auto output = [&](const float* stereo, size_t frames, uint32_t rate) {
    if (loudness_.Rate() != rate) loudness_.Reset(rate);
    loudness_.Process(stereo, frames);
    if (source_ == 0) feed_view(stereo, frames, rate);
  };
  auto inputs = [&](const float* stereo, size_t frames, uint32_t rate) {
    if (source_ == 1) feed_view(stereo, frames, rate);
  };
  meters_.Read(peaks, output, inputs);
  if (!Visible() && snapshot_path_.empty()) return;

  LoudnessView& l = model_.loudness;
  l.momentary = loudness_.Momentary();
  l.short_term = loudness_.ShortTerm();
  l.integrated = loudness_.Integrated();
  l.range = loudness_.Range();
  l.momentary_max = loudness_.MomentaryMax();
  l.short_term_max = loudness_.ShortTermMax();
  l.true_peak_max = loudness_.TruePeakMax();
  l.seconds = loudness_.Seconds();
  if (view_ == View::Loudness) {
    l.momentary_history.assign(loudness_.MomentaryHistory().begin(), loudness_.MomentaryHistory().end());
    l.short_term_history.assign(loudness_.ShortTermHistory().begin(), loudness_.ShortTermHistory().end());
  }
  if (spectrum) {
    spectrum_.Update(kSpectrumColumns, smoothing_ ? 1.0 / 3.0 : 1.0 / 6.0, dt);
    model_.spectrum.level = spectrum_.Level();
    model_.spectrum.peak = spectrum_.Peak();
  }
  if (phase) {
    model_.phase.correlation = phase_.Correlation();
    model_.phase.gain_db = phase_.GainDb();
    phase_.Points(&model_.phase.points);
  }
}

// Switching tabs or sources starts the spectrum and phase meter over, without the other source's history.
void App::SetView(View view, int source) {
  if (view == view_ && source == source_) return;
  view_ = view;
  source_ = source;
  WriteSetting(L"View", static_cast<DWORD>(view_));
  WriteSetting(L"AnalysisSource", static_cast<DWORD>(source_));
  spectrum_ = {};
  phase_ = {};
  phase_rate_ = 0;
  model_.spectrum = {};
  model_.phase = {};
  MetersVisible();
}

void App::ShowMainWindow() {
  ShowWindow(hwnd_, IsIconic(hwnd_) ? SW_RESTORE : SW_SHOW);
  SetForegroundWindow(hwnd_);
  SetTimer(hwnd_, kTimerFrame, kFrameMs, nullptr);
  QueryPerformanceCounter(&last_frame_);
  MetersVisible();
  RefreshDriver();
  UpdateModel();
  Repaint();
}

void App::HideToTray() {
  ShowWindow(hwnd_, SW_HIDE);
  KillTimer(hwnd_, kTimerFrame);  // no drawing while hidden
  MetersVisible();
  if (!told_about_tray_) {
    told_about_tray_ = true;
    Notify(L"Mbox Hub is still running", L"It keeps watching the Mbox for dropouts from the tray. Right-click the "
                                         L"icon to quit.");
  }
}

void App::AddTrayIcon() {
  tray_ = {};
  tray_.cbSize = sizeof tray_;
  tray_.hWnd = hwnd_;
  tray_.uID = 1;
  tray_.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP | NIF_SHOWTIP;
  tray_.uCallbackMessage = kMsgTray;
  tray_.hIcon = icon_small_;
  wcscpy_s(tray_.szTip, L"Mbox Hub");
  Shell_NotifyIconW(NIM_ADD, &tray_);
  tray_.uVersion = NOTIFYICON_VERSION_4;
  Shell_NotifyIconW(NIM_SETVERSION, &tray_);
  tray_tip_.clear();
  UpdateTray();
}

void App::UpdateTray() {
  if (!snapshot_path_.empty()) return;
  std::wstring tip = L"Mbox Hub \x2014 " + model_.status;
  if (status_.presence == DeviceLink::Presence::OpenFirmware) tip += L" \x00B7 " + Bcd(status_.firmware);
  if (status_.dropouts) tip += Printf(L" \x00B7 %u dropout%s", status_.dropouts, status_.dropouts == 1 ? L"" : L"s");
  if (tip == tray_tip_) return;
  tray_tip_ = tip;
  wcsncpy_s(tray_.szTip, tip.c_str(), _TRUNCATE);
  tray_.uFlags = NIF_TIP | NIF_SHOWTIP;
  Shell_NotifyIconW(NIM_MODIFY, &tray_);
}

void App::Notify(const wchar_t* title, const std::wstring& text) {
  if (!snapshot_path_.empty()) return;
  NOTIFYICONDATAW info = tray_;
  info.uFlags = NIF_INFO;
  info.dwInfoFlags = NIIF_USER | NIIF_LARGE_ICON;
  info.hBalloonIcon = icon_large_;
  wcsncpy_s(info.szInfoTitle, title, _TRUNCATE);
  wcsncpy_s(info.szInfo, text.c_str(), _TRUNCATE);
  Shell_NotifyIconW(NIM_MODIFY, &info);
}

void App::TrayMenu() {
  HMENU menu = CreatePopupMenu();
  bool startup = startup_ == Startup::On || startup_ == Startup::OtherCopy;
  AppendMenuW(menu, MF_STRING, kMenuOpen, L"Open Mbox Hub");
  AppendMenuW(menu, MF_STRING | (startup ? MF_CHECKED : 0), kMenuStartup, L"Start with Windows");
  AppendMenuW(menu, MF_STRING, kMenuLog, L"Open dropout log");
  if (status_.presence == DeviceLink::Presence::OpenFirmware &&
      status_.firmware >= MaintenanceLink::kConvResetFirmware) {
    AppendMenuW(menu, MF_STRING | (status_.can_reset ? 0 : MF_GRAYED), kMenuResetConverters,
                status_.reset_adc ? L"Reset audio converters" : L"Reset audio output (DAC)");
  }
  AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
  AppendMenuW(menu, MF_STRING | MF_GRAYED, 0, (std::wstring(L"Mbox Hub v") + kHubVersion).c_str());
  AppendMenuW(menu, MF_STRING | (updater_.Busy() ? MF_GRAYED : 0), kMenuCheckUpdates, L"Check for updates now");
  AppendMenuW(menu, MF_STRING | (ReadSetting(L"CheckForUpdates", 1) ? MF_CHECKED : 0), kMenuAutoUpdates,
              L"Check for updates daily");
  AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
  AppendMenuW(menu, MF_STRING, kMenuQuit, L"Quit");
  SetMenuDefaultItem(menu, kMenuOpen, FALSE);
  POINT cursor;
  GetCursorPos(&cursor);
  SetForegroundWindow(hwnd_);
  TrackPopupMenu(menu, TPM_RIGHTBUTTON | TPM_BOTTOMALIGN, cursor.x, cursor.y, 0, hwnd_, nullptr);
  DestroyMenu(menu);
}

}  // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int) {
  SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

  bool start_in_tray = false;
  std::wstring snapshot_path;
  UINT snapshot_delay_ms = kSnapshotDelayMs;
  int view = -1, panel = -1;
  DWORD after_update = 0;  // the old hub's process ID
  bool install_asio = false, uninstall_asio = false;
  std::wstring asio_dll, result_path;
  int argc = 0;
  wchar_t** argv = CommandLineToArgvW(GetCommandLineW(), &argc);
  for (int i = 1; argv && i < argc; ++i) {
    if (wcscmp(argv[i], L"--tray") == 0) start_in_tray = true;
    if (wcscmp(argv[i], L"--snapshot") == 0 && i + 1 < argc) snapshot_path = argv[++i];
    if (wcscmp(argv[i], L"--delay") == 0 && i + 1 < argc) snapshot_delay_ms = static_cast<UINT>(_wtof(argv[++i]) * 1000);
    if (wcscmp(argv[i], L"--after-update") == 0 && i + 1 < argc) after_update = wcstoul(argv[++i], nullptr, 10);
    if (wcscmp(argv[i], L"--install-asio") == 0 && i + 1 < argc) {
      install_asio = true;
      asio_dll = argv[++i];
    }
    if (wcscmp(argv[i], L"--uninstall-asio") == 0) uninstall_asio = true;
    if (wcscmp(argv[i], L"--result") == 0 && i + 1 < argc) result_path = argv[++i];
    if (wcscmp(argv[i], L"--view") == 0 && i + 1 < argc) {
      static const wchar_t* const kViews[] = {L"levels", L"loudness", L"spectrum", L"phase"};
      ++i;
      for (int v = 0; v < 4; ++v) {
        if (_wcsicmp(argv[i], kViews[v]) == 0) view = v;
      }
    }
    if (wcscmp(argv[i], L"--panel") == 0 && i + 1 < argc) {
      ++i;
      if (_wcsicmp(argv[i], L"closed") == 0) panel = 0;
      if (_wcsicmp(argv[i], L"open") == 0) panel = 1;
    }
  }
  LocalFree(argv);

  // The elevated copy that installs or removes the ASIO driver: no window, just the job and its outcome.
  if (install_asio || uninstall_asio) {
    const std::string error = install_asio ? InstallAsioDriver(asio_dll) : UninstallAsioDriver();
    Logger(L"hub.log").Write(std::string(install_asio ? "Elevated ASIO driver install: " : "Elevated ASIO driver "
                                                                                           "removal: ") +
                             (error.empty() ? "done" : error));
    if (!result_path.empty()) {
      if (FILE* file = _wfopen(result_path.c_str(), L"wb")) {
        fwrite(error.data(), 1, error.size(), file);
        fclose(file);
      }
    }
    return error.empty() ? 0 : 1;
  }

  // Started by the hub this one replaces: let it quit first, then delete the files it set aside.
  if (after_update) {
    if (HANDLE old = OpenProcess(SYNCHRONIZE, FALSE, after_update)) {
      WaitForSingleObject(old, 15000);
      CloseHandle(old);
    }
  }
  for (const std::wstring& old : {ExePath() + L".old", HubFolder() + L"\\" + kDriverFile + L".old",
                                  HubFolder() + L"\\" + kFirmwareFile + L".old"}) {
    DeleteFileW(old.c_str());
  }

  // One hub per user: a second launch just brings the first one forward.
  HANDLE single = nullptr;
  if (snapshot_path.empty()) {
    single = CreateMutexW(nullptr, TRUE, L"Local\\Mbox2Hub");
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
      if (HWND existing = FindWindowW(kClassName, nullptr)) PostMessageW(existing, kMsgShow, 0, 0);
      return 0;
    }
  }

  CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
  int result;
  {
    App app;
    result = app.Run(instance, start_in_tray, snapshot_path, snapshot_delay_ms, view, panel, after_update != 0);
  }
  CoUninitialize();
  if (single) CloseHandle(single);
  return result;
}
