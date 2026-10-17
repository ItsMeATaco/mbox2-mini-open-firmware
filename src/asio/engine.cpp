#include "asio/engine.h"

#include <audioclient.h>
#include <avrt.h>
#include <ksmedia.h>
#include <mmdeviceapi.h>
#include <wrl/client.h>
// Defines PKEY_Device_FriendlyName in this translation unit.
#include <initguid.h>
#include <functiondiscoverykeys_devpkey.h>

#include <algorithm>
#include <cmath>
#include <cstring>

#include "asio/log.h"
#include "asio/names.h"

namespace mbox2::asio {

using Microsoft::WRL::ComPtr;

namespace {

constexpr int64_t kTicksPerSecond = 10'000'000;  // REFERENCE_TIME and WASAPI's QPC positions: 100 ns

int64_t QpcNow() {
  static const int64_t frequency = [] {
    LARGE_INTEGER f;
    QueryPerformanceFrequency(&f);
    return f.QuadPart;
  }();
  LARGE_INTEGER count;
  QueryPerformanceCounter(&count);
  return count.QuadPart / frequency * kTicksPerSecond + count.QuadPart % frequency * kTicksPerSecond / frequency;
}

DWORD ReadSetting(const wchar_t* name, DWORD fallback) {
  DWORD value = 0, size = sizeof value;
  return RegGetValueW(HKEY_CURRENT_USER, kSettingsKey, name, RRF_RT_REG_DWORD, nullptr, &value, &size) == ERROR_SUCCESS
             ? value
             : fallback;
}

std::wstring FriendlyName(IMMDevice* device) {
  ComPtr<IPropertyStore> store;
  PROPVARIANT name;
  PropVariantInit(&name);
  std::wstring result;
  if (SUCCEEDED(device->OpenPropertyStore(STGM_READ, &store)) &&
      SUCCEEDED(store->GetValue(PKEY_Device_FriendlyName, &name)) && name.vt == VT_LPWSTR) {
    result = name.pwszVal;
  }
  PropVariantClear(&name);
  return result;
}

std::wstring FindEndpoint(IMMDeviceEnumerator* enumerator, EDataFlow flow) {
  ComPtr<IMMDeviceCollection> devices;
  if (FAILED(enumerator->EnumAudioEndpoints(flow, DEVICE_STATE_ACTIVE, &devices))) return {};
  UINT count = 0;
  devices->GetCount(&count);
  for (UINT i = 0; i < count; ++i) {
    ComPtr<IMMDevice> device;
    LPWSTR id = nullptr;
    if (FAILED(devices->Item(i, &device))) continue;
    if (FriendlyName(device.Get()).find(kDeviceName) == std::wstring::npos) continue;
    if (FAILED(device->GetId(&id))) continue;
    std::wstring result = id;
    CoTaskMemFree(id);
    return result;
  }
  return {};
}

WAVEFORMATEXTENSIBLE Format(int bytes, int rate) {
  WAVEFORMATEXTENSIBLE format{};
  format.Format.wFormatTag = WAVE_FORMAT_EXTENSIBLE;
  format.Format.nChannels = kChannels;
  format.Format.nSamplesPerSec = rate;
  format.Format.wBitsPerSample = static_cast<WORD>(8 * bytes);
  format.Format.nBlockAlign = static_cast<WORD>(kChannels * bytes);
  format.Format.nAvgBytesPerSec = rate * format.Format.nBlockAlign;
  format.Format.cbSize = sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX);
  format.Samples.wValidBitsPerSample = static_cast<WORD>(8 * bytes);
  format.dwChannelMask = SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT;
  format.SubFormat = KSDATAFORMAT_SUBTYPE_PCM;
  return format;
}

// Messages end up in the DAW's driver error box: at most 123 characters.
std::string OpenError(const char* side, HRESULT hr) {
  char text[200];
  if (hr == AUDCLNT_E_DEVICE_IN_USE) {
    snprintf(text, sizeof text, "The Mbox %s are in use by another program (a DAW, Windows sounds, or Mbox Hub's open window).", side);
  } else if (hr == AUDCLNT_E_EXCLUSIVE_MODE_NOT_ALLOWED) {
    snprintf(text, sizeof text, "Exclusive mode is off for the Mbox %s (Sound settings > Properties > Advanced).", side);
  } else {
    snprintf(text, sizeof text, "Can't open the Mbox %s (error 0x%08lX).", side, static_cast<unsigned long>(hr));
  }
  return text;
}

}  // namespace

