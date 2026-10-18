// Test host for the Mbox 2 Mini ASIO driver: loads mbox2asio.dll the way a DAW does, plays a signal on both
// outputs, records both inputs and reports timing.
//
//   asiotest [--dll PATH | --registered] info
//   asiotest [--dll PATH | --registered] run SECONDS [--rate HZ] [--buffer N] [--signal tone|chirp|silence]
//            [--freq HZ] [--level DBFS] [--record OUT.wav] [--no-timeinfo] [--stall SECONDS] [--load MS]
//
// --rate asks the driver for 44100 or 48000 (setSampleRate) before anything else; info and run use the
// driver's rate either way.
// --dll loads the DLL straight from a path (default: mbox2asio.dll next to this program), without
// registration. --registered creates it through COM from the registry, like a DAW.
// tone: a steady sine on both outputs (1300 Hz, -12 dBFS by default), for glitch analysis
// of the recording. chirp: a 50 ms sweep every half second; afterwards the program finds each sweep
// in input 1 and prints the round trip, to compare with the driver's reported input + output latency. Both
// need the headphones' earcup held on a microphone on input 1, and the Mix knob turned fully to Playback.
// The recording is 16-bit stereo (input 1 left, input 2 right), sample-aligned with the output timeline:
// a sound sent at output sample t shows up at recording sample t + round trip.
// --stall makes one callback sleep for two periods at that time, so the driver misses periods; with
// --signal chirp, the sweeps after it should keep the same round trip. --load makes every callback busy-wait
// that many milliseconds, as a DAW's processing would.
#include <windows.h>

#include <objbase.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cwchar>
#include <string>
#include <vector>

#include "asio/asio_sdk.h"
#include "asio/driver.h"
#include "asio/names.h"

using mbox2::asio::Engine;
using mbox2::asio::kClsid;

