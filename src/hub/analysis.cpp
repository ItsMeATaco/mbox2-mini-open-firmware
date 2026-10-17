#include "hub/analysis.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace {

constexpr double kPi = 3.14159265358979323846;
constexpr double kMinusInfinity = -std::numeric_limits<double>::infinity();

// BS.1770-4: loudness of a block from its channel-summed mean square (left and right weigh 1.0).
double Lufs(double energy) { return energy > 0.0 ? -0.691 + 10.0 * std::log10(energy) : kMinusInfinity; }

double BesselI0(double x) {
  double sum = 1.0, term = 1.0;
  for (int k = 1; k < 50; ++k) {
    term *= (x / (2.0 * k)) * (x / (2.0 * k));
    sum += term;
    if (term < sum * 1e-12) break;
  }
  return sum;
}

// Polyphase coefficients of the true-peak interpolator: a Kaiser-windowed sinc (beta 8) cut off at the
// original Nyquist frequency, each phase normalized to unity gain at DC.
const std::array<std::array<float, TruePeak::kTaps>, TruePeak::kPhases>& InterpolatorPhases() {
  static const auto phases = [] {
    constexpr int kL = TruePeak::kPhases, kN = TruePeak::kPhases * TruePeak::kTaps;
    constexpr double kBeta = 8.0;
    std::array<double, kN> h{};
    const double center = (kN - 1) / 2.0;
    for (int m = 0; m < kN; ++m) {
      double t = (m - center) / kL;
      double sinc = t == 0.0 ? 1.0 : std::sin(kPi * t) / (kPi * t);
      double u = 2.0 * m / (kN - 1) - 1.0;
      h[m] = sinc * BesselI0(kBeta * std::sqrt(std::max(0.0, 1.0 - u * u))) / BesselI0(kBeta);
    }
    std::array<std::array<float, TruePeak::kTaps>, TruePeak::kPhases> result{};
    for (int p = 0; p < kL; ++p) {
      double sum = 0.0;
      for (int k = 0; k < TruePeak::kTaps; ++k) sum += h[p + kL * k];
      for (int k = 0; k < TruePeak::kTaps; ++k) result[p][k] = static_cast<float>(h[p + kL * k] / sum);
    }
    return result;
  }();
  return phases;
}

}  // namespace

// ---- TruePeak

float TruePeak::Run(float x) {
  pos_ = (pos_ + 1) % kTaps;
  history_[pos_] = history_[pos_ + kTaps] = x;
  const float* newest = &history_[pos_ + kTaps];  // newest[-k] is the input k samples ago
  float peak = 0.0f;
  for (const auto& phase : InterpolatorPhases()) {
    float y = 0.0f;
    for (int k = 0; k < kTaps; ++k) y += phase[k] * newest[-k];
    peak = std::max(peak, std::fabs(y));
  }
  return peak;
}

void TruePeak::Clear() {
  history_.fill(0.0f);
  pos_ = 0;
}

// ---- LoudnessMeter

void LoudnessMeter::Histogram::Add(double energy_value, double loudness) {
  if (!(loudness > -70.0)) return;  // absolute gate
  int bin = std::clamp(static_cast<int>((loudness + 70.0) * 10.0), 0, kBins - 1);
  energy[bin] += energy_value;
  ++count[bin];
}

void LoudnessMeter::Histogram::Clear() {
  energy.fill(0.0);
  count.fill(0);
}

// K-weighting from its analog parameters, so it holds at any sample rate; at 48 kHz these give the
// standard's coefficients (checked by mbox2hub_analysis_test).
void LoudnessMeter::KWeighting(uint32_t rate, Biquad* shelf, Biquad* highpass) {
  {
    const double f0 = 1681.974450955533, gain_db = 3.999843853973347, q = 0.7071752369554196;
    const double k = std::tan(kPi * f0 / rate);
    const double vh = std::pow(10.0, gain_db / 20.0), vb = std::pow(vh, 0.4996667741545416);
    const double a0 = 1.0 + k / q + k * k;
    *shelf = {};
    shelf->b0 = (vh + vb * k / q + k * k) / a0;
    shelf->b1 = 2.0 * (k * k - vh) / a0;
    shelf->b2 = (vh - vb * k / q + k * k) / a0;
    shelf->a1 = 2.0 * (k * k - 1.0) / a0;
    shelf->a2 = (1.0 - k / q + k * k) / a0;
  }
  {
    const double f0 = 38.13547087602444, q = 0.5003270373238773;
    const double k = std::tan(kPi * f0 / rate);
    const double a0 = 1.0 + k / q + k * k;
    *highpass = {};
    highpass->b0 = 1.0;
    highpass->b1 = -2.0;
    highpass->b2 = 1.0;
    highpass->a1 = 2.0 * (k * k - 1.0) / a0;
    highpass->a2 = (1.0 - k / q + k * k) / a0;
  }
}