struct Engine::Wasapi {
  ComPtr<IAudioClient> render_client, capture_client;
  ComPtr<IAudioRenderClient> render;
  ComPtr<IAudioCaptureClient> capture;
  ComPtr<IAudioClock> clock;
  UINT64 clock_frequency = 0;
  HANDLE render_event = nullptr, capture_event = nullptr;

  ~Wasapi() {
    if (render_client) render_client->Stop();
    if (capture_client) capture_client->Stop();
    if (render_event) CloseHandle(render_event);
    if (capture_event) CloseHandle(capture_event);
  }
};

// The acoustic round trip (asiotest --signal chirp, 2026-10-17, firmware v0.23) was the measured input
// age plus one buffer plus 170 frames (169-174 at every buffer size from 144 to 1920, in either phase). A
// loopback can't tell which side the 170 belongs to, so it is split evenly; DAWs align recordings by the sum.
// At 44.1 kHz the same measurement gave 162 (reported minus measured +5..+10 with 85 + 85, at 441 to 1764 frames).
int ExtraInputFrames(int rate) { return static_cast<int>(ReadSetting(L"ExtraInputFrames", rate == 44100 ? 81 : 85)); }
int ExtraOutputFrames(int rate) { return static_cast<int>(ReadSetting(L"ExtraOutputFrames", rate == 44100 ? 81 : 85)); }

Engine::Engine() {
  stop_event_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
  ready_event_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
  InitializeCriticalSection(&process_lock_);
}

Engine::~Engine() {
  Close();
  DeleteCriticalSection(&process_lock_);
  CloseHandle(stop_event_);
  CloseHandle(ready_event_);
}

bool Engine::Find(std::string* error) {
  // On a thread of its own, so the host's COM apartment doesn't matter.
  std::wstring render, capture;
  bool rate_44k1 = false;
  std::thread([&] {
    const HRESULT co = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    ComPtr<IMMDeviceEnumerator> enumerator;
    if (SUCCEEDED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&enumerator)))) {
      render = FindEndpoint(enumerator.Get(), eRender);
      capture = FindEndpoint(enumerator.Get(), eCapture);
      // Firmware before v0.24 lists 48 kHz only. Asking doesn't open the device.
      ComPtr<IMMDevice> device;
      ComPtr<IAudioClient> client;
      WAVEFORMATEXTENSIBLE format = Format(kOutputSampleBytes, 44100);
      rate_44k1 = !render.empty() && SUCCEEDED(enumerator->GetDevice(render.c_str(), &device)) &&
                  SUCCEEDED(device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, &client)) &&
                  client->IsFormatSupported(AUDCLNT_SHAREMODE_EXCLUSIVE, &format.Format, nullptr) == S_OK;
    }
    enumerator.Reset();
    if (SUCCEEDED(co)) CoUninitialize();
  }).join();
  if (render.empty() || capture.empty()) {
    if (error) *error = "The Mbox 2 Mini isn't connected, or it isn't running the open firmware.";
    return false;
  }
  render_id_ = render;
  capture_id_ = capture;
  rate_44k1_ = rate_44k1;
  return true;
}

