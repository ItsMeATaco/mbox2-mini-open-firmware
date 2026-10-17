// What the hub knows about the Mbox's ASIO driver (src/asio) without loading it: loading it
// would open the Mbox in exclusive mode. It reads the driver's registration, sets the buffer size the driver
// prefers, and reads the driver's log for the last time a DAW opened it.
#pragma once

#include <windows.h>

#include <string>

// The buffer sizes the hub offers: whole milliseconds at 48 kHz, as the driver needs (multiples of 48 from
// 144 to 1920, src/asio/engine.h).
inline constexpr int kDriverBufferChoices[5] = {144, 192, 240, 288, 480};

struct AsioDriver {
  enum class State {
    NotInstalled,  // no HKLM\SOFTWARE\ASIO\Mbox 2 Mini (open firmware)
    OtherClass,    // that key names another CLSID
    NoServer,      // the CLSID has no InprocServer32 path
    DllMissing,    // the path names no file
    Installed,
  };
  State state = State::NotInstalled;
  std::wstring dll;  // InprocServer32
};

AsioDriver FindAsioDriver();

// The size the driver reports as preferred in getBufferSize(): HKCU\Software\Mbox2Asio BufferFrames, clamped
// and rounded as the driver does, or its default (192) without the value. `set`: whether the value exists.
int ReadDriverBuffer(bool* set = nullptr);
bool WriteDriverBuffer(int frames);

// The last open of the driver in %LOCALAPPDATA%\Mbox2Asio\driver.log, by any program.
struct DriverSession {
  bool found = false;
  SYSTEMTIME time{};
  std::wstring program;  // "Ableton Live 12 Suite"
  DWORD pid = 0;
  int frames = 0;
  int rate = 48000;  // the driver logs it since it offers 44.1 kHz too
  int input_latency = -1, output_latency = -1;  // as reported when it opened
  long long underruns = -1, late = -1, capture_gaps = -1;  // from its close line; -1 before that
  bool device_lost = false;
  bool closed = false;
  bool open_now = false;  // not closed, and the program is still running
};

// A failed open after the last successful one.
struct DriverFailure {
  bool found = false;
  SYSTEMTIME time{};
  std::wstring program;
  std::wstring error;  // the message the DAW shows
};

// Reads the end of the log again when the file changes. Never writes to it.
class DriverLog {
 public:
  void Refresh();
  const DriverSession& Session() const { return session_; }
  const DriverFailure& Failure() const { return failure_; }

 private:
  void Parse();

  std::wstring path_;
  ULONGLONG size_ = 0;
  FILETIME time_{};
  DriverSession session_;
  DriverFailure failure_;
};