void LoudnessMeter::Reset(uint32_t rate) {
  rate_ = rate;
  KWeighting(rate, &shelf_[0], &highpass_[0]);
  shelf_[1] = shelf_[0];
  highpass_[1] = highpass_[0];
  true_peak_[0].Clear();
  true_peak_[1].Clear();
  sub_length_ = std::max<uint32_t>(rate / 10, 1);
  sub_position_ = 0;
  sub_sum_[0] = sub_sum_[1] = 0.0;
  sub_peak_ = 0.0f;
  sub_energy_.fill(0.0);
  sub_head_ = sub_count_ = 0;
  momentary_ = short_term_ = kMinusInfinity;
  ResetMeasurement();
}

void LoudnessMeter::ResetMeasurement() {
  momentary_max_ = short_term_max_ = true_peak_max_ = kMinusInfinity;
  blocks_ = 0;
  integrated_.Clear();
  range_.Clear();
  momentary_history_.clear();
  short_term_history_.clear();
}

void LoudnessMeter::Process(const float* stereo, size_t frames) {
  if (!rate_) return;
  for (size_t i = 0; i < frames; ++i) {
    const float l = stereo[2 * i], r = stereo[2 * i + 1];
    const double yl = highpass_[0].Run(shelf_[0].Run(l));
    const double yr = highpass_[1].Run(shelf_[1].Run(r));
    sub_sum_[0] += yl * yl;
    sub_sum_[1] += yr * yr;
    sub_peak_ = std::max({sub_peak_, true_peak_[0].Run(l), true_peak_[1].Run(r)});
    if (++sub_position_ == sub_length_) EndSubBlock();
  }
}

// Every 100 ms: the gating blocks (400 ms, 75 % overlap) and short-term windows (3 s) end here.
void LoudnessMeter::EndSubBlock() {
  const double energy = (sub_sum_[0] + sub_sum_[1]) / sub_length_;
  sub_sum_[0] = sub_sum_[1] = 0.0;
  sub_position_ = 0;
  sub_head_ = (sub_head_ + 1) % sub_energy_.size();
  sub_energy_[sub_head_] = energy;
  ++sub_count_;
  ++blocks_;
  if (sub_peak_ > 0.0f) true_peak_max_ = std::max(true_peak_max_, 20.0 * std::log10(sub_peak_));
  sub_peak_ = 0.0f;

  auto mean_of_last = [&](size_t n) {
    double sum = 0.0;
    for (size_t i = 0; i < n; ++i) sum += sub_energy_[(sub_head_ + sub_energy_.size() - i) % sub_energy_.size()];
    return sum / n;
  };
  if (sub_count_ >= 4) {
    double block = mean_of_last(4);
    momentary_ = Lufs(block);
    momentary_max_ = std::max(momentary_max_, momentary_);
    integrated_.Add(block, momentary_);
  }
  if (sub_count_ >= sub_energy_.size()) {
    double block = mean_of_last(sub_energy_.size());
    short_term_ = Lufs(block);
    short_term_max_ = std::max(short_term_max_, short_term_);
    range_.Add(block, short_term_);
  }
  momentary_history_.push_back(static_cast<float>(std::max(momentary_, -200.0)));
  short_term_history_.push_back(static_cast<float>(std::max(short_term_, -200.0)));
  if (momentary_history_.size() > kHistory) momentary_history_.pop_front();
  if (short_term_history_.size() > kHistory) short_term_history_.pop_front();
}

// BS.1770-4: the mean of the gating blocks above -70 LUFS sets a relative gate 10 LU below it; the
// integrated loudness is the mean of the blocks above both gates.
double LoudnessMeter::Integrated() const {
  double total = 0.0;
  uint64_t n = 0;
  for (int i = 0; i < kBins; ++i) {
    total += integrated_.energy[i];
    n += integrated_.count[i];
  }
  if (!n) return kMinusInfinity;
  const double gate = Lufs(total / n) - 10.0;
  double sum = 0.0;
  uint64_t m = 0;
  for (int i = 0; i < kBins; ++i) {
    if (-70.0 + 0.1 * (i + 0.5) > gate) {
      sum += integrated_.energy[i];
      m += integrated_.count[i];
    }
  }
  return m ? Lufs(sum / m) : kMinusInfinity;
}

