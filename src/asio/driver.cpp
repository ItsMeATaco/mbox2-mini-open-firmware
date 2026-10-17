#include "asio/driver.h"

#include <mmsystem.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <new>

#include "asio/log.h"
#include "asio/names.h"

namespace mbox2::asio {
namespace {

int64_t QpcNanoseconds() {
  static const int64_t frequency = [] {
    LARGE_INTEGER f;
    QueryPerformanceFrequency(&f);
    return f.QuadPart;
  }();
  LARGE_INTEGER count;
  QueryPerformanceCounter(&count);
  return count.QuadPart / frequency * 1'000'000'000 + count.QuadPart % frequency * 1'000'000'000 / frequency;
}

template <typename T>
void Split(int64_t value, T* out) {
  out->hi = static_cast<unsigned long>(static_cast<uint64_t>(value) >> 32);
  out->lo = static_cast<unsigned long>(static_cast<uint64_t>(value) & 0xFFFFFFFFu);
}

// HKCU Software\Mbox2Asio BufferFrames counts 48 kHz samples (Mbox Hub sets it). At 44.1 kHz the driver prefers the
// size it takes nearest to the same duration.
long PreferredFrames(int rate) {
  DWORD value = 0, size = sizeof value;
  long frames = kDefaultFrames;
  if (RegGetValueW(HKEY_CURRENT_USER, kSettingsKey, L"BufferFrames", RRF_RT_REG_DWORD, nullptr, &value, &size) ==
      ERROR_SUCCESS) {
    frames = std::clamp(static_cast<long>(value), static_cast<long>(kMinFrames), static_cast<long>(kMaxFrames));
    frames = frames / kGranularity * kGranularity;
  } else if (rate != kDefaultRate) {
    return BuffersAt(rate).preferred;
  }
  if (rate == kDefaultRate) return frames;
  const BufferSizes b = BuffersAt(rate);
  const long scaled = std::lround(static_cast<double>(frames) * rate / kDefaultRate);
  return std::clamp(b.min + (scaled - b.min + b.granularity / 2) / b.granularity * b.granularity, static_cast<long>(b.min),
                    static_cast<long>(b.max));
}

}  // namespace

HRESULT Driver::Create(REFIID riid, void** object) {
  auto* driver = new (std::nothrow) Driver;
  if (!driver) return E_OUTOFMEMORY;
  const HRESULT hr = driver->QueryInterface(riid, object);
  driver->Release();
  return hr;
}

Driver::Driver() { ModuleAddRef(); }

Driver::~Driver() {
  disposeBuffers();
  if (initialized_) Log("driver released");
  ModuleRelease();
}

STDMETHODIMP Driver::QueryInterface(REFIID riid, void** object) {
  if (!object) return E_POINTER;
  // Hosts ask for the driver's CLSID as the interface (the ASIO SDK's asiolist.cpp does).
  if (IsEqualIID(riid, IID_IUnknown) || IsEqualIID(riid, kClsid)) {
    *object = static_cast<IASIO*>(this);
    AddRef();
    return S_OK;
  }
  *object = nullptr;
  return E_NOINTERFACE;
}

STDMETHODIMP_(ULONG) Driver::AddRef() { return refs_.fetch_add(1) + 1; }

STDMETHODIMP_(ULONG) Driver::Release() {
  const ULONG remaining = refs_.fetch_sub(1) - 1;
  if (remaining == 0) delete this;
  return remaining;
}

ASIOBool Driver::init(void* sys_handle) {
  window_ = static_cast<HWND>(sys_handle);
  if (initialized_) return ASIOTrue;
  if (!engine_.Find(&error_)) {
    Log("init: %s", error_.c_str());
    return ASIOFalse;
  }
  initialized_ = true;
  error_.clear();
  Log("init");
  return ASIOTrue;
}

void Driver::getDriverName(char* name) { strcpy_s(name, 32, kDriverName); }

long Driver::getDriverVersion() { return kDriverVersion; }

void Driver::getErrorMessage(char* string) {
  strncpy_s(string, 124, error_.empty() ? "No error." : error_.c_str(), _TRUNCATE);
}

ASIOError Driver::start() {
  if (!callbacks_) return ASE_InvalidMode;
  if (engine_.lost() || !engine_.is_open()) {
    error_ = "The Mbox was disconnected. Reset the driver in your DAW's audio settings.";
    return ASE_HWMalfunction;
  }
  if (started_) return ASE_OK;
  half_ = 0;
  time_offset_ns_ = static_cast<int64_t>(timeGetTime()) * 1'000'000 - QpcNanoseconds();
  switch_position_ = 0;
  switch_time_ns_ = QpcNanoseconds() + time_offset_ns_;
  started_ = true;
  engine_.Start();
  Log("start");
  return ASE_OK;
}

ASIOError Driver::stop() {
  if (!started_) return ASE_OK;
  engine_.Stop();
  started_ = false;
  const Engine::Stats s = engine_.stats();
  Log("stop after %llu callbacks: %llu input underruns, %llu late periods, %llu capture gaps, max process %.0f us",
      s.callbacks, s.input_underruns, s.late_periods, s.capture_gaps, s.max_process_us);
  return ASE_OK;
}

ASIOError Driver::getChannels(long* inputs, long* outputs) {
  if (!initialized_) return ASE_NotPresent;
  if (inputs) *inputs = kChannels;
  if (outputs) *outputs = kChannels;
  return ASE_OK;
}

ASIOError Driver::getLatencies(long* input, long* output) {
  if (!initialized_) return ASE_NotPresent;
  const int frames = callbacks_ ? frames_ : PreferredFrames(rate_);
  if (input) *input = engine_.input_latency(frames, rate_);
  if (output) *output = engine_.output_latency(frames, rate_);
  return ASE_OK;
}

ASIOError Driver::getBufferSize(long* min_size, long* max_size, long* preferred, long* granularity) {
  if (!initialized_) return ASE_NotPresent;
  const BufferSizes b = BuffersAt(rate_);
  if (min_size) *min_size = b.min;
  if (max_size) *max_size = b.max;
  if (preferred) *preferred = PreferredFrames(rate_);
  if (granularity) *granularity = b.granularity;
  return ASE_OK;
}

// 48 kHz always; 44.1 kHz with firmware v0.24 or later (Engine::has_44k1()).
ASIOError Driver::canSampleRate(ASIOSampleRate rate) {
  if (!initialized_) return ASE_NotPresent;
  return IsSupportedRate(rate) && (rate == kDefaultRate || engine_.has_44k1()) ? ASE_OK : ASE_NoClock;
}

ASIOError Driver::getSampleRate(ASIOSampleRate* rate) {
  if (!initialized_) return ASE_NotPresent;
  if (!rate) return ASE_InvalidParameter;
  *rate = rate_;
  return ASE_OK;
}

// While the buffers exist the driver reopens the Mbox at the new rate (and keeps streaming if it was), then
// tells the host its latencies changed. If the buffer size doesn't exist at the new rate (192 at 44.1 kHz), it
// asks the host to start over instead, which makes it read the sizes again. DAWs usually change the rate with
// the buffers disposed.
ASIOError Driver::setSampleRate(ASIOSampleRate rate) {
  if (!initialized_) return ASE_NotPresent;
  if (canSampleRate(rate) != ASE_OK) return ASE_NoClock;
  const int new_rate = static_cast<int>(rate);
  if (new_rate == rate_) return ASE_OK;
  Log("setSampleRate %d -> %d", rate_, new_rate);
  rate_ = new_rate;
  if (!callbacks_) return ASE_OK;
  const bool was_started = started_;
  stop();
  if (!BufferAllowed(frames_, rate_)) {
    engine_.Close();
    const BufferSizes b = BuffersAt(rate_);
    char text[124];
    snprintf(text, sizeof text, "Buffer size %ld isn't supported at %d Hz: pick %d, %d, %d ... in the audio settings.", frames_,
             rate_, b.min, b.min + b.granularity, b.min + 2 * b.granularity);
    error_ = text;
    Log("setSampleRate: %s; asking the host for a reset", text);
    if (HostSupports(kAsioResetRequest)) callbacks_->asioMessage(kAsioResetRequest, 0, nullptr, nullptr);
    return ASE_OK;
  }
  if (!engine_.Open(frames_, rate_, this, &error_)) return ASE_HWMalfunction;
  error_.clear();
  if (was_started) start();
  LatenciesChanged();
  return ASE_OK;
}

ASIOError Driver::getClockSources(ASIOClockSource* clocks, long* count) {
  if (!clocks || !count || *count < 1) return ASE_InvalidParameter;
  clocks[0] = {};
  clocks[0].index = 0;
  clocks[0].associatedChannel = -1;
  clocks[0].associatedGroup = -1;
  clocks[0].isCurrentSource = ASIOTrue;
  strcpy_s(clocks[0].name, "USB");
  *count = 1;
  return ASE_OK;
}

ASIOError Driver::setClockSource(long reference) { return reference == 0 ? ASE_OK : ASE_InvalidParameter; }

ASIOError Driver::getSamplePosition(ASIOSamples* position, ASIOTimeStamp* timestamp) {
  if (!started_) return ASE_SPNotAdvancing;
  if (position) Split(switch_position_.load(), position);
  if (timestamp) Split(switch_time_ns_.load(), timestamp);
  return ASE_OK;
}

ASIOError Driver::getChannelInfo(ASIOChannelInfo* info) {
  if (!initialized_) return ASE_NotPresent;
  if (!info || info->channel < 0 || info->channel >= kChannels) return ASE_InvalidParameter;
  const bool input = info->isInput != ASIOFalse;
  info->isActive = (input ? inputs_ : outputs_)[info->channel][0] ? ASIOTrue : ASIOFalse;
  info->channelGroup = 0;
  info->type = input ? ASIOSTInt16LSB : ASIOSTInt24LSB;
  snprintf(info->name, sizeof info->name, "%s %ld", input ? "Input" : "Output", info->channel + 1);
  return ASE_OK;
}

ASIOError Driver::createBuffers(ASIOBufferInfo* infos, long count, long frames, ASIOCallbacks* callbacks) {
  if (!initialized_) return ASE_NotPresent;
  if (callbacks_) disposeBuffers();
  if (!infos || !callbacks || count < 1) return ASE_InvalidMode;
  // Periods of whole milliseconds keep both streams' packets in step. Others (256, 512) make capture packets
  // jitter by up to two periods against the render events (measured at 256: 41 input underruns in 4 minutes).
  if (!BufferAllowed(frames, rate_)) {
    const BufferSizes b = BuffersAt(rate_);
    char text[124];
    snprintf(text, sizeof text, "Buffer size %ld isn't supported at %d Hz: pick %d, %d, %d, %d ... in the audio settings.", frames,
             rate_, b.min, b.min + b.granularity, b.min + 2 * b.granularity, b.min + 3 * b.granularity);
    error_ = text;
    Log("createBuffers: %s", text);
    return ASE_InvalidMode;
  }
  for (long i = 0; i < count; ++i) {
    if (infos[i].channelNum < 0 || infos[i].channelNum >= kChannels) return ASE_InvalidMode;
  }

  size_t bytes = 0;
  for (long i = 0; i < count; ++i) {
    bytes += 2 * static_cast<size_t>(frames) * (infos[i].isInput ? kInputSampleBytes : kOutputSampleBytes);
  }
  memory_.assign(bytes, 0);
  uint8_t* next = memory_.data();
  for (long i = 0; i < count; ++i) {
    const size_t half = static_cast<size_t>(frames) * (infos[i].isInput ? kInputSampleBytes : kOutputSampleBytes);
    void* (&slot)[2] = (infos[i].isInput ? inputs_ : outputs_)[infos[i].channelNum];
    for (int h = 0; h < 2; ++h) {
      slot[h] = next;
      infos[i].buffers[h] = next;
      next += half;
    }
  }

  callbacks_ = callbacks;
  frames_ = frames;
  time_info_ = HostSupports(kAsioSupportsTimeInfo) && callbacks->asioMessage(kAsioSupportsTimeInfo, 0, nullptr, nullptr) == 1;
  if (!engine_.Open(frames, rate_, this, &error_)) {
    disposeBuffers();
    return ASE_HWMalfunction;
  }
  error_.clear();
  Log("createBuffers: %ld channels, %ld frames (%.2f ms) at %d Hz, %s; latencies in %d, out %d frames", count, frames,
      frames * 1000.0 / rate_, rate_, time_info_ ? "time info" : "plain bufferSwitch", engine_.input_latency(frames, rate_),
      engine_.output_latency(frames, rate_));
  return ASE_OK;
}

ASIOError Driver::disposeBuffers() {
  if (!callbacks_) return ASE_InvalidMode;
  stop();
  engine_.Close();
  for (auto& channel : inputs_) channel[0] = channel[1] = nullptr;
  for (auto& channel : outputs_) channel[0] = channel[1] = nullptr;
  memory_.clear();
  callbacks_ = nullptr;
  return ASE_OK;
}

ASIOError Driver::controlPanel() {
  wchar_t text[1024];
  if (callbacks_) {
    const Engine::Stats s = engine_.stats();
    swprintf_s(text,
               L"Buffer: %ld samples (%.1f ms) at %.1f kHz. Pick the size and rate in your DAW's audio settings.\n"
               L"Latency: input %d, output %d samples (%.1f ms round trip).\n\n"
               L"Since start: %llu buffers, %llu input underruns, %llu late, %llu capture gaps.\n\n"
               L"Log: %%LOCALAPPDATA%%\\%s\\driver.log",
               frames_, frames_ * 1000.0 / rate_, rate_ / 1000.0, engine_.input_latency(frames_, rate_),
               engine_.output_latency(frames_, rate_),
               (engine_.input_latency(frames_, rate_) + engine_.output_latency(frames_, rate_)) * 1000.0 / rate_, s.callbacks,
               s.input_underruns, s.late_periods, s.capture_gaps, kLogFolder);
  } else {
    const BufferSizes b = BuffersAt(rate_);
    swprintf_s(text,
               L"Buffer sizes: %d to %d samples in steps of %d (%.0f to %.0f ms) at %.1f kHz.\n"
               L"Pick the size and rate in your DAW's audio settings.\n\nLog: %%LOCALAPPDATA%%\\%s\\driver.log",
               b.min, b.max, b.granularity, b.min * 1000.0 / rate_, b.max * 1000.0 / rate_, rate_ / 1000.0, kLogFolder);
  }
  MessageBoxW(window_, text, kRegistryName, MB_OK | MB_ICONINFORMATION);
  return ASE_OK;
}

ASIOError Driver::future(long selector, void* opt) {
  switch (selector) {
    case kAsioCanTimeInfo:
      return ASE_SUCCESS;
    case kFutureGetStats:
      if (!opt) return ASE_InvalidParameter;
      *static_cast<Engine::Stats*>(opt) = engine_.stats();
      return ASE_SUCCESS;
    default:
      return ASE_InvalidParameter;
  }
}

ASIOError Driver::outputReady() { return ASE_NotPresent; }

bool Driver::HostSupports(long selector) const {
  return callbacks_ && callbacks_->asioMessage &&
         callbacks_->asioMessage(kAsioSelectorSupported, selector, nullptr, nullptr) == 1;
}

void Driver::Process(const int16_t* input, uint8_t* output, int frames, int64_t position, int64_t time_ns) {
  const int half = half_;
  for (int c = 0; c < kChannels; ++c) {
    auto* in = static_cast<int16_t*>(inputs_[c][half]);
    if (!in) continue;
    for (int f = 0; f < frames; ++f) in[f] = input[f * kChannels + c];
  }

  const int64_t system_ns = time_ns + time_offset_ns_;
  switch_position_ = position;
  switch_time_ns_ = system_ns;
  if (time_info_) {
    ASIOTime time{};
    time.timeInfo.speed = 1.0;
    Split(system_ns, &time.timeInfo.systemTime);
    Split(position, &time.timeInfo.samplePosition);
    time.timeInfo.sampleRate = rate_;
    time.timeInfo.flags = kSystemTimeValid | kSamplePositionValid | kSampleRateValid;
    callbacks_->bufferSwitchTimeInfo(&time, half, ASIOTrue);
  } else {
    callbacks_->bufferSwitch(half, ASIOTrue);
  }

  // The host filled this half's output buffers in the callback; it starts playing at the next period.
  for (int c = 0; c < kChannels; ++c) {
    const auto* out = static_cast<const uint8_t*>(outputs_[c][half]);
    uint8_t* dst = output + static_cast<size_t>(c) * kOutputSampleBytes;
    for (int f = 0; f < frames; ++f, dst += kChannels * kOutputSampleBytes) {
      if (out) {
        memcpy(dst, out + static_cast<size_t>(f) * kOutputSampleBytes, kOutputSampleBytes);
      } else {
        memset(dst, 0, kOutputSampleBytes);
      }
    }
  }
  half_ = 1 - half;
}

void Driver::LatenciesChanged() {
  if (HostSupports(kAsioLatenciesChanged)) callbacks_->asioMessage(kAsioLatenciesChanged, 0, nullptr, nullptr);
}

void Driver::DeviceLost() {
  error_ = "The Mbox was disconnected or taken over. Reset the driver in your DAW's audio settings.";
  Log("device lost; asking the host for a reset");
  if (HostSupports(kAsioResetRequest)) callbacks_->asioMessage(kAsioResetRequest, 0, nullptr, nullptr);
}

}  // namespace mbox2::asio