bool Engine::Open(int frames, int rate, Client* client, std::string* error) {
  Close();
  if (!Find(error)) return false;
  frames_ = frames;
  rate_ = rate;
  client_ = client;
  fifo_capacity_ = static_cast<size_t>(frames) * 8;
  fifo_.assign(fifo_capacity_ * kChannels, 0);
  block_.assign(static_cast<size_t>(frames) * kChannels, 0);
  fifo_read_ = fifo_count_ = 0;
  fifo_first_position_ = next_capture_position_ = 0;
  have_capture_ = input_primed_ = false;
  const DWORD margin = ReadSetting(L"InputMargin", 0xFFFFFFFF);
  margin_fixed_ = margin <= 2;
  margin_ = margin_fixed_ ? static_cast<int>(margin) : 0;
  last_packet_position_ = 0;
  last_packet_qpc_ = last_packet_arrival_ = 0;
  packets_since_render_ = 0;
  written_ = 0;
  last_render_ = position_ = 0;
  input_age_ = output_queue_ = target_age_ = -1;
  drift_periods_ = 0;
  watch_until_ = skip_deadline_ = 0;
  pending_skips_ = 0;
  reported_age_ = 0;
  latency_changes_ = 0;
  measured_periods_ = 0;
  warmup_ = 0;
  warmup_age_sum_ = 0;
  warmup_age_count_ = 0;
  periods_ = callbacks_ = underruns_ = input_waits_ = dropped_ = gaps_ = late_ = slack_count_ = 0;
  for (auto& a : arrivals_) a = 0;
  max_process_us_ = 0;
  slack_min_us_ = 1e9;
  slack_max_us_ = -1e9;
  slack_sum_us_ = 0;
  input_age_frames_ = output_queue_frames_ = 0;
  margin_now_ = margin_;
  late_record_count_ = 0;
  last_capture_wake_ = 0;
  capture_wakes_since_render_ = 0;
  FILETIME wall;
  GetSystemTimePreciseAsFileTime(&wall);
  qpc_to_filetime_ = static_cast<int64_t>((static_cast<uint64_t>(wall.dwHighDateTime) << 32) | wall.dwLowDateTime) - QpcNow();
  running_ = false;
  lost_ = false;
  setup_ok_ = false;
  setup_error_.clear();
  ResetEvent(stop_event_);
  ResetEvent(ready_event_);

  thread_ = std::thread(&Engine::Run, this);
  if (WaitForSingleObject(ready_event_, 3000) != WAIT_OBJECT_0) {
    Close();
    if (error) *error = "The Mbox didn't start streaming. Unplug it and plug it in again.";
    Log("open %d frames at %d Hz: no audio events within 3 s", frames, rate);
    return false;
  }
  if (!setup_ok_) {
    const std::string why = setup_error_;
    Close();
    if (error) *error = why;
    Log("open %d frames at %d Hz failed: %s", frames, rate, why.c_str());
    return false;
  }
  const Stats s = stats();
  Log("open %d frames at %d Hz: input age %d, output queue %d, margin %d, slack %.0f..%.0f us, arrivals 0:%llu 1:%llu 2:%llu "
      "3+:%llu",
      frames, rate, s.input_age_frames, s.output_queue_frames, s.input_margin, s.slack_min_us, s.slack_max_us,
      s.arrivals[0], s.arrivals[1], s.arrivals[2], s.arrivals[3]);
  return true;
}

void Engine::Start() {
  EnterCriticalSection(&process_lock_);
  position_ = 0;
  callbacks_ = 0;
  running_ = true;
  LeaveCriticalSection(&process_lock_);
}

void Engine::Stop() {
  EnterCriticalSection(&process_lock_);
  running_ = false;
  LeaveCriticalSection(&process_lock_);
}

void Engine::Close() {
  Stop();
  if (!thread_.joinable()) return;
  SetEvent(stop_event_);
  if (thread_.get_id() == std::this_thread::get_id()) {
    thread_.detach();  // the host closed the driver from inside a callback
  } else {
    thread_.join();
  }
  const Stats s = stats();
  Log("close: %llu periods, %llu input underruns, %llu waits, %llu dropped, %llu capture gaps, %llu late, %llu latency changes, "
      "max process %.0f us, "
      "margin %d, input age %d, output queue %d, slack %.0f/%.0f/%.0f us, arrivals 0:%llu 1:%llu 2:%llu 3+:%llu",
      s.periods, s.input_underruns, s.input_waits, s.input_dropped, s.capture_gaps, s.late_periods, s.latency_changes,
      s.max_process_us, s.input_margin, s.input_age_frames, s.output_queue_frames, s.slack_min_us, s.slack_mean_us, s.slack_max_us,
      s.arrivals[0], s.arrivals[1], s.arrivals[2], s.arrivals[3]);
  for (int i = 0; i < late_record_count_; ++i) {
    const LatePeriod& r = late_records_[i];
    const int64_t ticks = r.qpc + qpc_to_filetime_;
    FILETIME utc{static_cast<DWORD>(ticks), static_cast<DWORD>(static_cast<uint64_t>(ticks) >> 32)};
    SYSTEMTIME st{}, local{};
    FileTimeToSystemTime(&utc, &st);
    SystemTimeToTzSpecificLocalTime(nullptr, &st, &local);
    Log("late period at %02d:%02d:%02d.%03d%s: %.2f ms since the previous one; %d capture wake-ups and %d packets "
        "meanwhile, last capture wake-up %.2f ms before, newest packet %.2f ms old",
        local.wHour, local.wMinute, local.wSecond, local.wMilliseconds, r.running ? "" : " (not running)", r.gap_ms,
        r.capture_wakes, r.packets, r.capture_wake_ms, r.packet_age_ms);
  }
}

