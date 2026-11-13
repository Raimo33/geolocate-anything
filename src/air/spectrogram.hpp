// Whitened spectrogram for aircraft tones.
//
// Log power minus each bin's slowly varying baseline (block medians over ~45 s,
// interpolated), minus each frame's median over the band (broadband gusts, traffic),
// divided by each bin's robust spread: a robust z-score per cell, clipped. Stationary
// tones (mains hum, fans, insects at one pitch) and the coloured noise floor go; a
// moving tone stands out by a few units wherever it passes, so sums along a modelled
// curve behave like sums of unit-variance noise plus signal.
#pragma once

#include <span>
#include <vector>

namespace geo::air {

inline constexpr int SPEC_RATE = 4000;  // analysis rate: tones up to 2 kHz

struct SpecOptions {
  int rate = SPEC_RATE;
  int win = 2048;   // 0.512 s Hann window
  int nfft = 4096;  // ~0.98 Hz bins
  int hop = 512;    // 0.128 s
  double fmin = 20.0, fmax = 1200.0;
  double baseline_s = 45.0;
  float clip_lo = -3.0f, clip_hi = 8.0f;
};

struct Spectrogram {
  int rate = SPEC_RATE, win = 0, nfft = 0, hop = 0;
  double df = 0;  // Hz per bin
  int k0 = 0;     // first stored bin (frequency k0 * df)
  int nb = 0;     // stored bins
  int nf = 0;     // frames
  std::vector<float> z;  // nf x nb
  double duration_s = 0;

  double frame_time(double j) const { return (j * hop + win * 0.5) / rate; }  // centre, s from start
  double frame_at(double t) const { return (t * rate - win * 0.5) / hop; }
  double bin_at(double f) const { return f / df - k0; }
  double freq(double bin) const { return (bin + k0) * df; }
  double fmax() const { return freq(nb - 1); }
  const float* row(int j) const { return z.data() + std::size_t(j) * std::size_t(nb); }
  // Linear interpolation in frequency at frame j (NaN outside the band or the clip).
  float at(int j, double f) const {
    const double b = bin_at(f);
    if (j < 0 || j >= nf || b < 0 || b > nb - 1) return NAN;
    const int i = std::min(nb - 2, int(b));
    const double w = b - i;
    const float* r = row(j);
    return float((1 - w) * r[i] + w * r[i + 1]);
  }
};

Spectrogram whitened_spectrogram(std::span<const double> x, const SpecOptions& o = {});

}  // namespace geo::air
