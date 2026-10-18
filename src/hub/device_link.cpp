#include "hub/device_link.h"

#include <cfgmgr32.h>
#include <objbase.h>

#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <cwctype>

#include "hub/meters.h"
#include "hub/usb_ids.h"

namespace {

constexpr DWORD kPollMs = 1000;
constexpr uint8_t kCountersString = 0x49;  // diag.c
constexpr double kMclkPerSample = 256.0;  // MCLK = 256 x the sample rate: 12288 cycles per 1 ms USB frame at 48 kHz
// Each endpoint's shared-mode format (Sound settings > Advanced): PKEY_AudioEngine_DeviceFormat, a WAVEFORMATEX.
const PROPERTYKEY kDeviceFormatKey = {{0xf19f064d, 0x082c, 0x4e27, {0xbc, 0x73, 0x68, 0x82, 0xa1, 0xbb, 0x8e, 0x4c}}, 0};
constexpr size_t kMaxRecent = 50;
constexpr size_t kHistory = 30;
// Windows' arrival time of a device: DEVPKEY_Device_LastArrivalDate, a FILETIME.
const DEVPROPKEY kLastArrivalKey = {{0x83da6326, 0x97a6, 0x4088, {0x94, 0x53, 0xa1, 0x92, 0x3f, 0x57, 0x3b, 0x29}}, 102};
// After an install the Mbox reboots, which unplugs it from USB for a moment; by this long after the install it
// is back. An absence after that is a replug (which an image before v0.27 asks for) or an unplug.
constexpr ULONGLONG kRebootGraceMs = 15000;
// How long after the Mbox connects a converter reset may include the ADC. Sooner after power-up an ADC reset
// left input 2 pinned for about 8 s (firmware/src/audio.c).
constexpr uint64_t kAdcResetDelay = 15ull * 10000000;  // FILETIME units

// Dropout rule. Measured 2026-10-16 with playback running: "frames with no packet seen" rises with every
// control request, whatever it asks (about 0.45 per SETUP and per 8-byte packet: 5 for a read of string
// 0x49, 1 for the clock request), and hardly at all between polls. So a poll normally adds 4-8; a dropout
// is a poll that adds well beyond that: more than twice the usual count plus kMissedMargin frames.
constexpr double kDefaultUsualPerPoll = 3.0;
constexpr double kMissedMargin = 10.0;
// The counters are 16 bits and wrap after 65 s of playback: differences over a longer gap are ambiguous.
constexpr double kMaxCounterGap = 30.0;

std::wstring Upper(std::wstring s) {
  for (auto& c : s) c = static_cast<wchar_t>(std::towupper(c));
  return s;
}

std::wstring DeviceId(DEVINST devinst) {
  wchar_t id[MAX_DEVICE_ID_LEN] = {};
  return CM_Get_Device_IDW(devinst, id, MAX_DEVICE_ID_LEN, 0) == CR_SUCCESS ? id : L"";
}

// Started without a problem: problem stays 0. Otherwise problem gets the Device Manager code, or
// CM_PROB_NOT_CONFIGURED when there is none.
bool DevnodeOk(DEVINST devinst, ULONG* problem) {
  ULONG status = 0;
  *problem = 0;
  if (CM_Get_DevNode_Status(&status, problem, devinst, 0) != CR_SUCCESS) {
    *problem = CM_PROB_NOT_CONFIGURED;
    return false;
  }
  if (status & DN_HAS_PROBLEM) return false;
  *problem = 0;
  if (!(status & DN_STARTED)) {
    *problem = CM_PROB_NOT_CONFIGURED;
    return false;
  }
  return true;
}

uint64_t SystemNow() {
  FILETIME now;
  GetSystemTimeAsFileTime(&now);
  return static_cast<uint64_t>(now.dwHighDateTime) << 32 | now.dwLowDateTime;
}

uint64_t LastArrival(DEVINST devinst) {
  DEVPROPTYPE type = 0;
  FILETIME time{};
  ULONG size = sizeof time;
  if (CM_Get_DevNode_PropertyW(devinst, &kLastArrivalKey, &type, reinterpret_cast<PBYTE>(&time), &size, 0) !=
          CR_SUCCESS ||
      type != DEVPROP_TYPE_FILETIME) {
    return 0;
  }
  return static_cast<uint64_t>(time.dwHighDateTime) << 32 | time.dwLowDateTime;
}

// bcdDevice from the hardware IDs' REV_xxxx.
uint16_t FirmwareRevision(DEVINST devinst) {
  ULONG size = 0;
  CM_Get_DevNode_Registry_PropertyW(devinst, CM_DRP_HARDWAREID, nullptr, nullptr, &size, 0);
  if (!size) return 0;
  std::wstring ids(size / sizeof(wchar_t) + 1, L'\0');
  if (CM_Get_DevNode_Registry_PropertyW(devinst, CM_DRP_HARDWAREID, nullptr, ids.data(), &size, 0) != CR_SUCCESS) {
    return 0;
  }
  for (const wchar_t* id = ids.c_str(); *id; id += wcslen(id) + 1) {
    std::wstring upper = Upper(id);
    size_t at = upper.find(L"&REV_");
    if (at != std::wstring::npos) return static_cast<uint16_t>(wcstoul(upper.c_str() + at + 5, nullptr, 16));
  }
  return 0;
}

// This hub or another copy of it, such as a --snapshot run next to the one in the tray. Windows reports a
// running exe's current name, so a hub moved aside for a rebuild (mbox2hub.exe.old) counts too.
bool IsHubProcess(DWORD process) {
  if (process == GetCurrentProcessId()) return true;
  HANDLE handle = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, process);
  if (!handle) return false;
  wchar_t path[1024];
  DWORD size = ARRAYSIZE(path);
  bool hub = false;
  if (QueryFullProcessImageNameW(handle, 0, path, &size)) {
    const wchar_t* slash = wcsrchr(path, L'\\');
    hub = _wcsnicmp(slash ? slash + 1 : path, L"mbox2hub.exe", 12) == 0;
  }
  CloseHandle(handle);
  return hub;
}