int Engine::input_latency(int frames, int rate) const {
  if (is_open() && frames == frames_ && rate == rate_ && reported_age_ > 0) return reported_age_ + ExtraInputFrames(rate);
  // Not measured yet: one block, plus up to one more for the phase between the streams.
  return frames + frames / 2 + ExtraInputFrames(rate);
}

int Engine::output_latency(int frames, int rate) const {
  // A block plays one period after the event that asked for it. The render clock (output_queue in the stats)
  // reads the buffer size to within a few frames, but jitters and shifts with when it is read.
  return frames + ExtraOutputFrames(rate);
}

Engine::Stats Engine::stats() const {
  Stats s;
  s.periods = periods_;
  s.callbacks = callbacks_;
  s.input_underruns = underruns_;
  s.input_waits = input_waits_;
  s.input_dropped = dropped_;
  s.capture_gaps = gaps_;
  s.late_periods = late_;
  s.max_process_us = max_process_us_;
  s.input_margin = margin_now_;
  s.latency_changes = latency_changes_;
  s.input_age_frames = input_age_frames_;
  s.output_queue_frames = output_queue_frames_;
  for (int i = 0; i < 4; ++i) s.arrivals[i] = arrivals_[i];
  const uint64_t n = slack_count_;
  s.slack_min_us = n ? slack_min_us_.load() : 0;
  s.slack_max_us = n ? slack_max_us_.load() : 0;
  s.slack_mean_us = n ? slack_sum_us_ / n : 0;
  return s;
}

