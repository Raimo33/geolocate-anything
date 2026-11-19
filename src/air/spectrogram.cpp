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

// Per-sample-block speech flags (blocks of `hop` samples at `rate`), from the syllable-rate
// modulation of the 300-1800 Hz band energy: 32 ms frames every 10 ms, 1.28 s windows every 0.1 s.
std::vector<unsigned char> speech_blocks(std::span<const double> x, int rate, const SpecOptions& o, std::size_t& nwin_out) {
  const int n = 128 * rate / 4000, hop = 40 * rate / 4000;
  if (x.size() < std::size_t(n + hop)) return {};
  const std::size_t nf = (x.size() - std::size_t(n)) / std::size_t(hop);
  int nfft = 1;
  while (nfft < n) nfft *= 2;
  const auto& plan = fft::RealPlanF::get(nfft);
  std::vector<float> hann(static_cast<std::size_t>(n));
  for (int i = 0; i < n; ++i) hann[std::size_t(i)] = float(0.5 - 0.5 * std::cos(2 * std::numbers::pi * i / n));
  const int b0 = int(std::ceil(300.0 * nfft / rate)), b1 = int(std::floor(1800.0 * nfft / rate));
  std::vector<float> e(nf);
  const std::size_t block = 4096;
  parallel_for((nf + block - 1) / block, [&](std::size_t b) {
    fft::Buf<float> in(static_cast<std::size_t>(nfft));
    fft::Buf<fft::cf> out(static_cast<std::size_t>(nfft / 2 + 1));
    for (std::size_t j = b * block; j < std::min(nf, (b + 1) * block); ++j) {
      const double* seg = x.data() + j * std::size_t(hop);
      for (int i = 0; i < n; ++i) in[std::size_t(i)] = float(seg[i]) * hann[std::size_t(i)];
      std::fill(in.begin() + n, in.end(), 0.0f);
      plan.forward(in.data(), out.data());
      double acc = 0;
      for (int k = b0; k <= b1; ++k) acc += std::norm(out[std::size_t(k)]);
      e[j] = float(std::log(acc + 1e-20));
    }
  });
  // Modulation spectrum of each 128-frame (1.28 s) window, every 10 frames.
  constexpr int L = 128, STEP = 10;
  const double fs_env = double(rate) / hop;  // 100 Hz
  const auto& mplan = fft::RealPlanF::get(L);
  std::vector<float> mh(static_cast<std::size_t>(L));
  for (int i = 0; i < L; ++i) mh[std::size_t(i)] = float(0.5 - 0.5 * std::cos(2 * std::numbers::pi * i / L));
  const std::size_t nwin = nf >= std::size_t(L) ? (nf - L) / STEP + 1 : 0;
  std::vector<unsigned char> win(nwin, 0);
  parallel_for((nwin + 255) / 256, [&](std::size_t b) {
    fft::Buf<float> in(static_cast<std::size_t>(L));
    fft::Buf<fft::cf> out(static_cast<std::size_t>(L / 2 + 1));
    for (std::size_t w = b * 256; w < std::min(nwin, (b + 1) * 256); ++w) {
      const float* seg = e.data() + w * STEP;
      double mean = 0, sq = 0;
      for (int i = 0; i < L; ++i) mean += seg[i];
      mean /= L;
      for (int i = 0; i < L; ++i) sq += (seg[i] - mean) * (seg[i] - mean);
      const double sd = std::sqrt(sq / L);
      if (sd < o.speech_sd) continue;
      for (int i = 0; i < L; ++i) in[std::size_t(i)] = float(seg[i] - mean) * mh[std::size_t(i)];
      mplan.forward(in.data(), out.data());
      double syl = 0, all = 0;
      for (int k = 1; k <= L / 2; ++k) {
        const double f = k * fs_env / L, p = std::norm(out[std::size_t(k)]);
        if (f >= 0.5 && f <= 20) all += p;
        if (f >= 2 && f <= 8) syl += p;
      }
      win[w] = all > 0 && syl >= o.speech_mod * all;
    }
  });
  nwin_out = nwin;
  // Back to 10 ms frames: a frame is speech if a window holding its centre is.
  std::vector<unsigned char> fr(nf, 0);
  for (std::size_t w = 0; w < nwin; ++w)
    if (win[w])
      for (std::size_t j = w * STEP + L / 4; j < std::min(nf, w * STEP + 3 * L / 4); ++j) fr[j] = 1;
  return fr;
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

  // Speech frames (see SpecOptions): flags at 10 ms, a spectrogram frame is speech when
  //    most of its window is, dilated by half a second each way.
  std::vector<unsigned char>& sp = s.speech;
  sp.assign(NF, 0);
  std::size_t nsp = 0;
  if (o.speech_mask) {
    std::size_t nwin = 0;
    const auto fr = speech_blocks(x, o.rate, o, nwin);
    const double fdt = 0.01;  // speech_blocks' frame step, s
    std::vector<unsigned char> raw(NF, 0);
    for (std::size_t j = 0; j < NF; ++j) {
      const double t0 = double(j) * o.hop / o.rate, t1 = t0 + double(o.win) / o.rate;
      const std::size_t a = std::size_t(t0 / fdt), b = std::min(fr.size(), std::size_t(t1 / fdt));
      std::size_t on = 0;
      for (std::size_t k = a; k < b; ++k) on += fr[k];
      raw[j] = b > a && 2 * on >= b - a;
    }
    const std::size_t dil = std::size_t(std::lround(0.5 * o.rate / o.hop));
    for (std::size_t j = 0; j < NF; ++j)
      if (raw[j])
        for (std::size_t k = j > dil ? j - dil : 0; k <= std::min(NF - 1, j + dil); ++k) sp[k] = 1;
    for (auto v : sp) nsp += v;
  }
  s.speech_fraction = double(nsp) / double(NF);
  s.speech_cap = o.speech_cap;
  const bool spread_off_speech = nsp > 0 && NF - nsp >= NF / 5;  // enough other frames to measure on

  // 3. Per-frame median over the band (broadband events), then per-bin robust spread.
  parallel_for(NF, [&](std::size_t j) {
    std::vector<float> tmp(s.z.begin() + std::ptrdiff_t(j * NB), s.z.begin() + std::ptrdiff_t((j + 1) * NB));
    const float m = median_of(tmp);
    for (std::size_t k = 0; k < NB; ++k) s.z[j * NB + k] -= m;
  });
  parallel_for(NB, [&](std::size_t k) {
    std::vector<float> tmp;
    tmp.reserve(NF);
    for (std::size_t j = 0; j < NF; ++j)
      if (!spread_off_speech || !sp[j]) tmp.push_back(std::abs(s.z[j * NB + k]));
    const float sd = std::max(1e-3f, 1.4826f * median_of(tmp));
    for (std::size_t j = 0; j < NF; ++j)
      s.z[j * NB + k] = std::clamp(s.z[j * NB + k] / sd, o.clip_lo, sp[j] ? std::min(o.clip_hi, o.speech_cap) : o.clip_hi);
  });
  return s;
}

}  // namespace geo::air