std::string Printf(const char* format, ...) {
  char buffer[256];
  va_list args;
  va_start(args, format);
  vsnprintf(buffer, sizeof buffer, format, args);
  va_end(args);
  return buffer;
}

}  // namespace

void DeviceLink::Start(std::function<void()> changed, LogFn log, std::function<void(const Dropout&)> dropout) {
  Stop();
  changed_ = std::move(changed);
  log_ = std::move(log);
  on_dropout_ = std::move(dropout);
  QueryPerformanceFrequency(&frequency_);
  stop_event_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
  wake_event_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
  thread_ = std::thread(&DeviceLink::Run, this);
}

void DeviceLink::Stop() {
  if (stop_event_) SetEvent(stop_event_);
  if (thread_.joinable()) thread_.join();
  for (HANDLE* event : {&stop_event_, &wake_event_}) {
    if (*event) {
      CloseHandle(*event);
      *event = nullptr;
    }
  }
}

DeviceLink::Status DeviceLink::GetStatus() const {
  std::lock_guard lock(mutex_);
  return status_;
}

bool DeviceLink::StartInstall(FirmwareImage image) {
  {
    std::lock_guard lock(mutex_);
    if (install_pending_ || status_.install.state == Install::State::Running) return false;
    install_pending_ = true;
    pending_image_ = std::move(image);
    status_.install = {};
    status_.install.state = Install::State::Running;
    status_.install.stage = "Starting";
    status_.install.version = pending_image_.version;
    status_.install.replug = pending_image_.NeedsReplug();
    status_.can_reset = false;
  }
  SetEvent(wake_event_);
  return true;
}

bool DeviceLink::ResetConverters() {
  {
    std::lock_guard lock(mutex_);
    if (!status_.can_reset || reset_pending_ || install_pending_) return false;
    reset_pending_ = true;
  }
  SetEvent(wake_event_);
  return true;
}

void DeviceLink::Changed() {
  if (changed_) changed_();
}

