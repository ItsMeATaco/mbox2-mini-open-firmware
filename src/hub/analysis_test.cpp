// Checks the hub's audio analysis (hub/analysis.h) against known signals: the K-weighting coefficients
// printed in ITU-R BS.1770-4, 1 kHz sines (one channel at -20 dBFS reads -23.0 LUFS, both read -20.0),
// relative gating, EBU Tech 3342 loudness-range signals, a true-peak sine whose peaks fall between samples,
// the spectrum level of a sine and the phase meter on mono, inverted and unrelated signals.
//
//   mbox2hub_analysis_test        prints each check; exit code 1 if any fails

#include <cmath>
#include <cstdio>
#include <random>
#include <vector>

#include "hub/analysis.h"

namespace {

constexpr double kPi = 3.14159265358979323846;
int failures = 0;

void Check(const char* what, double got, double want, double tolerance) {
  bool ok = std::fabs(got - want) <= tolerance;
  if (!ok) ++failures;
  printf("%-58s %10.3f  (want %8.3f +/- %.3f)  %s\n", what, got, want, tolerance, ok ? "ok" : "FAIL");
}

struct Segment {
  double seconds;
  double left_db, right_db;  // sine amplitude per channel in dBFS; <= -200 for silence
};

std::vector<float> Sine(uint32_t rate, double hz, const std::vector<Segment>& segments, double phase = 0.0) {
  std::vector<float> out;
  size_t n = 0;
  for (const Segment& s : segments) {
    double a_left = s.left_db <= -200.0 ? 0.0 : std::pow(10.0, s.left_db / 20.0);
    double a_right = s.right_db <= -200.0 ? 0.0 : std::pow(10.0, s.right_db / 20.0);
    size_t frames = static_cast<size_t>(s.seconds * rate);
    for (size_t i = 0; i < frames; ++i, ++n) {
      double v = std::sin(2.0 * kPi * hz * n / rate + phase);
      out.push_back(static_cast<float>(a_left * v));
      out.push_back(static_cast<float>(a_right * v));
    }
  }
  return out;
}

LoudnessMeter Measure(uint32_t rate, const std::vector<float>& stereo) {
  LoudnessMeter meter;
  meter.Reset(rate);
  // In UI-sized chunks, as the hub feeds it.
  for (size_t i = 0; i < stereo.size() / 2; i += 1600) {
    meter.Process(&stereo[2 * i], std::min<size_t>(1600, stereo.size() / 2 - i));
  }
  return meter;
}

}  // namespace

