#include "air/spectrogram.hpp"

#include <algorithm>
#include <cmath>
#include <numbers>

#include "dsp/fft.hpp"
#include "util/parallel.hpp"

namespace geo::air {

namespace {

float median_of(std::vector<float>& v) {
  if (v.empty()) return NAN;
  auto mid = v.begin() + std::ptrdiff_t(v.size() / 2);
  std::nth_element(v.begin(), mid, v.end());
  return *mid;
}

}  // namespace

Spectrogram whitened_spectrogram(std::span<const double> x, const SpecOptions& o) {
  Spectrogram s;
  s.rate = o.rate, s.win = o.win, s.nfft = o.nfft, s.hop = o.hop;
  s.df = double(o.rate) / o.nfft;
  s.k0 = std::max(1, int(std::floor(o.fmin / s.df)));
  const int k1 = std::min(o.nfft / 2, int(std::ceil(o.fmax / s.df)));
  s.nb = k1 - s.k0 + 1;
  s.duration_s = double(x.size()) / o.rate;
  s.nf = x.size() < std::size_t(o.win) ? 0 : int((x.size() - std::size_t(o.win)) / std::size_t(o.hop)) + 1;
  if (s.nf <= 0 || s.nb <= 2) return s;
  const std::size_t NB = std::size_t(s.nb), NF = std::size_t(s.nf);
  s.z.assign(NF * NB, 0.0f);

  // 1. Log power, frames spread over the cores in blocks.
  std::vector<float> hann(std::size_t(o.win));
  for (int i = 0; i < o.win; ++i) hann[std::size_t(i)] = float(0.5 - 0.5 * std::cos(2 * std::numbers::pi * i / o.win));
  const auto& plan = fft::RealPlanF::get(o.nfft);
  const std::size_t block = 256;
  parallel_for((NF + block - 1) / block, [&](std::size_t b) {
    fft::Buf<float> in(std::size_t(o.nfft));
    fft::Buf<fft::cf> out(std::size_t(o.nfft / 2 + 1));
    for (std::size_t j = b * block; j < std::min(NF, (b + 1) * block); ++j) {
      const double* seg = x.data() + j * std::size_t(o.hop);
      double mean = 0;
      for (int i = 0; i < o.win; ++i) mean += seg[i];
      mean /= o.win;
      for (int i = 0; i < o.win; ++i) in[std::size_t(i)] = float(seg[i] - mean) * hann[std::size_t(i)];
      std::fill(in.begin() + o.win, in.end(), 0.0f);
      plan.forward(in.data(), out.data());
      float* r = s.z.data() + j * NB;
      for (std::size_t k = 0; k < NB; ++k) r[k] = std::log(std::norm(out[k + std::size_t(s.k0)]) + 1e-20f);
    }
  });

  // 2. Per-bin baseline: medians over blocks of ~baseline_s, linearly interpolated
  //    between block centres, subtracted.
  const std::size_t bl = std::max<std::size_t>(16, std::size_t(o.baseline_s * o.rate / o.hop));
  const std::size_t nblk = (NF + bl - 1) / bl;
  parallel_for(NB, [&](std::size_t k) {
    std::vector<float> med(nblk), tmp;
    for (std::size_t b = 0; b < nblk; ++b) {
      tmp.clear();
      for (std::size_t j = b * bl; j < std::min(NF, (b + 1) * bl); ++j) tmp.push_back(s.z[j * NB + k]);
      med[b] = median_of(tmp);
    }
    for (std::size_t j = 0; j < NF; ++j) {
      const double pos = (double(j) - 0.5 * double(bl - 1)) / double(bl);  // in block-centre units
      const std::size_t b0 = std::size_t(std::clamp(std::floor(pos), 0.0, double(nblk - 1)));
      const std::size_t b1 = std::min(nblk - 1, b0 + 1);
      const double w = std::clamp(pos - double(b0), 0.0, 1.0);
      s.z[j * NB + k] -= float((1 - w) * med[b0] + w * med[b1]);
    }
  });

  // 3. Per-frame median over the band (broadband events), then per-bin robust spread.
  parallel_for(NF, [&](std::size_t j) {
    std::vector<float> tmp(s.z.begin() + std::ptrdiff_t(j * NB), s.z.begin() + std::ptrdiff_t((j + 1) * NB));
    const float m = median_of(tmp);
    for (std::size_t k = 0; k < NB; ++k) s.z[j * NB + k] -= m;
  });
  parallel_for(NB, [&](std::size_t k) {
    std::vector<float> tmp;
    tmp.reserve(NF);
    for (std::size_t j = 0; j < NF; ++j) tmp.push_back(std::abs(s.z[j * NB + k]));
    const float sd = std::max(1e-3f, 1.4826f * median_of(tmp));
    for (std::size_t j = 0; j < NF; ++j) s.z[j * NB + k] = std::clamp(s.z[j * NB + k] / sd, o.clip_lo, o.clip_hi);
  });
  return s;
}

}  // namespace geo::air