double DeviceLink::Seconds(LARGE_INTEGER from, LARGE_INTEGER to) const {
  return static_cast<double>(to.QuadPart - from.QuadPart) / frequency_.QuadPart;
}

void DeviceLink::Run() {
  CoInitializeEx(nullptr, COINIT_MULTITHREADED);
  for (;;) {
    bool install, reset;
    {
      std::lock_guard lock(mutex_);
      install = install_pending_;
      reset = reset_pending_;
    }
    if (install) {
      RunInstall();
    } else {
      if (reset) RunConverterReset();
      Poll();
    }
    HANDLE events[2] = {stop_event_, wake_event_};
    if (WaitForMultipleObjects(2, events, FALSE, kPollMs) == WAIT_OBJECT_0) break;
  }
  CoUninitialize();
}

void DeviceLink::ScanPnp(Status* s, uint64_t* arrival) {
  *arrival = 0;
  s->presence = Presence::Absent;
  s->instance.clear();
  s->firmware = 0;
  s->audio_ok = false;
  s->audio_problem = 0;
  s->maintenance = Maintenance::Missing;
  s->maintenance_problem = 0;

  const ULONG flags = CM_GETIDLIST_FILTER_ENUMERATOR | CM_GETIDLIST_FILTER_PRESENT;
  ULONG length = 0;
  if (CM_Get_Device_ID_List_SizeW(&length, L"USB", flags) != CR_SUCCESS || length <= 1) return;
  std::wstring list(length, L'\0');
  if (CM_Get_Device_ID_ListW(L"USB", list.data(), length, flags) != CR_SUCCESS) return;
  DEVINST device = 0;
  for (const wchar_t* id = list.c_str(); *id; id += wcslen(id) + 1) {
    std::wstring upper = Upper(id);
    if (upper.rfind(L"USB\\VID_0DBA&PID_3000\\", 0) == 0 && s->presence == Presence::Absent) {
      s->presence = Presence::AvidFirmware;
    } else if (upper.rfind(L"USB\\VID_0451&PID_3200\\", 0) == 0 && s->presence == Presence::Absent) {
      s->presence = Presence::BootRom;
    } else if (HasOpenFirmwareId(upper) &&
               CM_Locate_DevNodeW(&device, const_cast<wchar_t*>(id), CM_LOCATE_DEVNODE_NORMAL) == CR_SUCCESS &&
               IsOpenFirmwareMbox(device)) {  // not another gadget on pid.codes' shared test ID
      s->presence = Presence::OpenFirmware;
      s->instance = id;
      break;
    }
  }
  if (s->presence != Presence::OpenFirmware) return;

  s->firmware = FirmwareRevision(device);
  *arrival = LastArrival(device);
  bool found_audio = false;
  DEVINST child = 0;
  for (CONFIGRET cr = CM_Get_Child(&child, device, 0); cr == CR_SUCCESS; cr = CM_Get_Sibling(&child, child, 0)) {
    std::wstring id = Upper(DeviceId(child));
    if (id.find(L"&MI_00\\") != std::wstring::npos) {
      found_audio = true;
      s->audio_ok = DevnodeOk(child, &s->audio_problem);
    } else if (id.find(L"&MI_03\\") != std::wstring::npos) {
      ULONG problem = 0;
      s->maintenance = DevnodeOk(child, &problem) ? Maintenance::Ready : Maintenance::NotLoaded;
      s->maintenance_problem = problem;
    }
  }
  if (!found_audio) s->audio_problem = CM_PROB_NOT_CONFIGURED;
}

