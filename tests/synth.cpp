#include "synth.hpp"

#include <algorithm>
#include <cmath>
#include <random>
#include <stdexcept>

namespace geo::synth {

std::vector<double> hum_from_reference(const store::RefSeries& ref, i64 start_utc, double duration_s, int nominal, int rate,
                                       const std::map<int, double>& harmonics, double clock_ppm, std::uint64_t seed) {
  std::mt19937_64 rng(seed);
  std::uniform_real_distribution<double> phase0(0.0, 2 * M_PI);
  const auto n = static_cast<std::size_t>(std::nearbyint(duration_s * rate));
  const i64 i0 = start_utc - ref.start;
  if (i0 < 0 || double(i0) + duration_s + 2 > double(ref.values.size()))
    throw std::invalid_argument("requested window is outside the reference series");
  const auto len = static_cast<std::size_t>(std::ceil(duration_s) + 2);
  std::vector<double> seg(len);
  for (std::size_t k = 0; k < len; ++k) seg[k] = ref.values[std::size_t(i0) + k];
  // Bridge missing seconds linearly (constant beyond the ends).
  std::vector<std::size_t> ok;
  for (std::size_t k = 0; k < len; ++k)
    if (std::isfinite(seg[k])) ok.push_back(k);
  if (ok.size() < 2) throw std::invalid_argument("reference window is empty");
  if (ok.size() < len) {
    std::vector<double> filled(len);
    std::size_t j = 0;
    for (std::size_t k = 0; k < len; ++k) {
      while (j + 1 < ok.size() && ok[j + 1] <= k) ++j;
      if (k <= ok.front()) filled[k] = seg[ok.front()];
      else if (k >= ok.back()) filled[k] = seg[ok.back()];
      else {
        std::size_t a = ok[j], b = ok[j + 1];
        filled[k] = seg[a] + (seg[b] - seg[a]) * double(k - a) / double(b - a);
      }
    }
    seg = std::move(filled);
  }
  // Reference value k is the mean over second [k, k + 1): placed at k + 0.5.
  std::vector<double> out(n, 0.0);
  std::vector<std::pair<int, double>> hs(harmonics.begin(), harmonics.end());
  std::vector<double> ph0;
  for (std::size_t i = 0; i < hs.size(); ++i) ph0.push_back(phase0(rng));
  double phase = 0;
  const double scale = 1.0 + clock_ppm * 1e-6;
  for (std::size_t i = 0; i < n; ++i) {
    double t = double(i) / rate - 0.5;
    double dev;
    if (t <= 0) dev = seg[0];
    else if (t >= double(len - 1)) dev = seg[len - 1];
    else {
      auto k = static_cast<std::size_t>(t);
      double f = t - double(k);
      dev = seg[k] + f * (seg[k + 1] - seg[k]);
    }
    phase += 2 * M_PI * (nominal + dev) * scale / rate;
    double v = 0;
    for (std::size_t j = 0; j < hs.size(); ++j) v += hs[j].second * std::sin(hs[j].first * phase + ph0[j]);
    out[i] = v;
  }
  double mx = 1e-9;
  for (double v : out) mx = std::max(mx, std::fabs(v));
  for (double& v : out) v /= mx;
  return out;
}

std::vector<double> bury(const std::vector<double>& signal, double snr_db, std::uint64_t seed, const std::vector<double>* background) {
  std::vector<double> bg(signal.size());
  if (!background || background->empty()) {
    std::mt19937_64 rng(seed ^ 0x9E3779B97F4A7C15ULL);
    std::normal_distribution<double> nd;
    for (auto& v : bg) v = nd(rng);
  } else {
    for (std::size_t i = 0; i < bg.size(); ++i) bg[i] = (*background)[i % background->size()];
  }
  double ps = 0, pb = 0;
  for (std::size_t i = 0; i < signal.size(); ++i) {
    ps += signal[i] * signal[i];
    pb += bg[i] * bg[i];
  }
  ps /= double(std::max<std::size_t>(1, signal.size()));
  pb = pb / double(std::max<std::size_t>(1, signal.size())) + 1e-30;
  const double g = std::sqrt(ps / (pb * std::pow(10.0, snr_db / 10)));
  std::vector<double> mix(signal.size());
  double mx = 0;
  for (std::size_t i = 0; i < mix.size(); ++i) {
    mix[i] = signal[i] + g * bg[i];
    mx = std::max(mx, std::fabs(mix[i]));
  }
  for (auto& v : mix) v = 0.9 * v / (mx > 0 ? mx : 1.0);
  return mix;
}

}  // namespace geo::synth