// EBU Tech 3342: short-term values above -70 LUFS and above a relative gate 20 LU below their mean; the
// range is the spread between their 10th and 95th percentiles.
double LoudnessMeter::Range() const {
  double total = 0.0;
  uint64_t n = 0;
  for (int i = 0; i < kBins; ++i) {
    total += range_.energy[i];
    n += range_.count[i];
  }
  if (!n) return 0.0;
  const double gate = Lufs(total / n) - 20.0;
  int first = 0;
  while (first < kBins && -70.0 + 0.1 * (first + 0.5) <= gate) ++first;
  uint64_t kept = 0;
  for (int i = first; i < kBins; ++i) kept += range_.count[i];
  if (!kept) return 0.0;
  auto percentile = [&](double fraction) {
    const double target = fraction * (kept - 1);
    uint64_t seen = 0;
    for (int i = first; i < kBins; ++i) {
      seen += range_.count[i];
      if (seen > target) return -70.0 + 0.1 * (i + 0.5);
    }
    return -70.0 + 0.1 * (kBins - 0.5);
  };
  return percentile(0.95) - percentile(0.10);
}

// ---- SpectrumAnalyzer

double SpectrumAnalyzer::ColumnFrequency(int column, int columns) {
  return 20.0 * std::pow(1000.0, columns > 1 ? static_cast<double>(column) / (columns - 1) : 0.0);
}

void SpectrumAnalyzer::Reset(uint32_t rate) {
  rate_ = rate;
  left_.assign(kSize, 0.0f);
  right_.assign(kSize, 0.0f);
  position_ = filled_ = 0;
  window_.resize(kSize);
  window_sum_ = 0.0;
  for (int n = 0; n < kSize; ++n) {
    window_[n] = static_cast<float>(0.5 - 0.5 * std::cos(2.0 * kPi * n / kSize));  // Hann
    window_sum_ += window_[n];
  }
  buffer_.assign(kSize, {});
  twiddle_.resize(kSize / 2);
  for (int k = 0; k < kSize / 2; ++k) {
    twiddle_[k] = {static_cast<float>(std::cos(2.0 * kPi * k / kSize)), static_cast<float>(-std::sin(2.0 * kPi * k / kSize))};
  }
  reverse_.resize(kSize);
  int bits = 0;
  while ((1 << bits) < kSize) ++bits;
  for (uint32_t i = 0; i < static_cast<uint32_t>(kSize); ++i) {
    uint32_t r = 0;
    for (int b = 0; b < bits; ++b) r |= ((i >> b) & 1u) << (bits - 1 - b);
    reverse_[i] = r;
  }
  cumulative_.assign(kSize / 2 + 1, 0.0);
  level_.clear();
  peak_.clear();
  hold_.clear();
}

void SpectrumAnalyzer::Push(const float* stereo, size_t frames) {
  if (!rate_) return;
  for (size_t i = 0; i < frames; ++i) {
    left_[position_] = stereo[2 * i];
    right_[position_] = stereo[2 * i + 1];
    position_ = (position_ + 1) % kSize;
  }
  filled_ = std::min<size_t>(kSize, filled_ + frames);
}

void SpectrumAnalyzer::Fft() {
  for (uint32_t i = 0; i < static_cast<uint32_t>(kSize); ++i) {
    if (i < reverse_[i]) std::swap(buffer_[i], buffer_[reverse_[i]]);
  }
  for (int length = 2; length <= kSize; length <<= 1) {
    const int half = length / 2, step = kSize / length;
    for (int start = 0; start < kSize; start += length) {
      for (int k = 0; k < half; ++k) {
        std::complex<float> u = buffer_[start + k];
        std::complex<float> v = buffer_[start + k + half] * twiddle_[k * step];
        buffer_[start + k] = u + v;
        buffer_[start + k + half] = u - v;
      }
    }
  }
}

void SpectrumAnalyzer::ResetPeaks() {
  peak_ = level_;
  std::fill(hold_.begin(), hold_.end(), 0.0f);
}