void DeviceLink::Poll() {
  Status pnp;
  uint64_t arrival = 0;
  ScanPnp(&pnp, &arrival);
  LARGE_INTEGER now;
  QueryPerformanceCounter(&now);

  bool arrived = false, left = false;
  {
    std::lock_guard lock(mutex_);
    Status& s = status_;
    arrived = pnp.presence == Presence::OpenFirmware &&
              (last_presence_ != Presence::OpenFirmware || pnp.instance != s.instance);
    left = last_presence_ == Presence::OpenFirmware && pnp.presence != Presence::OpenFirmware;
    s.presence = pnp.presence;
    s.instance = pnp.instance;
    s.firmware = pnp.firmware;
    s.audio_ok = pnp.audio_ok;
    s.audio_problem = pnp.audio_problem;
    s.maintenance = pnp.maintenance;
    s.maintenance_problem = pnp.maintenance_problem;
    const bool rebooting = GetTickCount64() - install_finished_ < kRebootGraceMs;
    if (pnp.presence != Presence::OpenFirmware) {
      s.clock_valid = s.counters_valid = s.streaming = false;
      // An unplug, or the replug that an image before v0.27 asks for, ends the install's message.
      if ((s.install.state == Install::State::Done && !rebooting) || s.install.state == Install::State::Failed) {
        s.install = {};
      }
    } else if (s.install.state == Install::State::Done && !s.install.replug && (arrived || !rebooting)) {
      s.install = {};  // back from the install's reboot: that's all v0.27+ needs
    }

    // When the Mbox connected: Windows' arrival time if the hub found it already there, else when the hub saw
    // it arrive (the later of the two, in case Windows' time lags).
    if (arrived) connected_at_ = polled_ || !arrival ? SystemNow() : arrival;
    connected_at_ = std::max(connected_at_, arrival);
    polled_ = true;
    s.can_reset = pnp.presence == Presence::OpenFirmware && pnp.firmware >= MaintenanceLink::kConvResetFirmware &&
                  pnp.maintenance == Maintenance::Ready && s.install.state != Install::State::Running &&
                  !(s.install.state == Install::State::Done && rebooting) && !install_pending_;
    s.reset_adc = SystemNow() - connected_at_ >= kAdcResetDelay;
  }
  if (arrived || left) {
    EndRun();
    have_clock_ = have_counters_ = false;
    stream_start_.QuadPart = 0;
    stream_id_ = -1;
    missed_history_.clear();
    sessions_.Reset();
  }
  if (arrived) {
    log_(Printf("Mbox connected: open firmware v%x.%02x%s", pnp.firmware >> 8, pnp.firmware & 0xFF,
                pnp.audio_ok ? "" : ", but its audio interface isn't running"));
  } else if (left) {
    log_("Mbox disconnected");
  }
  if (pnp.presence != last_presence_ && pnp.presence == Presence::AvidFirmware) log_("Mbox with Avid's firmware found");
  if (pnp.presence != last_presence_ && pnp.presence == Presence::BootRom) {
    log_("Mbox found in TI's boot mode (USB 0451:3200): the EEPROM has no valid firmware");
  }
  last_presence_ = pnp.presence;
  if (pnp.presence != Presence::OpenFirmware) {
    Changed();
    return;
  }

  // Counters and clock: through the maintenance interface if Windows loaded it, else the counters alone
  // through the USB hub.
  const bool playing = WindowsPlaying();
  Maintenance maintenance = pnp.maintenance;
  std::wstring counters;
  DWORD error = ERROR_NOT_FOUND;
  bool clock_ok = false;
  uint8_t clock[12] = {};
  ULONG clock_length = 0;
  if (maintenance == Maintenance::Ready) {
    MaintenanceLink link;
    DWORD open = link.Open();
    if (open == ERROR_ACCESS_DENIED || open == ERROR_SHARING_VIOLATION || open == ERROR_BUSY) {
      maintenance = Maintenance::InUse;
    } else if (open) {
      maintenance = Maintenance::Error;
      if (last_maintenance_ != Maintenance::Error) log_("Opening the maintenance interface failed: " + Win32Text(open));
    } else {
      clock_ok = !link.ControlIn(MaintenanceLink::kClockStat, 0, 0, clock, sizeof clock, &clock_length) &&
                 (clock_length == 8 || clock_length == 12);
      error = link.ReadString(kCountersString, &counters);
    }
  }
  if (maintenance != Maintenance::Ready) error = ReadStringThroughHub(pnp.instance, kCountersString, &counters);
  if (maintenance != last_maintenance_) {
    if (maintenance == Maintenance::NotLoaded) {
      log_(Printf("Windows didn't load the Mbox's maintenance interface (Device Manager code %lu); "
                  "replugging the Mbox usually fixes it",
                  pnp.maintenance_problem));
    }
    if (maintenance == Maintenance::Ready && last_maintenance_ != Maintenance::Missing) {
      log_("Maintenance interface ready");
    }
    last_maintenance_ = maintenance;
  }
  {
    std::lock_guard lock(mutex_);
    status_.maintenance = maintenance;
    if (!clock_ok) status_.clock_valid = false;
    if (error) status_.counters_valid = false;
  }
  ReadWindowsRates();
  if (clock_ok) {
    TakeClock(clock, clock_length, now);
  } else {
    have_clock_ = false;
  }
  if (!error) {
    TakeCounters(counters, now, playing);
  } else {
    have_counters_ = false;
  }
  Changed();
}