bool Engine::Setup(std::string* error) {
  w_ = new Wasapi;
  ComPtr<IMMDeviceEnumerator> enumerator;
  HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&enumerator));
  ComPtr<IMMDevice> render_device, capture_device;
  if (SUCCEEDED(hr)) hr = enumerator->GetDevice(render_id_.c_str(), &render_device);
  if (SUCCEEDED(hr)) hr = enumerator->GetDevice(capture_id_.c_str(), &capture_device);
  if (SUCCEEDED(hr)) hr = render_device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, &w_->render_client);
  if (SUCCEEDED(hr)) hr = capture_device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, &w_->capture_client);
  if (FAILED(hr)) {
    *error = OpenError("endpoints", hr);
    return false;
  }

  const REFERENCE_TIME period = (static_cast<int64_t>(frames_) * kTicksPerSecond + rate_ / 2) / rate_;
  WAVEFORMATEXTENSIBLE render_format = Format(kOutputSampleBytes, rate_);
  WAVEFORMATEXTENSIBLE capture_format = Format(kInputSampleBytes, rate_);
  hr = w_->render_client->Initialize(AUDCLNT_SHAREMODE_EXCLUSIVE, AUDCLNT_STREAMFLAGS_EVENTCALLBACK, period, period,
                                     &render_format.Format, nullptr);
  if (FAILED(hr)) {
    *error = OpenError("outputs", hr);
    return false;
  }
  hr = w_->capture_client->Initialize(AUDCLNT_SHAREMODE_EXCLUSIVE, AUDCLNT_STREAMFLAGS_EVENTCALLBACK, period, period,
                                      &capture_format.Format, nullptr);
  if (FAILED(hr)) {
    *error = OpenError("inputs", hr);
    return false;
  }
  UINT32 render_frames = 0, capture_frames = 0;
  w_->render_client->GetBufferSize(&render_frames);
  w_->capture_client->GetBufferSize(&capture_frames);
  if (render_frames != static_cast<UINT32>(frames_) || capture_frames != static_cast<UINT32>(frames_)) {
    char text[128];
    snprintf(text, sizeof text, "Windows gave buffers of %u/%u frames instead of %d.", render_frames, capture_frames,
             frames_);
    *error = text;
    return false;
  }

  w_->render_event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
  w_->capture_event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
  hr = w_->render_client->SetEventHandle(w_->render_event);
  if (SUCCEEDED(hr)) hr = w_->capture_client->SetEventHandle(w_->capture_event);
  if (SUCCEEDED(hr)) hr = w_->render_client->GetService(IID_PPV_ARGS(&w_->render));
  if (SUCCEEDED(hr)) hr = w_->capture_client->GetService(IID_PPV_ARGS(&w_->capture));
  if (SUCCEEDED(hr)) hr = w_->render_client->GetService(IID_PPV_ARGS(&w_->clock));
  if (SUCCEEDED(hr)) hr = w_->clock->GetFrequency(&w_->clock_frequency);

  // Exclusive event-driven render wants its first buffer before Start().
  BYTE* data = nullptr;
  if (SUCCEEDED(hr)) hr = w_->render->GetBuffer(frames_, &data);
  if (SUCCEEDED(hr)) hr = w_->render->ReleaseBuffer(frames_, AUDCLNT_BUFFERFLAGS_SILENT);
  written_ = frames_;
  if (SUCCEEDED(hr)) hr = w_->capture_client->Start();
  if (SUCCEEDED(hr)) hr = w_->render_client->Start();
  if (FAILED(hr)) {
    *error = OpenError("endpoints", hr);
    return false;
  }
  return true;
}

void Engine::Teardown() {
  delete w_;
  w_ = nullptr;
}

void Engine::Run() {
  const HRESULT co = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
  DWORD task_index = 0;
  HANDLE mmcss = AvSetMmThreadCharacteristicsW(L"Pro Audio", &task_index);
  if (mmcss) {
    AvSetMmThreadPriority(mmcss, AVRT_PRIORITY_CRITICAL);
  } else {
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);
  }

  setup_ok_ = Setup(&setup_error_);
  if (setup_ok_) {
    // Stream silence for at least 150 ms before Open() returns: settles the input FIFO and measures latencies.
    warmup_ = std::max(16, static_cast<int>(std::ceil(0.15 * rate_ / frames_)));
    bool ready = false;
    int quiet_ms = 0;
    HANDLE waits[] = {stop_event_, w_->render_event, w_->capture_event};
    for (;;) {
      const DWORD woke = WaitForMultipleObjects(3, waits, FALSE, 100);
      if (woke == WAIT_OBJECT_0) break;
      HRESULT hr = S_OK;
      if (woke == WAIT_TIMEOUT) {
        quiet_ms += 100;
        if (quiet_ms < 2000) continue;
        hr = AUDCLNT_E_DEVICE_INVALIDATED;
        Log("no audio events for 2 s");
      } else if (woke == WAIT_OBJECT_0 + 1 || woke == WAIT_OBJECT_0 + 2) {
        quiet_ms = 0;
        const int64_t now = QpcNow();
        if (woke == WAIT_OBJECT_0 + 2) {
          last_capture_wake_ = now;
          ++capture_wakes_since_render_;
        }
        hr = DrainCapture(now);
        if (SUCCEEDED(hr) && woke == WAIT_OBJECT_0 + 1) hr = RenderPeriod(now);
      } else {
        hr = HRESULT_FROM_WIN32(GetLastError());
      }
      if (FAILED(hr)) {
        Log("stream stopped: 0x%08lX", static_cast<unsigned long>(hr));
        if (ready) {
          lost_ = true;
          Stop();
          if (client_) client_->DeviceLost();
        } else {
          setup_ok_ = false;
          setup_error_ = OpenError("endpoints", hr);
        }
        break;
      }
      if (!ready && measured_periods_ >= warmup_) {
        ready = true;
        // The mean over the second half of the warm-up; the first periods still carry the streams' start.
        target_age_ = warmup_age_count_ ? warmup_age_sum_ / warmup_age_count_ : input_age_;
        reported_age_ = static_cast<int>(std::lround(target_age_));
        SetEvent(ready_event_);
      }
    }
  }
  Teardown();
  SetEvent(ready_event_);
  if (mmcss) AvRevertMmThreadCharacteristics(mmcss);
  if (SUCCEEDED(co)) CoUninitialize();
}