void SpectrumAnalyzer::Update(int columns, double octaves, float dt) {
  if (!rate_ || columns < 2) return;
  if (static_cast<int>(level_.size()) != columns) {
    level_.assign(columns, kFloorDb);
    peak_.assign(columns, kFloorDb);
    hold_.assign(columns, 0.0f);
  }
  // Both channels in one complex FFT (left real, right imaginary), oldest sample first.
  for (int n = 0; n < kSize; ++n) {
    size_t i = (position_ + n) % kSize;
    buffer_[n] = {left_[i] * window_[n], right_[i] * window_[n]};
  }
  Fft();
  // Power per bin, scaled so a sine centred on a bin reads its amplitude squared, averaged over the two channels.
  const double scale = (2.0 / window_sum_) * (2.0 / window_sum_);
  cumulative_[0] = cumulative_[1] = 0.0;
  for (int k = 1; k < kSize / 2; ++k) {
    std::complex<float> x = buffer_[k], y = std::conj(buffer_[kSize - k]);
    std::complex<float> left = (x + y) * 0.5f;
    std::complex<float> right = (x - y) * std::complex<float>(0.0f, -0.5f);
    double power = 0.5 * (std::norm(left) + std::norm(right)) * scale;
    cumulative_[k + 1] = cumulative_[k] + power;
  }
  auto bin_power = [&](int k) { return cumulative_[k + 1] - cumulative_[k]; };

  const double bin_hz = static_cast<double>(rate_) / kSize;
  const double half_band = std::pow(2.0, octaves / 2.0);
  constexpr double kHannEnbw = 1.5;  // equivalent noise bandwidth in bins: turns a band's bin sum into its power
  for (int j = 0; j < columns; ++j) {
    const double f = ColumnFrequency(j, columns);
    int low = std::max(1, static_cast<int>(std::ceil(f / half_band / bin_hz)));
    int high = std::min(kSize / 2 - 1, static_cast<int>(std::floor(f * half_band / bin_hz)));
    double band = high >= low ? (cumulative_[high + 1] - cumulative_[low]) / kHannEnbw : 0.0;
    // Bands narrower than the bins (low frequencies): the bin power at f, interpolated.
    double position = std::clamp(f / bin_hz, 1.0, kSize / 2.0 - 2.0);
    int k = static_cast<int>(position);
    double frac = position - k;
    double single = bin_power(k) * (1.0 - frac) + bin_power(k + 1) * frac;
    float db = static_cast<float>(10.0 * std::log10(std::max(band, single) + 1e-24));
    db = std::max(db, kFloorDb);

    float& level = level_[j];
    level = db >= level ? db : std::max(db, level - 40.0f * dt);  // fast attack, 40 dB/s release
    float& peak = peak_[j];
    float& hold = hold_[j];
    if (level >= peak) {
      peak = level;
      hold = 2.0f;
    } else if (hold > 0.0f) {
      hold -= dt;
    } else {
      peak = std::max(level, peak - 15.0f * dt);
    }
  }
}

// ---- PhaseMeter

void PhaseMeter::Reset(uint32_t rate) {
  rate_ = rate ? rate : 48000;
  decay_ = std::exp(-1.0 / (0.3 * rate_));
  ll_ = rr_ = lr_ = 0.0;
  head_ = count_ = 0;
  envelope_ = 0.0;
}

void PhaseMeter::Push(const float* stereo, size_t frames) {
  const double envelope_decay = std::exp(-1.0 / (1.0 * rate_));
  constexpr float kHalfRoot2 = 0.70710678f;
  for (size_t i = 0; i < frames; ++i) {
    const float l = stereo[2 * i], r = stereo[2 * i + 1];
    ll_ = ll_ * decay_ + static_cast<double>(l) * l;
    rr_ = rr_ * decay_ + static_cast<double>(r) * r;
    lr_ = lr_ * decay_ + static_cast<double>(l) * r;
    const float side = (r - l) * kHalfRoot2, mid = (l + r) * kHalfRoot2;
    points_[2 * head_] = side;
    points_[2 * head_ + 1] = mid;
    head_ = (head_ + 1) % kPoints;
    count_ = std::min(count_ + 1, kPoints);
    envelope_ = std::max<double>({std::fabs(side), std::fabs(mid), envelope_ * envelope_decay});
  }
}

double PhaseMeter::Correlation() const {
  const double floor = 1e-9 / (1.0 - decay_);  // a mean power of -90 dBFS
  const bool left = ll_ > floor, right = rr_ > floor;
  if (!left && !right) return std::numeric_limits<double>::quiet_NaN();
  if (!left || !right) return 0.0;  // one channel silent: unrelated
  return std::clamp(lr_ / std::sqrt(ll_ * rr_), -1.0, 1.0);
}

double PhaseMeter::GainDb() const {
  double gain = std::min(1000.0, 0.85 / std::max(envelope_, 1e-6));
  return 20.0 * std::log10(gain);
}

void PhaseMeter::Points(std::vector<float>* xy) const {
  const float gain = static_cast<float>(std::pow(10.0, GainDb() / 20.0));
  xy->resize(2 * count_);
  size_t start = (head_ + kPoints - count_) % kPoints;
  for (size_t i = 0; i < count_; ++i) {
    size_t p = (start + i) % kPoints;
    (*xy)[2 * i] = std::clamp(points_[2 * p] * gain, -1.2f, 1.2f);
    (*xy)[2 * i + 1] = std::clamp(points_[2 * p + 1] * gain, -1.2f, 1.2f);
  }
}