void DeviceLink::TakeClock(const uint8_t* r, ULONG length, LARGE_INTEGER now) {
  const bool reported = length >= 12;
  const int rate = reported ? r[8] | r[9] << 8 | r[10] << 16 : 48000;
  ClockSample sample{static_cast<uint16_t>((r[0] | r[1] << 8) & 0x7FF), static_cast<uint16_t>(r[2] | r[3] << 8), rate,
                     now};
  int16_t trim = static_cast<int16_t>(r[4] | r[5] << 8);
  int16_t phase = static_cast<int16_t>(r[6] | r[7] << 8);
  const double nominal = kMclkPerSample * rate / 1000.0;  // MCLK cycles per frame: 12288, or 11289.6 at 44.1 kHz
  bool have_ppm = false;
  double ppm = 0.0;
  if (have_clock_ && last_clock_.rate == rate) {  // across a rate change the count mixes both frequencies
    double dt = Seconds(last_clock_.time, now);
    if (dt > 0.2 && dt < 10.0) {
      // Exact frame count near the wall-clock estimate, then the MCLK count unwrapped around its
      // nominal value (fwdebug.py drift).
      int64_t frames = static_cast<int64_t>(dt * 1000.0 + 0.5);
      frames += ((static_cast<int64_t>(sample.frame) - last_clock_.frame - frames + 1024) % 2048 + 2048) % 2048 - 1024;
      if (frames > 0) {
        int64_t expected = static_cast<int64_t>(frames * nominal);
        int64_t counts = expected + ((static_cast<int64_t>(sample.capture) - last_clock_.capture - expected + 32768) %
                                         65536 + 65536) % 65536 - 32768;
        ppm = (static_cast<double>(counts) / frames / nominal - 1.0) * 1e6;
        have_ppm = true;
      }
    }
  }
  last_clock_ = sample;
  have_clock_ = true;
  std::lock_guard lock(mutex_);
  status_.trim = trim;
  status_.phase = phase;
  status_.rate = rate;
  status_.rate_reported = reported;
  status_.streams = reported ? r[11] : 0;
  if (have_ppm) {
    status_.clock_ppm = ppm;
    status_.clock_valid = true;
  }
}

// The rate of each Mbox endpoint's shared-mode format, as Windows' audio engine will open it.
void DeviceLink::ReadWindowsRates() {
  int rates[2] = {};
  if (enumerator_) {
    for (int i = 0; i < 2; ++i) {
      Microsoft::WRL::ComPtr<IMMDevice> device = FindMboxEndpoint(enumerator_.Get(), i ? eCapture : eRender);
      Microsoft::WRL::ComPtr<IPropertyStore> store;
      PROPVARIANT value;
      PropVariantInit(&value);
      if (device && SUCCEEDED(device->OpenPropertyStore(STGM_READ, &store)) &&
          SUCCEEDED(store->GetValue(kDeviceFormatKey, &value)) && value.vt == VT_BLOB &&
          value.blob.cbSize >= sizeof(WAVEFORMATEX)) {
        rates[i] = static_cast<int>(reinterpret_cast<const WAVEFORMATEX*>(value.blob.pBlobData)->nSamplesPerSec);
      }
      PropVariantClear(&value);
    }
  }
  std::lock_guard lock(mutex_);
  status_.windows_play_rate = rates[0];
  status_.windows_rec_rate = rates[1];
}