int main() {
  printf("-- K-weighting at 48 kHz against BS.1770-4 Table 1 and 2\n");
  Biquad shelf, highpass;
  LoudnessMeter::KWeighting(48000, &shelf, &highpass);
  Check("shelf b0", shelf.b0, 1.53512485958697, 1e-6);
  Check("shelf b1", shelf.b1, -2.69169618940638, 1e-6);
  Check("shelf b2", shelf.b2, 1.19839281085285, 1e-6);
  Check("shelf a1", shelf.a1, -1.69065929318241, 1e-6);
  Check("shelf a2", shelf.a2, 0.73248077421585, 1e-6);
  Check("high-pass a1", highpass.a1, -1.99004745483398, 1e-6);
  Check("high-pass a2", highpass.a2, 0.99007225036621, 1e-6);

  printf("-- Loudness of 1 kHz sines (20 s)\n");
  for (uint32_t rate : {48000u, 44100u}) {
    LoudnessMeter one = Measure(rate, Sine(rate, 1000.0, {{20.0, -20.0, -300.0}}));
    LoudnessMeter both = Measure(rate, Sine(rate, 1000.0, {{20.0, -20.0, -20.0}}));
    char label[96];
    snprintf(label, sizeof label, "%u Hz, -20 dBFS left only: momentary", rate);
    Check(label, one.Momentary(), -23.0, 0.1);
    snprintf(label, sizeof label, "%u Hz, -20 dBFS left only: short-term", rate);
    Check(label, one.ShortTerm(), -23.0, 0.1);
    snprintf(label, sizeof label, "%u Hz, -20 dBFS left only: integrated", rate);
    Check(label, one.Integrated(), -23.0, 0.1);
    snprintf(label, sizeof label, "%u Hz, -20 dBFS both channels: integrated", rate);
    Check(label, both.Integrated(), -20.0, 0.1);
    snprintf(label, sizeof label, "%u Hz, -20 dBFS both channels: true peak (dBTP)", rate);
    Check(label, both.TruePeakMax(), -20.0, 0.1);
    snprintf(label, sizeof label, "%u Hz, steady sine: loudness range (LU)", rate);
    Check(label, both.Range(), 0.0, 0.2);
  }

  printf("-- Relative gate (EBU Tech 3341 case 3: -36 / -23 / -36 dBFS, 10 / 20 / 10 s)\n");
  {
    LoudnessMeter m = Measure(48000, Sine(48000, 1000.0, {{10, -36, -36}, {20, -23, -23}, {10, -36, -36}}));
    Check("integrated", m.Integrated(), -23.0, 0.1);
  }
  printf("-- Absolute gate: 10 s at -80 dBFS around 20 s at -23\n");
  {
    LoudnessMeter m = Measure(48000, Sine(48000, 1000.0, {{10, -80, -80}, {20, -23, -23}, {10, -80, -80}}));
    Check("integrated", m.Integrated(), -23.0, 0.1);
  }

  printf("-- Loudness range (EBU Tech 3342 cases 1-3: 20 s + 20 s)\n");
  {
    Check("-20 then -30 dBFS", Measure(48000, Sine(48000, 1000.0, {{20, -20, -20}, {20, -30, -30}})).Range(), 10.0, 1.0);
    Check("-20 then -15 dBFS", Measure(48000, Sine(48000, 1000.0, {{20, -20, -20}, {20, -15, -15}})).Range(), 5.0, 1.0);
    Check("-40 then -20 dBFS", Measure(48000, Sine(48000, 1000.0, {{20, -40, -40}, {20, -20, -20}})).Range(), 20.0, 1.0);
  }

  printf("-- True peak: 12 kHz at 48 kHz, peaks between samples (samples at -3 dB of the peak)\n");
  {
    // Phase 45 degrees: every sample lands at +-0.707 of the amplitude; the waveform peaks in between.
    LoudnessMeter m = Measure(48000, Sine(48000, 12000.0, {{2.0, -6.02, -6.02}}, kPi / 4));
    Check("amplitude -6.02 dBFS: true peak (dBTP)", m.TruePeakMax(), -6.02, 0.3);
    LoudnessMeter full = Measure(48000, Sine(48000, 12000.0, {{2.0, 0.0, 0.0}}, kPi / 4));
    Check("amplitude 0 dBFS, samples at -3.01: true peak (dBTP)", full.TruePeakMax(), 0.0, 0.3);
    LoudnessMeter low = Measure(48000, Sine(48000, 997.0, {{2.0, -1.0, -1.0}}));
    Check("997 Hz at -1 dBFS: true peak (dBTP)", low.TruePeakMax(), -1.0, 0.1);
  }

  printf("-- Spectrum (both channels, 1/6 and 1/3 octave, 320 columns)\n");
  for (double octaves : {1.0 / 6.0, 1.0 / 3.0}) {
    SpectrumAnalyzer spectrum;
    spectrum.Reset(48000);
    std::vector<float> tone = Sine(48000, 1000.0, {{1.0, -20.0, -20.0}});
    spectrum.Push(tone.data(), tone.size() / 2);
    spectrum.Update(320, octaves, 0.033f);
    int at_1k = 0, at_100 = 0;
    for (int j = 0; j < 320; ++j) {
      double f = SpectrumAnalyzer::ColumnFrequency(j, 320);
      if (std::fabs(std::log(f / 1000.0)) < std::fabs(std::log(SpectrumAnalyzer::ColumnFrequency(at_1k, 320) / 1000.0))) at_1k = j;
      if (std::fabs(std::log(f / 100.0)) < std::fabs(std::log(SpectrumAnalyzer::ColumnFrequency(at_100, 320) / 100.0))) at_100 = j;
    }
    char label[96];
    snprintf(label, sizeof label, "1/%.0f oct: -20 dBFS 1 kHz sine, level at 1 kHz", 1.0 / octaves);
    Check(label, spectrum.Level()[at_1k], -20.0, 1.0);
    snprintf(label, sizeof label, "1/%.0f oct: same sine, level at 100 Hz (below -90)", 1.0 / octaves);
    Check(label, std::max<double>(spectrum.Level()[at_100], -90.0), -90.0, 0.0);
  }
  {
    SpectrumAnalyzer spectrum;
    spectrum.Reset(48000);
    std::vector<float> tone = Sine(48000, 50.0, {{1.0, -20.0, -20.0}});
    spectrum.Push(tone.data(), tone.size() / 2);
    spectrum.Update(320, 1.0 / 6.0, 0.033f);
    float best = SpectrumAnalyzer::kFloorDb;
    for (float v : spectrum.Level()) best = std::max(best, v);
    Check("1/6 oct: -20 dBFS 50 Hz sine, highest level", best, -20.0, 1.5);
  }

  printf("-- Phase meter (1 s of 440 Hz, or noise)\n");
  {
    PhaseMeter phase;
    phase.Reset(48000);
    std::vector<float> mono = Sine(48000, 440.0, {{1.0, -10.0, -10.0}});
    phase.Push(mono.data(), mono.size() / 2);
    Check("mono: correlation", phase.Correlation(), 1.0, 0.01);
    std::vector<float> xy;
    phase.Points(&xy);
    double widest = 0.0;
    for (size_t i = 0; i < xy.size(); i += 2) widest = std::max(widest, std::fabs(static_cast<double>(xy[i])));
    Check("mono: goniometer stays on the vertical (largest |x|)", widest, 0.0, 0.01);

    std::vector<float> inverted = mono;
    for (size_t i = 1; i < inverted.size(); i += 2) inverted[i] = -inverted[i];
    phase.Reset(48000);
    phase.Push(inverted.data(), inverted.size() / 2);
    Check("left = -right: correlation", phase.Correlation(), -1.0, 0.01);

    std::mt19937 rng(1);
    std::normal_distribution<float> noise(0.0f, 0.1f);
    std::vector<float> unrelated(2 * 48000);
    for (float& v : unrelated) v = noise(rng);
    phase.Reset(48000);
    phase.Push(unrelated.data(), unrelated.size() / 2);
    Check("independent noise: correlation", phase.Correlation(), 0.0, 0.1);

    std::vector<float> left_only = Sine(48000, 440.0, {{1.0, -10.0, -300.0}});
    phase.Reset(48000);
    phase.Push(left_only.data(), left_only.size() / 2);
    Check("left only: correlation", phase.Correlation(), 0.0, 0.01);
  }

  printf("%s\n", failures ? "SOME CHECKS FAILED" : "all checks passed");
  return failures ? 1 : 0;
}
