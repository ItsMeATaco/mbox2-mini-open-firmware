// Watches the Mbox from a background thread, about once a second: whether it's plugged in and with which
// firmware, and, through the open firmware's maintenance channel (hub/maintenance.h), its clock lock and
// stream counters. Spots dropouts in the counters and installs firmware images.
//
// Audio itself no longer goes through the hub: with the open firmware the Mbox is a USB Audio Class
// device that Windows' own driver (usbaudio.sys) plays to and records from.
#pragma once

#include <windows.h>
#include <audiopolicy.h>
#include <mmdeviceapi.h>
#include <wrl/client.h>

#include <deque>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "hub/eeprom.h"

class DeviceLink {
 public:
  enum class Presence {
    Absent,
    OpenFirmware,  // the open USB Audio Class firmware: one of kOpenFirmwareIds and its name (hub/usb_ids.h)
    AvidFirmware,  // USB 0DBA:3000, Avid's firmware
    BootRom,       // USB 0451:3200, TI's boot ROM: the EEPROM has no valid image
  };
  enum class Maintenance {
    Missing,    // no maintenance interface (firmware older than v0.19)
    NotLoaded,  // Windows didn't start WinUSB on it (maintenance_problem has the Device Manager code)
    InUse,      // another program has it open
    Ready,
    Error,
  };

  struct Dropout {
    SYSTEMTIME time;  // local time
    std::string text;
    bool continuation = false;  // the summary of a dropout that lasted several polls (log it, don't notify)
  };

  struct Install {
    enum class State { Idle, Running, Done, Failed };
    State state = State::Idle;
    std::string stage;
    int done = 0, total = 0;
    std::string error;
    uint8_t version = 0;  // image version being or last installed
    bool replug = true;   // that image needs a replug after its reboot (FirmwareImage::NeedsReplug)
  };

  struct ConverterReset {
    uint32_t count = 0;  // resets sent (or tried) since the hub started
    bool adc = false;    // the last one included the ADC
    std::string error;   // why the last one failed; empty if it worked
  };

  struct Status {
    Presence presence = Presence::Absent;
    std::wstring instance;  // the composite USB device's instance ID
    uint16_t firmware = 0;  // bcdDevice, from the hardware ID's REV_xxxx
    bool audio_ok = false;  // interface 0 runs on Windows' audio driver
    ULONG audio_problem = 0;
    Maintenance maintenance = Maintenance::Missing;
    ULONG maintenance_problem = 0;

    // Clock lock (vendor request 0x09), only through the maintenance interface.
    bool clock_valid = false;
    double clock_ppm = 0.0;  // MCLK against 256 x the sample rate, measured over USB frames since the last poll
    int16_t trim = 0;        // the lock's N trim, in synthesizer LSB
    int16_t phase = 0;       // the lock's accumulated phase error, in MCLK cycles

    // The sample rate the Mbox's one clock runs at, both directions (firmware v0.24+ sends it with the clock;
    // older firmware only runs 48 kHz). 0 until the first clock read.
    int rate = 0;
    bool rate_reported = false;  // the firmware reported it (otherwise 48 kHz is assumed)
    uint8_t streams = 0;         // v0.24+: bit 0 the playback interface is on, bit 1 the capture interface
    // Windows' shared-mode rates for the Mbox's speakers and microphone (Sound settings > Advanced), 0 if
    // unknown. When they differ, the stream that starts second fails while the other runs (v0.24+).
    int windows_play_rate = 0, windows_rec_rate = 0;

    // Stream counters (string 0x49, diag.c, firmware v0.23+). They restart when playback starts, which
    // changes the stream number, and wrap at 10000h: the hub works with differences between polls.
    bool counters_valid = false;
    bool counters_old = false;     // firmware before v0.23: its counters stop at FFFF, so they aren't used
    uint16_t sofs = 0, missed = 0, one = 0, two_plus = 0, overflow_count = 0, ticks_per_frame = 0;
    uint8_t bin_shift = 0, stream = 0;
    bool streaming = false;        // the playback interface is on
    bool paused = false;           // ...but no packets arrive (Windows paused the stream)
    bool playing = false;          // a program has an active audio session on the Mbox's speakers
    double stream_seconds = -1.0;  // since playback started; -1 when it started before the hub saw it
    double missed_rate = 0.0;      // frames in which no packet was seen, per second, at the last poll
    double missed_usual = 0.0;     // the background rate (median of recent polls)
    uint32_t overflows = 0;        // buffer overflows since the hub started
    uint32_t dropouts = 0;         // since the hub started
    std::vector<Dropout> recent;   // newest last