namespace {

constexpr double kPi = 3.14159265358979323846;
int g_rate = 48000;  // --rate: asks the driver for it (setSampleRate)

IASIO* g_driver = nullptr;
long g_frames = 0;
void* g_in[2][2] = {};
void* g_out[2][2] = {};

enum class Signal { kTone, kChirp, kSilence };
Signal g_signal = Signal::kTone;
double g_freq = 1300, g_amplitude = 0.25;
std::vector<float> g_chirp;  // one sweep
int64_t g_chirp_every = 0;  // half a second, set in Run()
int64_t g_position = 0;      // next output sample
int64_t g_stall_at = -1;     // --stall: the callback sleeps once, here, to make the driver miss periods
double g_load_ms = 0;        // --load: the callback busy-waits this long, like a DAW's processing
std::vector<int16_t> g_recording;
size_t g_record_limit = 0;

// Callback timing.
LARGE_INTEGER g_qpc_frequency;
int64_t g_last_callback = 0, g_callbacks = 0, g_late = 0;
double g_max_interval_ms = 0;
volatile bool g_reset_requested = false, g_latencies_changed = false;

int64_t Now() {
  LARGE_INTEGER c;
  QueryPerformanceCounter(&c);
  return c.QuadPart;
}

double Sample(int64_t t) {
  switch (g_signal) {
    case Signal::kTone: {
      const double ramp = std::min(1.0, static_cast<double>(t) * 10 / g_rate);
      return g_amplitude * ramp * std::sin(2 * kPi * g_freq * static_cast<double>(t % (g_rate * 100)) / g_rate);
    }
    case Signal::kChirp: {
      const int64_t k = t % g_chirp_every;
      return t >= g_rate && k < static_cast<int64_t>(g_chirp.size()) ? g_chirp[k] : 0.0;
    }
    default:
      return 0;
  }
}

void BufferSwitch(long index, ASIOBool) {
  const int64_t now = Now();
  if (g_last_callback) {
    const double ms = static_cast<double>(now - g_last_callback) * 1000.0 / g_qpc_frequency.QuadPart;
    g_max_interval_ms = std::max(g_max_interval_ms, ms);
    if (ms > 1.5 * 1000.0 * g_frames / g_rate) ++g_late;
  }
  g_last_callback = now;
  ++g_callbacks;
  if (g_load_ms > 0) {
    const int64_t until = now + static_cast<int64_t>(g_load_ms * g_qpc_frequency.QuadPart / 1000);
    while (Now() < until) YieldProcessor();
  }
  if (g_stall_at >= 0 && g_position >= g_stall_at) {
    g_stall_at = -1;
    Sleep(static_cast<DWORD>(2 * 1000 * g_frames / g_rate + 1));
  }

  const auto* in0 = static_cast<const int16_t*>(g_in[0][index]);
  const auto* in1 = static_cast<const int16_t*>(g_in[1][index]);
  for (long f = 0; f < g_frames && g_recording.size() + 2 <= g_record_limit; ++f) {
    g_recording.push_back(in0[f]);
    g_recording.push_back(in1[f]);
  }
  for (long f = 0; f < g_frames; ++f) {
    const int32_t v = static_cast<int32_t>(std::lround(Sample(g_position + f) * 8388607.0));
    for (int c = 0; c < 2; ++c) {
      auto* out = static_cast<uint8_t*>(g_out[c][index]) + f * 3;
      out[0] = static_cast<uint8_t>(v);
      out[1] = static_cast<uint8_t>(v >> 8);
      out[2] = static_cast<uint8_t>(v >> 16);
    }
  }
  g_position += g_frames;
}

ASIOTime* BufferSwitchTimeInfo(ASIOTime* time, long index, ASIOBool direct) {
  const int64_t position = (static_cast<int64_t>(time->timeInfo.samplePosition.hi) << 32) | time->timeInfo.samplePosition.lo;
  if (position != g_position) {
    printf("time info: sample position %lld, expected %lld\n", position, g_position);
    g_position = position;
  }
  BufferSwitch(index, direct);
  return nullptr;
}

void SampleRateChanged(ASIOSampleRate rate) { printf("sample rate changed: %.0f\n", rate); }

bool g_time_info = true;

long AsioMessage(long selector, long value, void*, double*) {
  switch (selector) {
    case kAsioSelectorSupported:
      return value == kAsioEngineVersion || value == kAsioResetRequest || value == kAsioLatenciesChanged ||
             value == kAsioSupportsTimeInfo || value == kAsioResyncRequest || value == kAsioOverload;
    case kAsioEngineVersion:
      return 2;
    case kAsioSupportsTimeInfo:
      return g_time_info ? 1 : 0;
    case kAsioResetRequest:
      g_reset_requested = true;
      return 1;
    case kAsioLatenciesChanged:
      g_latencies_changed = true;
      return 1;
    default:
      return 0;
  }
}

ASIOCallbacks g_callbacks_table = {BufferSwitch, SampleRateChanged, AsioMessage, BufferSwitchTimeInfo};

IASIO* LoadFromDll(const std::wstring& path) {
  HMODULE dll = LoadLibraryW(path.c_str());
  if (!dll) {
    fprintf(stderr, "can't load %ls (error %lu)\n", path.c_str(), GetLastError());
    return nullptr;
  }
  using GetClassObject = HRESULT(STDAPICALLTYPE*)(REFCLSID, REFIID, void**);
  auto get = reinterpret_cast<GetClassObject>(GetProcAddress(dll, "DllGetClassObject"));
  IClassFactory* factory = nullptr;
  if (!get || FAILED(get(kClsid, IID_IClassFactory, reinterpret_cast<void**>(&factory)))) {
    fprintf(stderr, "%ls has no class factory for the driver\n", path.c_str());
    return nullptr;
  }
  IASIO* driver = nullptr;
  const HRESULT hr = factory->CreateInstance(nullptr, kClsid, reinterpret_cast<void**>(&driver));
  factory->Release();
  if (FAILED(hr)) fprintf(stderr, "CreateInstance failed: 0x%08lx\n", static_cast<unsigned long>(hr));
  return driver;
}

bool WriteWav(const std::wstring& path, const std::vector<int16_t>& pcm) {
  FILE* file = _wfopen(path.c_str(), L"wb");
  if (!file) return false;
  const DWORD data_bytes = static_cast<DWORD>(pcm.size() * sizeof(int16_t));
  const DWORD riff_bytes = 36 + data_bytes, fmt_bytes = 16, rate = g_rate, byte_rate = g_rate * 4;
  const WORD pcm_tag = 1, channels = 2, block_align = 4, bits = 16;
  fwrite("RIFF", 1, 4, file);
  fwrite(&riff_bytes, 4, 1, file);
  fwrite("WAVEfmt ", 1, 8, file);
  fwrite(&fmt_bytes, 4, 1, file);
  fwrite(&pcm_tag, 2, 1, file);
  fwrite(&channels, 2, 1, file);
  fwrite(&rate, 4, 1, file);
  fwrite(&byte_rate, 4, 1, file);
  fwrite(&block_align, 2, 1, file);
  fwrite(&bits, 2, 1, file);
  fwrite("data", 1, 4, file);
  fwrite(&data_bytes, 4, 1, file);
  fwrite(pcm.data(), sizeof(int16_t), pcm.size(), file);
  return fclose(file) == 0;
}

const char* ErrorName(ASIOError e) {
  switch (e) {
    case ASE_OK: return "ASE_OK";
    case ASE_SUCCESS: return "ASE_SUCCESS";
    case ASE_NotPresent: return "ASE_NotPresent";
    case ASE_HWMalfunction: return "ASE_HWMalfunction";
    case ASE_InvalidParameter: return "ASE_InvalidParameter";
    case ASE_InvalidMode: return "ASE_InvalidMode";
    case ASE_SPNotAdvancing: return "ASE_SPNotAdvancing";
    case ASE_NoClock: return "ASE_NoClock";
    case ASE_NoMemory: return "ASE_NoMemory";
    default: return "?";
  }
}

void PrintError(const char* what, ASIOError e) {
  char message[124] = "";
  g_driver->getErrorMessage(message);
  fprintf(stderr, "%s: %s (%s)\n", what, ErrorName(e), message);
}

int Info() {
  char name[32] = "";
  g_driver->getDriverName(name);
  long inputs = 0, outputs = 0, min_size = 0, max_size = 0, preferred = 0, granularity = 0, in_lat = 0, out_lat = 0;
  g_driver->getChannels(&inputs, &outputs);
  g_driver->getBufferSize(&min_size, &max_size, &preferred, &granularity);
  g_driver->getLatencies(&in_lat, &out_lat);
  ASIOSampleRate rate = 0;
  g_driver->getSampleRate(&rate);
  printf("driver: %s, version %ld\n", name, g_driver->getDriverVersion());
  printf("channels: %ld in, %ld out\n", inputs, outputs);
  for (long c = 0; c < inputs + outputs; ++c) {
    ASIOChannelInfo info{};
    info.channel = c < inputs ? c : c - inputs;
    info.isInput = c < inputs;
    g_driver->getChannelInfo(&info);
    printf("  %-10s type %ld, group %ld\n", info.name, info.type, info.channelGroup);
  }
  printf("buffer sizes: min %ld, max %ld, preferred %ld, granularity %ld\n", min_size, max_size, preferred, granularity);
  printf("latencies (estimate, preferred size): in %ld, out %ld\n", in_lat, out_lat);
  printf("sample rate %.0f; canSampleRate 44100: %s, 48000: %s, 96000: %s\n", rate, ErrorName(g_driver->canSampleRate(44100)),
         ErrorName(g_driver->canSampleRate(48000)), ErrorName(g_driver->canSampleRate(96000)));
  ASIOClockSource clocks[4] = {};
  long count = 4;
  g_driver->getClockSources(clocks, &count);
  printf("clock sources: %ld (%s)\n", count, clocks[0].name);
  printf("future(kAsioCanTimeInfo): %s\n", ErrorName(g_driver->future(kAsioCanTimeInfo, nullptr)));
  return 0;
}

// Finds each sweep in the recording's input 1 and returns the round trips in samples.
std::vector<int> FindChirps(const std::vector<int16_t>& recording, int64_t total) {
  std::vector<int> lags;
  const int64_t frames = static_cast<int64_t>(recording.size() / 2);
  const int max_lag = 8000;
  const int n = static_cast<int>(g_chirp.size());
  double template_energy = 0;
  for (float v : g_chirp) template_energy += static_cast<double>(v) * v;
  for (int64_t start = g_rate; start + n + max_lag < std::min(frames, total); start += g_chirp_every) {
    double best = 0, best_norm = 0;
    int best_lag = -1;
    for (int lag = 0; lag < max_lag; ++lag) {
      double dot = 0, energy = 0;
      for (int i = 0; i < n; ++i) {
        const double x = recording[2 * (start + lag + i)];
        dot += x * g_chirp[i];
        energy += x * x;
      }
      if (std::abs(dot) > best) {
        best = std::abs(dot);
        best_lag = lag;
        best_norm = energy > 0 ? best / std::sqrt(energy * template_energy) : 0;
      }
    }
    if (best_norm > 0.5) lags.push_back(best_lag);
  }
  return lags;
}

int Run(double seconds, long frames, const std::wstring& record_path) {
  long min_size = 0, max_size = 0, preferred = 0, granularity = 0;
  g_driver->getBufferSize(&min_size, &max_size, &preferred, &granularity);
  g_frames = frames ? frames : preferred;
  g_chirp_every = g_rate / 2;

  ASIOBufferInfo infos[4] = {};
  for (int i = 0; i < 4; ++i) {
    infos[i].isInput = i < 2;
    infos[i].channelNum = i % 2;
  }
  ASIOError e = g_driver->createBuffers(infos, 4, g_frames, &g_callbacks_table);
  if (e != ASE_OK) {
    PrintError("createBuffers", e);
    return 1;
  }
  for (int i = 0; i < 4; ++i) {
    for (int h = 0; h < 2; ++h) (i < 2 ? g_in : g_out)[i % 2][h] = infos[i].buffers[h];
  }
  long in_lat = 0, out_lat = 0;
  g_driver->getLatencies(&in_lat, &out_lat);
  printf("rate %d Hz, buffer %ld frames (%.2f ms); reported latency in %ld, out %ld frames: %.2f ms round trip\n", g_rate,
         g_frames, g_frames * 1000.0 / g_rate, in_lat, out_lat, (in_lat + out_lat) * 1000.0 / g_rate);

  const int64_t total = static_cast<int64_t>(seconds * g_rate);
  g_record_limit = static_cast<size_t>(total + g_rate) * 2;
  g_recording.reserve(g_record_limit);
  if (g_signal == Signal::kChirp) {
    const int n = g_rate / 20;  // 50 ms, 300 Hz to 6 kHz, Hann window
    g_chirp.resize(n);
    for (int i = 0; i < n; ++i) {
      const double t = static_cast<double>(i) / g_rate, length = static_cast<double>(n) / g_rate;
      const double phase = 2 * kPi * (300 * t + (6000 - 300) * t * t / (2 * length));
      g_chirp[i] = static_cast<float>(g_amplitude * 0.5 * (1 - std::cos(2 * kPi * i / (n - 1))) * std::sin(phase));
    }
  }

  e = g_driver->start();
  if (e != ASE_OK) {
    PrintError("start", e);
    g_driver->disposeBuffers();
    return 1;
  }
  const int64_t begin = Now();
  int64_t next_report = 10;
  for (;;) {
    Sleep(50);
    const double elapsed = static_cast<double>(Now() - begin) / g_qpc_frequency.QuadPart;
    if (g_reset_requested) {
      printf("%.1f s: the driver asked for a reset\n", elapsed);
      break;
    }
    if (g_latencies_changed) {
      g_latencies_changed = false;
      g_driver->getLatencies(&in_lat, &out_lat);
      printf("%.1f s: latencies changed: in %ld, out %ld\n", elapsed, in_lat, out_lat);
    }
    if (elapsed >= next_report) {
      Engine::Stats s;
      g_driver->future(mbox2::asio::kFutureGetStats, &s);
      printf("%4.0f s: %lld callbacks, %lld late; driver: %llu underruns, %llu late, %llu gaps, %llu dropped, %llu realigned\n",
             elapsed, g_callbacks, g_late, s.input_underruns, s.late_periods, s.capture_gaps, s.input_dropped, s.input_realigns);
      next_report += 10;
    }
    if (g_position >= total) break;
  }
  g_driver->stop();
  const double elapsed = static_cast<double>(Now() - begin) / g_qpc_frequency.QuadPart;
  Engine::Stats s;
  g_driver->future(mbox2::asio::kFutureGetStats, &s);
  g_driver->getLatencies(&in_lat, &out_lat);
  g_driver->disposeBuffers();

  printf("ran %.1f s: %lld callbacks (%.1f expected), %lld late (>1.5 periods), longest gap %.2f ms\n", elapsed,
         g_callbacks, elapsed * g_rate / g_frames, g_late, g_max_interval_ms);
  printf("driver: %llu periods, %llu input underruns, %llu input waits, %llu dropped, %llu realigned, %llu capture gaps, "
         "%llu late, %llu latency changes, max process %.0f us\n",
         s.periods, s.input_underruns, s.input_waits, s.input_dropped, s.input_realigns, s.capture_gaps, s.late_periods,
         s.latency_changes, s.max_process_us);
  printf("driver: input safety %d, input age %d, output queue %d frames; capture slack %.0f/%.0f/%.0f us; "
         "packets per period 0:%llu 1:%llu 2:%llu 3+:%llu\n",
         s.input_safety, s.input_age_frames, s.output_queue_frames, s.slack_min_us, s.slack_mean_us, s.slack_max_us,
         s.arrivals[0], s.arrivals[1], s.arrivals[2], s.arrivals[3]);
  printf("reported latency at the end: in %ld, out %ld = %ld frames (%.2f ms)\n", in_lat, out_lat, in_lat + out_lat,
         (in_lat + out_lat) * 1000.0 / g_rate);

  if (g_signal == Signal::kChirp) {
    std::vector<int> lags = FindChirps(g_recording, total);
    if (lags.empty()) {
      printf("round trip: no sweeps found in input 1 (earcup on the microphone? level?)\n");
    } else {
      printf("sweep round trips in order:");
      for (int lag : lags) printf(" %d", lag);
      printf("\n");
      std::sort(lags.begin(), lags.end());
      printf("round trip: %zu sweeps, median %d frames (%.2f ms), min %d, max %d; reported in+out %ld (%+ld)\n",
             lags.size(), lags[lags.size() / 2], lags[lags.size() / 2] * 1000.0 / g_rate, lags.front(), lags.back(),
             in_lat + out_lat, in_lat + out_lat - lags[lags.size() / 2]);
    }
  }
  if (!record_path.empty()) {
    if (!WriteWav(record_path, g_recording)) {
      fprintf(stderr, "can't write %ls\n", record_path.c_str());
      return 1;
    }
    printf("recording: %ls\n", record_path.c_str());
  }
  return 0;
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
  QueryPerformanceFrequency(&g_qpc_frequency);
  std::wstring dll, command, record;
  bool registered = false;
  double seconds = 10, stall_seconds = -1;
  long frames = 0;
  int rate = 0;
  std::vector<std::wstring> positional;
  for (int i = 1; i < argc; ++i) {
    const std::wstring a = argv[i];
    auto next = [&]() -> std::wstring { return i + 1 < argc ? argv[++i] : L""; };
    if (a == L"--dll") dll = next();
    else if (a == L"--registered") registered = true;
    else if (a == L"--buffer") frames = _wtol(next().c_str());
    else if (a == L"--rate") rate = _wtoi(next().c_str());
    else if (a == L"--freq") g_freq = _wtof(next().c_str());
    else if (a == L"--level") g_amplitude = std::pow(10.0, _wtof(next().c_str()) / 20);
    else if (a == L"--record") record = next();
    else if (a == L"--no-timeinfo") g_time_info = false;
    else if (a == L"--load") g_load_ms = _wtof(next().c_str());
    else if (a == L"--stall") stall_seconds = _wtof(next().c_str());
    else if (a == L"--signal") {
      const std::wstring s = next();
      g_signal = s == L"chirp" ? Signal::kChirp : s == L"silence" ? Signal::kSilence : Signal::kTone;
    } else positional.push_back(a);
  }
  if (positional.empty() || (positional[0] != L"info" && positional[0] != L"run")) {
    fprintf(stderr,
            "usage: asiotest [--dll PATH | --registered] info\n"
            "       asiotest [--dll PATH | --registered] run SECONDS [--rate HZ] [--buffer N] [--signal tone|chirp|silence]\n"
            "                [--freq HZ] [--level DBFS] [--record OUT.wav] [--no-timeinfo] [--stall SECONDS]\n");
    return 2;
  }
  command = positional[0];
  if (positional.size() > 1) seconds = _wtof(positional[1].c_str());

  CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
  if (registered) {
    const HRESULT hr = CoCreateInstance(kClsid, nullptr, CLSCTX_INPROC_SERVER, kClsid, reinterpret_cast<void**>(&g_driver));
    if (FAILED(hr)) {
      fprintf(stderr, "the driver isn't registered (CoCreateInstance 0x%08lx)\n", static_cast<unsigned long>(hr));
      return 1;
    }
  } else {
    if (dll.empty()) {
      wchar_t exe[MAX_PATH];
      GetModuleFileNameW(nullptr, exe, MAX_PATH);
      dll = exe;
      dll = dll.substr(0, dll.find_last_of(L'\\') + 1) + L"mbox2asio.dll";
    }
    g_driver = LoadFromDll(dll);
    if (!g_driver) return 1;
  }
  if (!g_driver->init(GetConsoleWindow())) {
    char message[124] = "";
    g_driver->getErrorMessage(message);
    fprintf(stderr, "init failed: %s\n", message);
    g_driver->Release();
    return 1;
  }
  if (rate) {
    const ASIOError e = g_driver->setSampleRate(rate);
    if (e != ASE_OK) {
      PrintError("setSampleRate", e);
      g_driver->Release();
      return 1;
    }
  }
  ASIOSampleRate current = 0;
  g_driver->getSampleRate(&current);
  g_rate = static_cast<int>(current);
  if (stall_seconds >= 0) g_stall_at = static_cast<int64_t>(stall_seconds * g_rate);
  const int result = command == L"info" ? Info() : Run(seconds, frames, record);
  g_driver->Release();
  CoUninitialize();
  return result;
}