// Whether any program has an active audio session on the Mbox's speakers. When the last one stops,
// Windows keeps the Mbox's playback interface on for a few seconds with no packets (it pauses the stream
// before closing it), and does the same for several seconds after the Mbox is plugged in: that's not a
// dropout.
bool DeviceLink::WindowsPlaying() {
  uint64_t service = AudioServiceInstance();
  if (service != audio_service_) {
    sessions_.Reset();
    enumerator_.Reset();
    audio_service_ = service;
  }
  if (!enumerator_ &&
      FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&enumerator_)))) {
    return false;
  }
  if (!sessions_) {
    Microsoft::WRL::ComPtr<IMMDevice> speakers = FindMboxEndpoint(enumerator_.Get(), eRender);
    if (!speakers ||
        FAILED(speakers->Activate(__uuidof(IAudioSessionManager2), CLSCTX_ALL, nullptr, &sessions_))) {
      sessions_.Reset();
      return false;
    }
  }
  Microsoft::WRL::ComPtr<IAudioSessionEnumerator> list;
  if (FAILED(sessions_->GetSessionEnumerator(&list))) {
    sessions_.Reset();  // the endpoint went away; find it again next time
    return false;
  }
  int count = 0;
  list->GetCount(&count);
  for (int i = 0; i < count; ++i) {
    Microsoft::WRL::ComPtr<IAudioSessionControl> session;
    Microsoft::WRL::ComPtr<IAudioSessionControl2> session2;
    AudioSessionState state = AudioSessionStateInactive;
    DWORD process = 0;
    if (FAILED(list->GetSession(i, &session)) || FAILED(session->GetState(&state)) || state != AudioSessionStateActive) {
      continue;
    }
    // A hub's loopback capture of the speakers (analysis views, a --snapshot run) plays nothing, though it
    // starts the playback stream without packets.
    if (SUCCEEDED(session.As(&session2)) && SUCCEEDED(session2->GetProcessId(&process)) && process &&
        IsHubProcess(process)) {
      continue;
    }
    return true;
  }
  return false;
}