void Engine::PushInput(const int16_t* data, uint32_t frames) {
  for (uint32_t done = 0; done < frames;) {
    if (fifo_count_ == fifo_capacity_) {  // full: drop the oldest frame
      fifo_read_ = (fifo_read_ + 1) % fifo_capacity_;
      --fifo_count_;
      ++fifo_first_position_;
    }
    const size_t write = (fifo_read_ + fifo_count_) % fifo_capacity_;
    const size_t run = std::min<size_t>({frames - done, fifo_capacity_ - write, fifo_capacity_ - fifo_count_});
    if (data) {
      memcpy(&fifo_[write * kChannels], data + static_cast<size_t>(done) * kChannels, run * kChannels * sizeof(int16_t));
    } else {
      memset(&fifo_[write * kChannels], 0, run * kChannels * sizeof(int16_t));
    }
    fifo_count_ += run;
    done += static_cast<uint32_t>(run);
  }
}

HRESULT Engine::DrainCapture(int64_t now) {
  for (;;) {
    UINT32 packet = 0;
    HRESULT hr = w_->capture->GetNextPacketSize(&packet);
    if (FAILED(hr)) return hr;
    if (!packet) return S_OK;
    BYTE* data = nullptr;
    UINT32 frames = 0;
    DWORD flags = 0;
    UINT64 position = 0, qpc = 0;
    hr = w_->capture->GetBuffer(&data, &frames, &flags, &position, &qpc);
    if (FAILED(hr)) return hr;
    if (hr == AUDCLNT_S_BUFFER_EMPTY) return S_OK;

    if (have_capture_ && position != next_capture_position_) {
      // Lost or repeated frames: fill a short gap with silence so later blocks keep their timing; restart
      // the FIFO after anything odder.
      gaps_ = gaps_ + 1;
      watch_until_ = periods_ + kWatchSeconds * rate_ / frames_;
      const int64_t gap = static_cast<int64_t>(position - next_capture_position_);
      if (gap > 0 && gap < static_cast<int64_t>(fifo_capacity_)) {
        if (fifo_count_) PushInput(nullptr, static_cast<uint32_t>(gap));
      } else {
        fifo_count_ = 0;
        input_primed_ = false;
      }
    } else if (have_capture_ && (flags & AUDCLNT_BUFFERFLAGS_DATA_DISCONTINUITY)) {
      gaps_ = gaps_ + 1;
    }
    if (fifo_count_ == 0) fifo_first_position_ = position;
    PushInput(flags & AUDCLNT_BUFFERFLAGS_SILENT ? nullptr : reinterpret_cast<const int16_t*>(data), frames);
    have_capture_ = true;
    next_capture_position_ = position + frames;
    last_packet_position_ = position;
    last_packet_frames_ = frames;
    last_packet_qpc_ = static_cast<int64_t>(qpc);
    last_packet_arrival_ = now;
    ++packets_since_render_;
    hr = w_->capture->ReleaseBuffer(frames);
    if (FAILED(hr)) return hr;
  }
}

