#include "cuts.hpp"

#include <algorithm>
#include <cmath>
#include <numeric>
#include <stdexcept>

#include "dsp/filters.hpp"

namespace geo::cuts {

using fft::cd;

namespace {

// Complex baseband of [centre - half_bw, centre + half_bw] with a Hann-shaped band:
// a rectangular band rings for tens of seconds, which looks like phase jumps.
std::pair<std::vector<cd>, double> band(std::span<const cd> X, double T, double centre_hz, double half_bw) {
  const long k0 = std::lrint(centre_hz * T), K = std::lrint(half_bw * T);
  const long lo = k0 - K, hi = k0 + K + 1;
  if (lo < 1 || hi > static_cast<long>(X.size())) throw std::invalid_argument("harmonic band outside the spectrum");
  const std::size_t M = static_cast<std::size_t>(hi - lo);
  auto w = dsp::hanning(M);
  std::vector<cd> z(M);
  for (std::size_t i = 0; i < M; ++i) {
    std::size_t src = (i + static_cast<std::size_t>(K)) % M;
    z[i] = X[static_cast<std::size_t>(lo) + src] * w[src];
  }
  fft::backward_inplace(z);
  return {std::move(z), double(M) / T};
}

std::vector<double> running_median(const std::vector<double>& x, int k) {
  const std::ptrdiff_t n = static_cast<std::ptrdiff_t>(x.size());
  if (n == 0) return {};
  // numpy.pad(..., "symmetric"): reflect including the edge sample.
  auto at = [&](std::ptrdiff_t i) {
    while (i < 0 || i >= n) i = i < 0 ? -i - 1 : 2 * n - i - 1;
    return x[static_cast<std::size_t>(i)];
  };
  std::vector<double> out(x.size()), win(static_cast<std::size_t>(2 * k + 1));
  for (std::ptrdiff_t i = 0; i < n; ++i) {
    for (std::ptrdiff_t j = -k; j <= k; ++j) win[static_cast<std::size_t>(j + k)] = at(i + j);
    out[static_cast<std::size_t>(i)] = dsp::median_inplace(win);
  }
  return out;
}

double refine_cut(std::span<const cd> X, double T, double centre_hz, int harmonic, double t0, double win_s) {
  auto [z, fs_d] = band(X, T, centre_hz, std::max(1.0, 0.3 * harmonic));
  const long span = std::lrint(SIDE_PHASE * win_s * fs_d), ref = std::lrint(SIDE_FREQ * win_s * fs_d), i0 = std::lrint(t0 * fs_d);
  const long a = i0 - 2 * span, b = i0 + 2 * span;
  if (a - ref < 1 || b + ref >= static_cast<long>(z.size())) return t0;
  cd sl = 0, sr = 0;
  for (long i = a - ref; i < a; ++i) sl += z[std::size_t(i + 1)] * std::conj(z[std::size_t(i)]);
  for (long i = b; i < b + ref; ++i) sr += z[std::size_t(i + 1)] * std::conj(z[std::size_t(i)]);
  const double wl = std::arg(sl), wr = std::arg(sr);
  const std::size_t L = std::size_t(b - a);
  std::vector<cd> cl(L + 1, 0.0), cr(L + 1, 0.0);
  for (std::size_t k = 0; k < L; ++k) {
    const double i = double(a) + double(k);
    cl[k + 1] = cl[k] + z[std::size_t(a) + k] * std::polar(1.0, -wl * i);
    cr[k + 1] = cr[k] + z[std::size_t(a) + k] * std::polar(1.0, -wr * i);
  }
  double best = -INFINITY;
  long bp = span;
  for (long p = span; p <= 3 * span; ++p) {
    double sc = std::norm(cl[std::size_t(p)]) / double(p) + std::norm(cr[L] - cr[std::size_t(p)]) / double(b - a - p);
    if (sc > best) {
      best = sc;
      bp = p;
    }
  }
  return double(a + bp) / fs_d;
}

double floor_jump(const std::vector<double>& floor, std::ptrdiff_t k, std::ptrdiff_t span = 5) {
  const std::ptrdiff_t n = static_cast<std::ptrdiff_t>(floor.size());
  std::vector<double> left, right;
  for (std::ptrdiff_t i = std::max<std::ptrdiff_t>(0, k - span); i < std::min(k, n); ++i) left.push_back(floor[std::size_t(i)]);
  for (std::ptrdiff_t i = k; i < std::min(k + span, n); ++i) right.push_back(floor[std::size_t(i)]);
  if (left.empty() || right.empty()) return 0.0;
  return dsp::median(right) - dsp::median(left);
}

}  // namespace

std::vector<double> phase_steps(std::span<const cd> X, double T, double centre_hz, int harmonic, double win_s) {
  auto [z, fs_d] = band(X, T, centre_hz, std::max(1.0, 0.3 * harmonic));
  const long n = static_cast<long>(std::floor(T / win_s));
  std::vector<double> out(static_cast<std::size_t>(std::max(0L, n + 1)), 0.0);
  if (n < 2 * SIDE_FREQ + 2) return out;
  std::vector<cd> a(static_cast<std::size_t>(n), 0.0);
  for (std::size_t i = 0; i < z.size(); ++i) {
    long w = std::min(static_cast<long>(std::floor((double(i) / fs_d) / win_s)), n - 1);
    a[static_cast<std::size_t>(w)] += z[i];
  }
  std::vector<cd> cu(static_cast<std::size_t>(n), 0.0);  // cu[k] = sum of the first k unit steps
  for (long k = 0; k + 1 < n; ++k) {
    cd s = a[std::size_t(k + 1)] * std::conj(a[std::size_t(k)]);
    cu[std::size_t(k + 1)] = cu[std::size_t(k)] + std::polar(1.0, std::arg(s));
  }
  const long edge = static_cast<long>(std::ceil(EDGE_S / win_s));
  const long lo = std::max<long>(SIDE_FREQ + 1, edge), hi = n - std::max<long>(SIDE_FREQ, edge);
  if (hi - lo < 2) return out;
  std::vector<double> r(static_cast<std::size_t>(hi - lo));
  for (long b = lo; b < hi; ++b) {
    const double wl = std::arg(cu[std::size_t(b - 1)] - cu[std::size_t(b - SIDE_FREQ)]);
    const double wr = std::arg(cu[std::size_t(b + SIDE_FREQ - 1)] - cu[std::size_t(b)]);
    cd left = 0, right = 0;
    for (int j = 0; j < SIDE_PHASE; ++j) {
      left += a[std::size_t(b - 1 - j)] * std::polar(1.0, wl * (j + 0.5));
      right += a[std::size_t(b + j)] * std::polar(1.0, -wr * (j + 0.5));
    }
    r[std::size_t(b - lo)] = std::fabs(std::arg(right * std::conj(left)));
  }
  auto med = running_median(r, SPREAD_WIN);
  for (std::size_t i = 0; i < r.size(); ++i) out[std::size_t(lo) + i] = r[i] / std::max(1.4826 * med[i], MIN_SPREAD_RAD);
  return out;
}

std::vector<double> noise_floor_db(std::span<const double> x, double fs, double win_s, const std::vector<double>& hum_lines) {
  const std::size_t m = static_cast<std::size_t>(std::lrint(win_s * fs));
  const std::size_t n = m ? x.size() / m : 0;
  std::vector<double> out(n);
  if (!n) return out;
  auto win = dsp::hanning(m);
  fft::RealPlan plan(static_cast<int>(m));
  fft::Buf<double> in(m);
  fft::Buf<cd> spec(m / 2 + 1);
  std::vector<std::size_t> keep;
  for (std::size_t k = 0; k < m / 2 + 1; ++k) {
    double f = double(k) * fs / double(m);
    bool ok = f > 20;
    for (double c : hum_lines) ok = ok && std::fabs(f - c) > 3;
    if (ok) keep.push_back(k);
  }
  std::vector<double> p(keep.size());
  for (std::size_t t = 0; t < n; ++t) {
    double mean = 0;
    for (std::size_t i = 0; i < m; ++i) mean += x[t * m + i];
    mean /= double(m);
    for (std::size_t i = 0; i < m; ++i) in[i] = (x[t * m + i] - mean) * win[i];
    plan.forward(in.data(), spec.data());
    for (std::size_t j = 0; j < keep.size(); ++j) p[j] = std::norm(spec[keep[j]]);
    out[t] = 10 * std::log10(dsp::median_inplace(p) + 1e-30);
  }
  return out;
}

std::vector<Cut> detect_cuts(std::span<const double> x0, double fs, const enf::HumDetection& hum, std::size_t max_harmonics,
                             double win_s) {
  auto usable = hum.usable(max_harmonics);
  if (usable.empty()) return {};
  const std::size_t len = x0.size();
  const double mean = std::accumulate(x0.begin(), x0.end(), 0.0) / double(std::max<std::size_t>(1, len));
  const double T = double(len) / fs;
  const std::size_t ramp = std::min(len / 2, static_cast<std::size_t>(2 * fs));
  // Mean removed and both ends tapered, written straight into the in-place FFT buffer.
  auto X = fft::rfft_inplace(len, [&](double* d) {
    for (std::size_t i = 0; i < len; ++i) d[i] = x0[i] - mean;
    for (std::size_t i = 0; i < ramp; ++i) {
      double w = 0.5 - 0.5 * std::cos(M_PI * double(i) / double(ramp));
      d[i] *= w;
      d[len - 1 - i] *= w;
    }
  });
  std::span<const double> x = x0;  // the noise floor removes each window's own mean
  std::vector<int> keys;
  std::vector<std::vector<double>> Z;
  for (auto& h : usable) {
    try {
      Z.push_back(phase_steps(X.span(), T, h.centre_hz, h.harmonic, win_s));
      keys.push_back(h.harmonic);
    } catch (const std::invalid_argument&) {
    }
  }
  if (Z.empty()) return {};
  std::size_t n = Z[0].size();
  for (auto& v : Z) n = std::min(n, v.size());
  if (n == 0) return {};
  std::vector<double> best(n, -INFINITY);
  for (auto& v : Z)
    for (std::size_t b = 0; b < n; ++b) best[b] = std::max(best[b], v[b]);
  std::vector<double> centres;
  for (auto& h : usable) centres.push_back(h.centre_hz);
  auto floor = noise_floor_db(x, fs, win_s, centres);
  std::vector<std::size_t> order(n);
  std::iota(order.begin(), order.end(), 0);
  std::stable_sort(order.begin(), order.end(), [&](auto a, auto b) { return best[a] > best[b]; });
  const long sep = static_cast<long>(std::ceil(MIN_SEGMENT_S / win_s));
  std::vector<Cut> cuts;
  for (auto b : order) {
    if (best[b] < Z_SUPPORTED) break;
    const double tb = double(b) * win_s;
    if (std::any_of(cuts.begin(), cuts.end(), [&](auto& c) { return std::fabs(tb - c.time_s) < double(sep) * win_s; })) continue;
    double jump = floor_jump(floor, static_cast<std::ptrdiff_t>(b));
    if (best[b] >= Z_CUT || std::fabs(jump) >= FLOOR_JUMP_DB) {
      Cut c{tb, best[b], jump, {}};
      for (std::size_t i = 0; i < Z.size(); ++i)
        if (Z[i][b] >= Z_SUPPORTED) c.harmonics.push_back(keys[i]);
      cuts.push_back(std::move(c));
    }
  }
  for (auto& c : cuts) {
    std::size_t col = static_cast<std::size_t>(std::lrint(c.time_s / win_s));
    std::size_t bi = 0;
    for (std::size_t i = 1; i < Z.size(); ++i)
      if (Z[i][col] > Z[bi][col]) bi = i;
    const auto& h = *std::find_if(usable.begin(), usable.end(), [&](auto& u) { return u.harmonic == keys[bi]; });
    c.time_s = refine_cut(X.span(), T, h.centre_hz, h.harmonic, c.time_s, win_s);
  }
  std::stable_sort(cuts.begin(), cuts.end(), [](auto& a, auto& b) { return a.time_s < b.time_s; });
  const double edge = std::max(MIN_SEGMENT_S, EDGE_S);
  std::erase_if(cuts, [&](auto& c) { return !(edge <= c.time_s && c.time_s <= T - edge); });
  return cuts;
}

std::vector<Cut> clean_cuts(double duration_s, std::vector<Cut> cuts, double min_gap_s) {
  std::stable_sort(cuts.begin(), cuts.end(), [](auto& a, auto& b) { return a.time_s < b.time_s; });
  std::vector<Cut> out;
  for (auto& c : cuts) {
    double last = out.empty() ? 0.0 : out.back().time_s;
    if (c.time_s - last >= min_gap_s && duration_s - c.time_s >= min_gap_s) out.push_back(c);
  }
  return out;
}

std::vector<Segment> segments_from_cuts(double duration_s, const std::vector<Cut>& cuts) {
  std::vector<double> edges{0.0};
  for (auto& c : clean_cuts(duration_s, cuts)) edges.push_back(c.time_s);
  edges.push_back(duration_s);
  std::vector<Segment> out;
  for (std::size_t i = 0; i + 1 < edges.size(); ++i) out.push_back({edges[i], edges[i + 1]});
  return out;
}

}  // namespace geo::cuts