// String 0x49: SOFs, frames with 0 / 1 / 2+ packets seen, overflows, timer ticks per frame, bin shift,
// stream number (diag.c). Firmware before v0.23 sends only the first seven.
void DeviceLink::TakeCounters(const std::wstring& text, LARGE_INTEGER now, bool playing) {
  unsigned v[8] = {};
  int fields = swscanf_s(text.c_str(), L"%x %x %x %x %x %x %x %x", &v[0], &v[1], &v[2], &v[3], &v[4], &v[5],
                         &v[6], &v[7]);
  if (fields < 8) {
    have_counters_ = false;
    std::lock_guard lock(mutex_);
    status_.counters_valid = false;
    status_.counters_old = fields == 7;  // they stop at FFFF and nothing marks a new stream
    return;
  }
  CounterSample c{static_cast<uint16_t>(v[0]), static_cast<uint16_t>(v[1]), static_cast<uint16_t>(v[4]),
                  static_cast<uint8_t>(v[7]), now, playing};
  const CounterSample p = last_counters_;
  const bool had = have_counters_ && Seconds(p.time, now) < kMaxCounterGap;
  last_counters_ = c;
  have_counters_ = true;

  // The counters restart when playback starts (SET_INTERFACE), and the stream number moves on: sofs is the
  // time since then (exact here, since the last poll was under kMaxCounterGap ago).
  const bool restarted = had && c.stream != p.stream;
  if (restarted) {
    EndRun();
    stream_start_.QuadPart = now.QuadPart - static_cast<LONGLONG>(c.sofs) * frequency_.QuadPart / 1000;
    stream_id_ = c.stream;
    log_("Playback stream started");
  } else if (!had && c.stream != stream_id_) {
    stream_start_.QuadPart = 0;  // a stream that started while the hub wasn't watching
    stream_id_ = c.stream;
  }

  double rate = 0.0, usual = 0.0;
  bool streaming, paused = false;
  {
    std::lock_guard lock(mutex_);
    streaming = status_.streaming;
    paused = status_.paused;
  }
  if (restarted) streaming = true;
  bool bad = false;
  int d_missed = 0, d_ovf = 0;
  double dt = 0.0, usual_rate = kDefaultUsualPerPoll * 1000.0 / kPollMs;
  if (had && !restarted) {
    dt = Seconds(p.time, now);
    // Differences modulo 10000h: the counters wrap.
    const int d_sofs = static_cast<uint16_t>(c.sofs - p.sofs);
    d_missed = static_cast<uint16_t>(c.missed - p.missed);
    d_ovf = static_cast<uint16_t>(c.overflows - p.overflows);
    streaming = d_sofs > 0;  // the firmware counts SOFs only while the playback interface is on
    paused = streaming && d_sofs >= 100 && d_missed >= 0.9 * d_sofs;
    if (missed_history_.size() >= 5) {
      std::vector<double> sorted(missed_history_.begin(), missed_history_.end());
      std::nth_element(sorted.begin(), sorted.begin() + sorted.size() / 2, sorted.end());
      usual_rate = sorted[sorted.size() / 2];
    }
    if (streaming && dt > 0.1) {
      rate = d_missed / dt;
      usual = usual_rate;
    }
    // Judged only while Windows was playing at both ends of the interval, and only with a dropout callback.
    if (on_dropout_ && streaming && dt > 0.1 && playing && p.playing) {
      bad = d_missed > 2.0 * usual_rate * dt + kMissedMargin || d_ovf > 0;
      if (!bad) {
        missed_history_.push_back(rate);
        if (missed_history_.size() > kHistory) missed_history_.pop_front();
      }
    }
    std::lock_guard lock(mutex_);
    status_.overflows += d_ovf;
  }

  {
    std::lock_guard lock(mutex_);
    Status& s = status_;
    s.counters_valid = true;
    s.counters_old = false;
    s.sofs = c.sofs;
    s.missed = c.missed;
    s.one = static_cast<uint16_t>(v[2]);
    s.two_plus = static_cast<uint16_t>(v[3]);
    s.overflow_count = c.overflows;
    s.ticks_per_frame = static_cast<uint16_t>(v[5]);
    s.bin_shift = static_cast<uint8_t>(v[6]);
    s.stream = c.stream;
    s.streaming = streaming;
    s.paused = streaming && paused;
    s.playing = playing;
    s.stream_seconds = streaming && stream_start_.QuadPart ? Seconds(stream_start_, now) : -1.0;
    s.missed_rate = rate;
    s.missed_usual = usual;
  }

  if (!bad) {
    EndRun();
    return;
  }
  // Consecutive bad polls are one dropout: logged and notified when it starts, summed up when it ends.
  const bool start = !run_.active;
  if (start) run_ = {};
  run_.active = true;
  ++run_.polls;
  run_.missed += d_missed;
  run_.overflows += d_ovf;
  run_.seconds += dt;
  run_.usual = usual_rate;
  run_.all_missing = (start || run_.all_missing) && paused;
  std::string entry = RunText(dt);
  if (start) {
    AddDropout(entry);
  } else {
    std::lock_guard lock(mutex_);
    if (!status_.recent.empty()) status_.recent.back().text = entry;
  }
}

std::string DeviceLink::RunText(double last_dt) const {
  std::string text;
  if (run_.all_missing && run_.missed) {
    text = Printf("no packets for %.0f s while Windows was playing", run_.seconds);
  } else if (run_.missed > 2.0 * run_.usual * run_.seconds + kMissedMargin) {
    text = run_.polls == 1 ? Printf("%d frames without a packet in %.1f s (usually %.0f)", run_.missed, last_dt,
                                    run_.usual * last_dt)
                           : Printf("%d frames without a packet over %.0f s (usually %.0f/s)", run_.missed,
                                    run_.seconds, run_.usual);
  }
  if (run_.overflows) {
    if (!text.empty()) text += "; ";
    text += Printf("%d buffer overflow%s", run_.overflows, run_.overflows == 1 ? "" : "s");
  }
  if (text.empty()) text = Printf("%d frames without a packet over %.0f s", run_.missed, run_.seconds);
  return text;
}