const int16_t* Engine::TakeInput(int64_t now, bool running) {
  const size_t block = static_cast<size_t>(frames_);
  auto drop = [&](size_t n) {
    fifo_read_ = (fifo_read_ + n) % fifo_capacity_;
    fifo_count_ -= n;
    fifo_first_position_ += n;
  };
  // Age of the oldest frame in the FIFO. usbaudio.sys stamps a capture packet when it completes, not when
  // its first frame came in (measured: the round trip is a whole packet longer than the stamp says).
  auto oldest_age = [&] {
    const int64_t newest_end = static_cast<int64_t>(last_packet_position_ + last_packet_frames_);
    return static_cast<double>(now - last_packet_qpc_) * rate_ / kTicksPerSecond +
           static_cast<double>(newest_end - static_cast<int64_t>(fifo_first_position_));
  };
  if (input_primed_ && fifo_count_ > (margin_ + 2) * block) {
    drop(fifo_count_ - (margin_ + 1) * block);
    if (running) dropped_ = dropped_ + 1;
  }
  // When the render side misses periods (its event came late), their capture packets stay in the FIFO and
  // every later block would be that much older than the latency the DAW was told. Drop them, but only down
  // to the FIFO's steady-state depth, so packets that merely came early (jitter) are kept.
  while (pending_skips_ > 0 && input_primed_ && fifo_count_ >= (margin_ + 2) * block) {
    drop(block);
    --pending_skips_;
    if (running) dropped_ = dropped_ + 1;
  }
  if (pending_skips_ > 0 && periods_ >= skip_deadline_) pending_skips_ = 0;
  if (!input_primed_) {
    if (fifo_count_ < (margin_ + 1) * block) {
      std::fill(block_.begin(), block_.end(), int16_t{0});
      return block_.data();
    }
    drop(fifo_count_ - (margin_ + 1) * block);
    input_primed_ = true;
  } else if (fifo_count_ < block) {
    // The capture packet for this period isn't in yet: keep one more block from now on.
    if (running) underruns_ = underruns_ + 1;
    input_primed_ = false;
    watch_until_ = periods_ + kWatchSeconds * rate_ / frames_;
    if (!margin_fixed_ && margin_ < 2) {
      ++margin_;
      margin_now_ = margin_;
      if (target_age_ > 0) {
        target_age_ += static_cast<double>(block);
        reported_age_ = static_cast<int>(std::lround(target_age_));
      }
      Log("input underrun: keeping %d extra block(s) of input", margin_);
      if (running && client_) client_->LatenciesChanged();
    }
    std::fill(block_.begin(), block_.end(), int16_t{0});
    return block_.data();
  }

  const double age = oldest_age();
  input_age_ = input_age_ < 0 ? age : input_age_ + (age - input_age_) / 16;
  if (target_age_ < 0 && measured_periods_ >= warmup_ / 2) {
    warmup_age_sum_ += age;
    ++warmup_age_count_;
  }
  input_age_frames_ = static_cast<int>(std::lround(input_age_));

  const size_t first = std::min(block, fifo_capacity_ - fifo_read_);
  memcpy(block_.data(), &fifo_[fifo_read_ * kChannels], first * kChannels * sizeof(int16_t));
  if (first < block) memcpy(block_.data() + first * kChannels, fifo_.data(), (block - first) * kChannels * sizeof(int16_t));
  drop(block);
  return block_.data();
}

void Engine::CheckLatencies(bool running) {
  // Both streams run off the USB clock, so their phase only moves after a glitch: look for a new phase in the
  // seconds after one, and nowhere else (capture timestamps jitter at some sizes).
  if (target_age_ < 0 || periods_ >= watch_until_) {
    drift_periods_ = 0;
    return;
  }
  // The means smooth over 16 measurements; 64 periods off by more than half a millisecond is a new phase.
  const bool off = std::abs(input_age_ - target_age_) > 24;
  drift_periods_ = off ? drift_periods_ + 1 : 0;
  if (drift_periods_ < 64) return;
  drift_periods_ = 0;
  Log("input latency moved: input age %.0f -> %.0f frames", target_age_, input_age_);
  target_age_ = input_age_;
  reported_age_ = static_cast<int>(std::lround(target_age_));
  latency_changes_ = latency_changes_ + 1;
  if (running && client_) client_->LatenciesChanged();
}

