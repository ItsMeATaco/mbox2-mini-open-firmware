// Duplex audio for the Mbox 2 Mini through WASAPI exclusive mode, the engine behind the ASIO driver.
//
// The open firmware's two endpoints, "Speakers (Mbox 2 Mini)" (2 ch, 24-bit in 3 bytes) and "Microphone (Mbox
// 2 Mini)" (2 ch, 16-bit), are opened exclusively at one rate (48 or 44.1 kHz) with the same period. Both run off
// the USB frame clock (adaptive playback, synchronous capture), so they never drift apart: the render
// stream's event drives the period, and capture packets go through a short FIFO that absorbs the phase
// between the two streams' events.
//
// All WASAPI objects live on the engine's own thread (MTA, MMCSS "Pro Audio"). Open() starts that thread,
// which opens both endpoints and streams silence until Start(); Stop() goes back to silence and Close()
// releases the device. So the streams keep one phase from Open() to Close(), and the latencies measured
// in Open() stay true across Start()/Stop().
#pragma once

#include <windows.h>

#include <atomic>
#include <cstdint>
#include <string>
#include <thread>
#include <vector>

namespace mbox2::asio {

// The open firmware runs at 48 or 44.1 kHz (44.1 since v0.24), one clock for both directions.
inline constexpr int kDefaultRate = 48000;
constexpr bool IsSupportedRate(double rate) { return rate == 44100 || rate == 48000; }
inline constexpr int kChannels = 2;
inline constexpr int kOutputSampleBytes = 3;  // 24-bit in 3-byte containers
inline constexpr int kInputSampleBytes = 2;   // 16-bit
// usbaudio.sys's shortest exclusive-mode period is 3 ms; it takes any frame count from there.
inline constexpr int kMinFrames = 144;
inline constexpr int kMaxFrames = 1920;
inline constexpr int kGranularity = 48;
inline constexpr int kDefaultFrames = 192;

// The buffer sizes the driver takes at each rate, as ASIO's getBufferSize() reports them. 48 kHz: whole
// milliseconds (above). 44.1 kHz: multiples of 10 ms, the only whole milliseconds there. usbaudio.sys delivers
// capture packets on a whole-millisecond schedule: at other sizes they reach the driver up to a period late, in a
// cycle of 0.5 s (133 frames) to 4 s (265) and longer, and so do their timestamps. Those sizes played without an
// input underrun, but the input latency measured from the timestamps came out 63-162 frames short of the acoustic
// round trip (2026-10-17), and a DAW aligns recordings by it. At 441 it was 9-10 frames long, as steady as 48 kHz.
inline constexpr int kMinFrames44k1 = 441;
inline constexpr int kGranularity44k1 = 441;
inline constexpr int kMaxFrames44k1 = 4 * kGranularity44k1;  // 1764, 40 ms
inline constexpr int kDefaultFrames44k1 = 441;
struct BufferSizes {
  int min, max, granularity, preferred;
};
constexpr BufferSizes BuffersAt(int rate) {
  return rate == 44100 ? BufferSizes{kMinFrames44k1, kMaxFrames44k1, kGranularity44k1, kDefaultFrames44k1}
                       : BufferSizes{kMinFrames, kMaxFrames, kGranularity, kDefaultFrames};
}
constexpr bool BufferAllowed(int frames, int rate) {
  const BufferSizes b = BuffersAt(rate);
  return frames >= b.min && frames <= b.max && (frames - b.min) % b.granularity == 0;
}

class Engine {
 public:
  class Client {
   public:
    // One period on the audio thread. `input` holds `frames` interleaved stereo 16-bit frames; `output` takes
    // as many interleaved stereo 24-bit (3-byte) frames. `position` counts frames since Start(); `time_ns`
    // is the start of the period in QueryPerformanceCounter time, in nanoseconds.
    virtual void Process(const int16_t* input, uint8_t* output, int frames, int64_t position, int64_t time_ns) = 0;
    // The engine had to buffer more input after an underrun, so the input latency grew.
    virtual void LatenciesChanged() = 0;
    // The device stopped (unplugged, or taken over); the engine streams nothing more until Close().
    virtual void DeviceLost() = 0;

   protected:
    ~Client() = default;
  };

  struct Stats {
    uint64_t periods = 0;          // render periods since Open()
    uint64_t callbacks = 0;        // Process() calls since Start()
    uint64_t input_underruns = 0;  // periods that had no full input block while running
    uint64_t input_waits = 0;      // periods whose capture packet came just after the render event (waited for)
    uint64_t input_dropped = 0;    // input blocks dropped to keep the reported latency (after a missed period)
    uint64_t capture_gaps = 0;     // capture packets missing (device-position jumps or discontinuity flags)
    uint64_t late_periods = 0;     // render events more than half a period late while running
    uint64_t latency_changes = 0;  // times the latencies were re-measured and the DAW told (after glitches)
    double max_process_us = 0;     // longest Process() call
    int input_margin = 0;          // extra input blocks kept in the FIFO
    int input_age_frames = 0;      // measured age of an input block's first frame at Process() (Windows side)
    int output_queue_frames = 0;   // measured frames queued ahead of a new output block (Windows side)
    uint64_t arrivals[4] = {};     // render periods with 0, 1, 2 and 3+ capture packets since the previous one
    double slack_min_us = 0;       // render event minus the newest capture packet's timestamp: min, mean, max
    double slack_mean_us = 0;
    double slack_max_us = 0;
  };

  Engine();
  ~Engine();
  Engine(const Engine&) = delete;
  Engine& operator=(const Engine&) = delete;