void DeviceLink::EndRun() {
  if (!run_.active) return;
  run_.active = false;
  if (run_.polls < 2 || !on_dropout_) return;
  Dropout summary;
  GetLocalTime(&summary.time);
  summary.text = Printf("%s (ended after %.0f s)", RunText(0.0).c_str(), run_.seconds);
  summary.continuation = true;
  on_dropout_(summary);
}

void DeviceLink::AddDropout(const std::string& text) {
  Dropout event;
  GetLocalTime(&event.time);
  event.text = text;
  {
    std::lock_guard lock(mutex_);
    ++status_.dropouts;
    status_.recent.push_back(event);
    if (status_.recent.size() > kMaxRecent) status_.recent.erase(status_.recent.begin());
  }
  if (on_dropout_) on_dropout_(event);
}

void DeviceLink::RunInstall() {
  FirmwareImage image;
  {
    std::lock_guard lock(mutex_);
    image = pending_image_;
    install_pending_ = false;
  }
  log_(Printf("Installing firmware v%d.%02d (%u bytes of code)", image.version / 100, image.version % 100, image.size));
  MaintenanceLink link;
  std::string error;
  if (DWORD open = link.Open()) {
    error = "Opening the maintenance interface failed: " + Win32Text(open);
  } else {
    EepromWriter writer(link);
    ULONGLONG last_report = 0;
    error = writer.Install(image, [&](const char* stage, int done, int total) {
      {
        std::lock_guard lock(mutex_);
        status_.install.stage = stage;
        status_.install.done = done;
        status_.install.total = total;
      }
      ULONGLONG tick = GetTickCount64();
      if (tick - last_report >= 100 || done == total) {
        last_report = tick;
        Changed();
      }
    });
  }
  link.Close();
  {
    std::lock_guard lock(mutex_);
    status_.install.state = error.empty() ? Install::State::Done : Install::State::Failed;
    status_.install.error = error;
    install_finished_ = GetTickCount64();
  }
  log_(!error.empty()           ? "Firmware install failed: " + error
       : image.NeedsReplug() ? "Firmware installed and the Mbox rebooted; it needs a replug before listening"
                             : "Firmware installed and the Mbox rebooted into it");
  // The counters were disturbed by the install's own traffic, and the Mbox restarts.
  have_clock_ = have_counters_ = false;
  run_ = {};
  missed_history_.clear();
  Changed();
}

// The firmware sets the reset lines and answers at once; the converters are back about 40 ms later (20 ms held,
// then 20 ms before the outputs come on), counted in SOFs.
void DeviceLink::RunConverterReset() {
  {
    std::lock_guard lock(mutex_);
    reset_pending_ = false;
  }
  const bool adc = SystemNow() - connected_at_ >= kAdcResetDelay;
  MaintenanceLink link;
  DWORD error = link.Open();
  // Another hub's once-a-second poll holds the interface for a few milliseconds at a time.
  for (int attempt = 0;
       attempt < 10 && (error == ERROR_ACCESS_DENIED || error == ERROR_SHARING_VIOLATION || error == ERROR_BUSY);
       ++attempt) {
    Sleep(20);
    error = link.Open();
  }
  if (!error) {
    error = link.ControlOut(MaintenanceLink::kConvReset,
                            MaintenanceLink::kConvDac | (adc ? MaintenanceLink::kConvAdc : 0), 0);
  }
  link.Close();
  const std::string text = error ? Win32Text(error) : "";
  {
    std::lock_guard lock(mutex_);
    ++status_.reset.count;
    status_.reset.adc = adc;
    status_.reset.error = text;
  }
  log_(error ? "Resetting the converters failed: " + text
       : adc ? "Converters reset (DAC and ADC)"
             : "DAC reset (the ADC is left alone until the Mbox has been connected for 15 s)");
  Changed();
}
