// The IASIO COM object DAWs load: two inputs (16-bit) and two outputs (24-bit) at 48 or 44.1 kHz, on top of Engine.
#pragma once

#include <atomic>
#include <cstdint>
#include <string>
#include <vector>

#include "asio/asio_sdk.h"
#include "asio/engine.h"

namespace mbox2::asio {

// A private future() selector: fills an Engine::Stats (for asiotest). The value spells "MB2S".
inline constexpr long kFutureGetStats = 0x4D423253;

// Live COM objects, for DllCanUnloadNow (dll.cpp).
void ModuleAddRef();
void ModuleRelease();

class Driver final : public IASIO, private Engine::Client {
 public:
  static HRESULT Create(REFIID riid, void** object);

  STDMETHODIMP QueryInterface(REFIID riid, void** object) override;
  STDMETHODIMP_(ULONG) AddRef() override;
  STDMETHODIMP_(ULONG) Release() override;

  ASIOBool init(void* sys_handle) override;
  void getDriverName(char* name) override;
  long getDriverVersion() override;
  void getErrorMessage(char* string) override;
  ASIOError start() override;
  ASIOError stop() override;
  ASIOError getChannels(long* inputs, long* outputs) override;
  ASIOError getLatencies(long* input, long* output) override;
  ASIOError getBufferSize(long* min_size, long* max_size, long* preferred, long* granularity) override;
  ASIOError canSampleRate(ASIOSampleRate rate) override;
  ASIOError getSampleRate(ASIOSampleRate* rate) override;
  ASIOError setSampleRate(ASIOSampleRate rate) override;
  ASIOError getClockSources(ASIOClockSource* clocks, long* count) override;
  ASIOError setClockSource(long reference) override;
  ASIOError getSamplePosition(ASIOSamples* position, ASIOTimeStamp* timestamp) override;
  ASIOError getChannelInfo(ASIOChannelInfo* info) override;
  ASIOError createBuffers(ASIOBufferInfo* infos, long count, long frames, ASIOCallbacks* callbacks) override;
  ASIOError disposeBuffers() override;
  ASIOError controlPanel() override;
  ASIOError future(long selector, void* opt) override;
  ASIOError outputReady() override;

 private:
  Driver();
  ~Driver();

  void Process(const int16_t* input, uint8_t* output, int frames, int64_t position, int64_t time_ns) override;
  void LatenciesChanged() override;
  void DeviceLost() override;
  bool HostSupports(long selector) const;

  std::atomic<ULONG> refs_{1};
  Engine engine_;
  std::string error_;
  bool initialized_ = false;
  HWND window_ = nullptr;

  std::vector<uint8_t> memory_;
  void* inputs_[kChannels][2] = {};   // [channel][buffer half], null for channels the host didn't ask for
  void* outputs_[kChannels][2] = {};
  ASIOCallbacks* callbacks_ = nullptr;
  bool time_info_ = false;
  long frames_ = 0;
  int rate_ = kDefaultRate;  // the host's choice (setSampleRate); the Mbox runs at it while the buffers exist
  int half_ = 0;
  bool started_ = false;
  int64_t time_offset_ns_ = 0;  // QPC nanoseconds to timeGetTime() nanoseconds, which ASIO asks for
  std::atomic<int64_t> switch_position_{0}, switch_time_ns_{0};
};

}  // namespace mbox2::asio