    // Converter resets (vendor request 0x0C, firmware v0.27+).
    bool can_reset = false;  // the firmware takes it, the maintenance interface is up and no install runs
    bool reset_adc = false;  // a reset now would include the ADC (see ResetConverters)
    ConverterReset reset;

    Install install;
  };

  using LogFn = std::function<void(const std::string&)>;

  ~DeviceLink() { Stop(); }

  // `changed` and `dropout` run on the link's thread. Without `dropout` the link doesn't look for dropouts.
  void Start(std::function<void()> changed, LogFn log, std::function<void(const Dropout&)> dropout);
  void Stop();
  Status GetStatus() const;

  // Installs an image (validated with LoadFirmwareImage) on the link's thread; false if one is running.
  bool StartInstall(FirmwareImage image);

  // Resets the Mbox's converters on the link's thread: the DAC, and the ADC too once the Mbox has been connected
  // for 15 s (an ADC reset sooner after power-up leaves input 2 pinned for about 8 s). False unless
  // status.can_reset. The outcome lands in status.reset.
  bool ResetConverters();

 private:
  struct ClockSample {
    uint16_t frame, capture;
    int rate;
    LARGE_INTEGER time;
  };
  struct CounterSample {
    uint16_t sofs, missed, overflows;
    uint8_t stream;
    LARGE_INTEGER time;
    bool playing;
  };
  struct DropoutRun {  // consecutive polls that looked like a dropout
    bool active = false;
    int polls = 0, missed = 0, overflows = 0;
    double seconds = 0.0, usual = 0.0;
    bool all_missing = false;
  };

  void Run();
  void Poll();
  void RunInstall();
  void RunConverterReset();
  void ScanPnp(Status* status, uint64_t* arrival);  // arrival: Windows' last arrival time of the Mbox, or 0
  void TakeClock(const uint8_t* reply, ULONG length, LARGE_INTEGER now);  // 8 bytes (up to v0.23) or 12
  void ReadWindowsRates();
  void TakeCounters(const std::wstring& text, LARGE_INTEGER now, bool playing);
  bool WindowsPlaying();
  void AddDropout(const std::string& text);
  void EndRun();
  std::string RunText(double last_dt) const;
  double Seconds(LARGE_INTEGER from, LARGE_INTEGER to) const;
  void Changed();

  std::function<void()> changed_;
  LogFn log_;
  std::function<void(const Dropout&)> on_dropout_;
  HANDLE stop_event_ = nullptr;
  HANDLE wake_event_ = nullptr;
  std::thread thread_;
  LARGE_INTEGER frequency_{};

  // Link thread only.
  bool have_clock_ = false, have_counters_ = false;
  ClockSample last_clock_{};
  CounterSample last_counters_{};
  LARGE_INTEGER stream_start_{};  // when the counters were seen to restart; 0 if not seen
  int stream_id_ = -1;            // the firmware's stream number that stream_start_ belongs to
  std::deque<double> missed_history_;  // frames without a packet per second at recent quiet polls
  DropoutRun run_;
  Microsoft::WRL::ComPtr<IMMDeviceEnumerator> enumerator_;
  Microsoft::WRL::ComPtr<IAudioSessionManager2> sessions_;
  uint64_t audio_service_ = 0;  // AudioServiceInstance() when the two above were made
  Maintenance last_maintenance_ = Maintenance::Missing;
  Presence last_presence_ = Presence::Absent;
  bool polled_ = false;
  uint64_t connected_at_ = 0;  // when the Mbox last connected, as a FILETIME (UTC)

  mutable std::mutex mutex_;
  Status status_;
  bool install_pending_ = false;
  bool reset_pending_ = false;
  ULONGLONG install_finished_ = 0;
  FirmwareImage pending_image_;
};
