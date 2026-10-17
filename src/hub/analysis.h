// Audio analysis behind the hub's analysis views, in the style of RME DIGICheck: loudness (ITU-R BS.1770-4,
// EBU R128 and EBU Tech 3342 loudness range), 4x oversampled true peak, a fractional-octave spectrum and a
// phase meter (correlation and goniometer). Input is interleaved stereo float at any sample rate.
// Not thread-safe: the hub runs it on the UI thread, and only while an analysis view is on screen.
#pragma once

#include <array>
#include <complex>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <vector>

// Second-order IIR section (direct form I, double precision).
struct Biquad {
  double b0 = 1.0, b1 = 0.0, b2 = 0.0, a1 = 0.0, a2 = 0.0;
  double x1 = 0.0, x2 = 0.0, y1 = 0.0, y2 = 0.0;

  double Run(double x) {
    double y = b0 * x + b1 * x1 + b2 * x2 - a1 * y1 - a2 * y2;
    x2 = x1;
    x1 = x;
    y2 = y1;
    y1 = y;
    return y;
  }
  void Clear() { x1 = x2 = y1 = y2 = 0.0; }
};

// True-peak detector (BS.1770-4 Annex 2): 4x oversampling with a polyphase windowed-sinc interpolator.
class TruePeak {
 public:
  static constexpr int kPhases = 4;
  static constexpr int kTaps = 24;  // per phase

  // The largest magnitude among the four interpolated samples that follow this input sample.
  float Run(float x);
  void Clear();

 private:
  std::array<float, 2 * kTaps> history_{};  // each sample twice, so a window is always contiguous
  int pos_ = 0;
};

// Loudness to EBU R128: momentary (400 ms), short-term (3 s), integrated (gated, BS.1770-4) and loudness range
// (EBU Tech 3342), plus the true peak. Values in LUFS / LU / dBTP; -infinity where there isn't enough audio.
class LoudnessMeter {
 public:
  static constexpr size_t kHistory = 600;  // 100 ms steps: one minute

  // The K-weighting filter's two stages (BS.1770-4 Annex 1) for a sample rate.
  static void KWeighting(uint32_t rate, Biquad* shelf, Biquad* highpass);

  void Reset(uint32_t rate);  // filters and everything measured
  void ResetMeasurement();    // integrated, range, maxima, true peak and history (the Reset button)
  void Process(const float* stereo, size_t frames);

  uint32_t Rate() const { return rate_; }
  double Momentary() const { return momentary_; }
  double ShortTerm() const { return short_term_; }
  double MomentaryMax() const { return momentary_max_; }
  double ShortTermMax() const { return short_term_max_; }
  double Integrated() const;
  double Range() const;
  double TruePeakMax() const { return true_peak_max_; }  // dBTP, both channels
  double Seconds() const { return blocks_ / 10.0; }      // audio measured since ResetMeasurement
  // Momentary and short-term loudness every 100 ms, oldest first.
  const std::deque<float>& MomentaryHistory() const { return momentary_history_; }
  const std::deque<float>& ShortTermHistory() const { return short_term_history_; }

 private:
  // Gating histograms: 0.1 LU bins from -70 LUFS (the absolute gate) up.
  static constexpr int kBins = 800;
  struct Histogram {
    std::array<double, kBins> energy{};
    std::array<uint32_t, kBins> count{};
    void Add(double energy_value, double loudness);
    void Clear();
  };
  void EndSubBlock();

  uint32_t rate_ = 0;
  Biquad shelf_[2], highpass_[2];
  TruePeak true_peak_[2];
  size_t sub_length_ = 0, sub_position_ = 0;
  double sub_sum_[2] = {};
  float sub_peak_ = 0.0f;
  std::array<double, 30> sub_energy_{};  // the last 3 s in 100 ms steps (ring)
  size_t sub_head_ = 0, sub_count_ = 0;
  double momentary_ = 0.0, short_term_ = 0.0, momentary_max_ = 0.0, short_term_max_ = 0.0;
  double true_peak_max_ = 0.0;
  uint64_t blocks_ = 0;
  Histogram integrated_, range_;
  std::deque<float> momentary_history_, short_term_history_;
};

// Spectrum of both channels (their average power), as band power over fractional-octave bands on a log
// frequency axis from 20 Hz to 20 kHz. A sine reads its own level (a -20 dBFS sine shows -20 dB).
class SpectrumAnalyzer {
 public:
  static constexpr int kSize = 8192;  // FFT length: 5.9 Hz bins and a 170 ms window at 48 kHz
  static constexpr float kFloorDb = -120.0f;

  void Reset(uint32_t rate);
  void Push(const float* stereo, size_t frames);
  // Recomputes `columns` bands, each `octaves` wide (1/3 or 1/6), with meter ballistics over dt seconds.
  void Update(int columns, double octaves, float dt);
  void ResetPeaks();
  uint32_t Rate() const { return rate_; }
  const std::vector<float>& Level() const { return level_; }  // dB, column 0 = 20 Hz
  const std::vector<float>& Peak() const { return peak_; }    // dB, peak hold
  static double ColumnFrequency(int column, int columns);

 private:
  void Fft();

  uint32_t rate_ = 0;
  std::vector<float> left_, right_;  // ring buffers
  size_t position_ = 0, filled_ = 0;
  std::vector<float> window_;
  double window_sum_ = 0.0;
  std::vector<std::complex<float>> buffer_, twiddle_;
  std::vector<uint32_t> reverse_;
  std::vector<double> cumulative_;  // running sum of bin power, for band sums
  std::vector<float> level_, peak_, hold_;
};

// Stereo phase: correlation (+1 mono, 0 unrelated, -1 out of phase) and goniometer points.
class PhaseMeter {
 public:
  static constexpr size_t kPoints = 2048;

  void Reset(uint32_t rate);
  void Push(const float* stereo, size_t frames);
  // NaN while the signal is too quiet to tell.
  double Correlation() const;
  // The newest points, oldest first, as x (side, right is +) and y (mid) pairs scaled to about -1..1 by an
  // automatic gain.
  void Points(std::vector<float>* xy) const;
  double GainDb() const;

 private:
  uint32_t rate_ = 48000;
  double decay_ = 0.0;  // per sample, for a 300 ms average
  double ll_ = 0.0, rr_ = 0.0, lr_ = 0.0;
  std::array<float, 2 * kPoints> points_{};
  size_t head_ = 0, count_ = 0;
  double envelope_ = 0.0;  // peak of mid and side, decaying over about a second
};