void Engine::MeasureOutput(int64_t now) {
  UINT64 position = 0, qpc = 0;
  if (!w_->clock_frequency || FAILED(w_->clock->GetPosition(&position, &qpc))) return;
  const double played = static_cast<double>(position) * rate_ / static_cast<double>(w_->clock_frequency) +
                        static_cast<double>(now - static_cast<int64_t>(qpc)) * rate_ / kTicksPerSecond;
  const double queued = static_cast<double>(written_) - played;
  output_queue_ = output_queue_ < 0 ? queued : output_queue_ + (queued - output_queue_) / 16;
  output_queue_frames_ = static_cast<int>(std::lround(output_queue_));
}

HRESULT Engine::RenderPeriod(int64_t now) {
  // The capture packet can complete within microseconds of the render event (slack measured down to -4 us
  // at 144 frames). If it isn't in yet, give it up to half a millisecond before calling it an underrun.
  if (input_primed_ && fifo_count_ < static_cast<size_t>(frames_)) {
    const int64_t deadline = now + kTicksPerSecond / 2000;
    while (fifo_count_ < static_cast<size_t>(frames_) && QpcNow() < deadline) {
      YieldProcessor();
      const HRESULT hr = DrainCapture(QpcNow());
      if (FAILED(hr)) return hr;
    }
    if (fifo_count_ >= static_cast<size_t>(frames_)) input_waits_ = input_waits_ + 1;
  }

  const bool running = running_.load(std::memory_order_acquire);
  const int64_t period = static_cast<int64_t>(frames_) * kTicksPerSecond / rate_;
  if (last_render_ && now - last_render_ > period * 3 / 2) {
    if (running) late_ = late_ + 1;
    const int missed = static_cast<int>(std::lround(static_cast<double>(now - last_render_) / period)) - 1;
    if (missed > 0) {
      pending_skips_ += missed;
      skip_deadline_ = periods_ + 4;
    }
    watch_until_ = periods_ + kWatchSeconds * rate_ / frames_;
    if (late_record_count_ < kLateRecords) {
      late_records_[late_record_count_++] = {
          now,
          running,
          static_cast<double>(now - last_render_) / 1e4,
          last_capture_wake_ ? static_cast<double>(now - last_capture_wake_) / 1e4 : -1.0,
          capture_wakes_since_render_,
          packets_since_render_,
          have_capture_ ? static_cast<double>(now - last_packet_qpc_) / 1e4 : -1.0};
    }
  }
  capture_wakes_since_render_ = 0;
  last_render_ = now;
  periods_ = periods_ + 1;
  arrivals_[std::min(packets_since_render_, 3)] = arrivals_[std::min(packets_since_render_, 3)] + 1;
  packets_since_render_ = 0;
  if (have_capture_) {
    // How long before this event the newest capture packet completed.
    const double slack = static_cast<double>(now - last_packet_qpc_) / 10.0;
    slack_min_us_ = std::min(slack_min_us_.load(), slack);
    slack_max_us_ = std::max(slack_max_us_.load(), slack);
    slack_sum_us_ = slack_sum_us_ + slack;
    slack_count_ = slack_count_ + 1;
  }

  // Measure the output queue (every period while warming up, then every 8th) before the host's callback: the
  // render clock moves in coarse steps, so reading it at a time that depends on the host's load biases it.
  if (measured_periods_ < 64 || periods_ % 8 == 0) MeasureOutput(now);

  BYTE* output = nullptr;
  HRESULT hr = w_->render->GetBuffer(frames_, &output);
  if (FAILED(hr)) return hr;
  const int16_t* input = TakeInput(now, running);
  DWORD flags = AUDCLNT_BUFFERFLAGS_SILENT;
  if (running) {
    EnterCriticalSection(&process_lock_);
    if (running_.load(std::memory_order_relaxed)) {
      const int64_t start = QpcNow();
      client_->Process(input, output, frames_, position_, now * 100);
      const double us = static_cast<double>(QpcNow() - start) / 10.0;
      if (us > max_process_us_) max_process_us_ = us;
      position_ += frames_;
      callbacks_ = callbacks_ + 1;
      flags = 0;
    }
    LeaveCriticalSection(&process_lock_);
  }
  hr = w_->render->ReleaseBuffer(frames_, flags);
  if (FAILED(hr)) return hr;
  CheckLatencies(running);
  written_ += frames_;
  ++measured_periods_;
  return S_OK;
}

}  // namespace mbox2::asio