  // Looks for both endpoints. False, with a message for the user, if the Mbox isn't there.
  bool Find(std::string* error);
  // Whether the firmware found by Find() offers 44.1 kHz (v0.24 and later).
  bool has_44k1() const { return rate_44k1_; }
  // Opens both endpoints exclusively at `rate` with `frames` per period, streams silence and measures the
  // latencies.
  bool Open(int frames, int rate, Client* client, std::string* error);
  void Start();
  // On return, Process() is not running and won't be called again until Start().
  void Stop();
  void Close();

  bool is_open() const { return thread_.joinable(); }
  int frames() const { return frames_; }
  int rate() const { return rate_; }
  // Age of the first frame of an input block when Process() gets it (measured in Open() when open, estimated
  // otherwise), and the time from Process() until the first frame of its output block plays, in frames. Both
  // include the converters, firmware and USB delays WASAPI can't see.
  int input_latency(int frames, int rate) const;
  int output_latency(int frames, int rate) const;
  Stats stats() const;
  bool lost() const { return lost_.load(); }

 private:
  struct Packet;
  void Run();
  bool Setup(std::string* error);
  void Teardown();
  HRESULT DrainCapture(int64_t now);
  HRESULT RenderPeriod(int64_t now);
  const int16_t* TakeInput(int64_t now, bool running);
  void PushInput(const int16_t* data, uint32_t frames);
  void MeasureOutput(int64_t now);
  void CheckLatencies(bool running);

  std::wstring render_id_, capture_id_;
  bool rate_44k1_ = false;
  int frames_ = 0;
  int rate_ = kDefaultRate;
  Client* client_ = nullptr;

  std::thread thread_;
  HANDLE stop_event_ = nullptr;
  HANDLE ready_event_ = nullptr;
  std::string setup_error_;
  bool setup_ok_ = false;

  std::atomic<bool> running_{false};
  std::atomic<bool> lost_{false};
  CRITICAL_SECTION process_lock_;

  // Engine-thread state (WASAPI objects are opaque here; see engine.cpp).
  struct Wasapi;
  Wasapi* w_ = nullptr;

  // Input FIFO: interleaved stereo 16-bit, a ring of `fifo_capacity_` frames.
  std::vector<int16_t> fifo_;
  std::vector<int16_t> block_;
  size_t fifo_capacity_ = 0, fifo_read_ = 0, fifo_count_ = 0;
  uint64_t fifo_first_position_ = 0;  // capture device position of the oldest frame in the FIFO
  uint64_t next_capture_position_ = 0;
  bool have_capture_ = false;
  bool input_primed_ = false;
  int margin_ = 1;
  bool margin_fixed_ = false;
  uint64_t last_packet_position_ = 0;  // device position, length and QPC time (100 ns) of the newest packet
  uint32_t last_packet_frames_ = 0;
  int64_t last_packet_qpc_ = 0;
  int64_t last_packet_arrival_ = 0;
  int packets_since_render_ = 0;

  // Late render periods, logged by Close(): whether the thread still woke for capture packets during the
  // gap tells a late render notification from a thread that didn't get to run.
  struct LatePeriod {
    int64_t qpc;             // when the late render event woke the thread (100 ns)
    bool running;
    double gap_ms;           // since the previous render event
    double capture_wake_ms;  // since the thread last woke for a capture packet
    int capture_wakes;       // capture wake-ups since the previous render event
    int packets;             // capture packets since the previous render event
    double packet_age_ms;    // age of the newest capture packet's timestamp
  };
  static constexpr int kLateRecords = 16;
  LatePeriod late_records_[kLateRecords] = {};
  int late_record_count_ = 0;
  int64_t last_capture_wake_ = 0;
  int capture_wakes_since_render_ = 0;
  int64_t qpc_to_filetime_ = 0;

  uint64_t written_ = 0;  // render frames released, silence included
  int64_t last_render_ = 0;
  int64_t position_ = 0;
  double input_age_ = -1, output_queue_ = -1;  // running means, frames
  // The input age the DAW was told (frames): set at the end of the warm-up, and again when the measured age
  // moves away from it for good (after a glitch the streams can settle on another phase).
  double target_age_ = -1;
  int drift_periods_ = 0;
  static constexpr int kWatchSeconds = 2;
  uint64_t watch_until_ = 0;     // look for a new phase until this period (set after a glitch)
  int pending_skips_ = 0;        // render periods missed whose capture blocks are still to drop
  uint64_t skip_deadline_ = 0;
  int measured_periods_ = 0;
  int warmup_ = 0;  // periods streamed before Open() returns
  double warmup_age_sum_ = 0;
  int warmup_age_count_ = 0;

  // Stats, written by the engine thread.
  std::atomic<uint64_t> periods_{0}, callbacks_{0}, underruns_{0}, input_waits_{0}, dropped_{0}, gaps_{0}, late_{0};
  std::atomic<uint64_t> arrivals_[4] = {};
  std::atomic<double> max_process_us_{0}, slack_min_us_{1e9}, slack_max_us_{-1e9}, slack_sum_us_{0};
  std::atomic<uint64_t> slack_count_{0};
  std::atomic<int> input_age_frames_{0}, output_queue_frames_{0}, margin_now_{1};
  std::atomic<int> reported_age_{0};
  std::atomic<uint64_t> latency_changes_{0};
};

// Hardware delays that WASAPI's clocks can't see (converter filters, the firmware's buffering), in frames.
// Measured acoustically (engine.cpp); HKCU\Software\Mbox2Asio ExtraInputFrames / ExtraOutputFrames override.
int ExtraInputFrames(int rate);
int ExtraOutputFrames(int rate);

}  // namespace mbox2::asio
