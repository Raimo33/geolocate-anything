#include "dsp/filters.hpp"

#include <algorithm>
#include <bit>
#include <cstdint>

namespace geo::dsp {

std::vector<double> hanning(std::size_t n) {
  std::vector<double> w(n);
  if (n == 1) {
    w[0] = 1.0;
    return w;
  }
  for (std::size_t k = 0; k < n; ++k) w[k] = 0.5 - 0.5 * std::cos(2.0 * M_PI * double(k) / double(n - 1));
  return w;
}

// Centred window sums from tile-local prefix sums (double) over the tile plus the
// window's halo: everything stays in cache, and the per-output loop has no carried
// dependency, so it vectorises. Interior outputs (the whole window inside the data)
// use affine indices; gap-free tiles skip the valid counts and multiply by 1 / w.
// HP: emit x - mean (the high-pass) directly.
template <class T, bool HP>
static void windowed_mean(std::span<const T> x, int w, std::span<T> out) {
  w = std::max(1, w) | 1;
  const std::ptrdiff_t n = static_cast<std::ptrdiff_t>(x.size()), h = w / 2;
  constexpr std::ptrdiff_t TILE = 16384;
  static thread_local std::vector<double> ps, pc;
  static thread_local std::vector<float> fv;
  ps.resize(std::size_t(TILE + 2 * h + 2));
  pc.resize(std::size_t(TILE + 2 * h + 2));
  fv.resize(std::size_t(TILE + 2 * h + 2));
  const double min_n = 0.3 * w, inv_w = 1.0 / w;
  const T* __restrict xi = x.data();
  T* __restrict o = out.data();
  auto put = [&](std::ptrdiff_t i, double cnt, double mean) {
    const T v = static_cast<T>(cnt < min_n ? NAN : mean);
    if constexpr (HP)
      o[i] = xi[i] == xi[i] ? xi[i] - v : T(NAN);
    else
      o[i] = v;
  };
  for (std::ptrdiff_t t0 = 0; t0 < n; t0 += TILE) {
    const std::ptrdiff_t t1 = std::min(n, t0 + TILE), lo = std::max<std::ptrdiff_t>(0, t0 - h), hi = std::min(n, t1 + h);
    const std::size_t len = std::size_t(hi - lo);
    const T* xs = xi + lo;
    const bool gaps = has_nan(xs, len);
    prefix_sums(xs, len, ps.data(), gaps);
    if (gaps) {
      for (std::size_t k = 0; k < len; ++k) fv[k] = xs[k] == xs[k] ? 1.0f : 0.0f;
      prefix_sums(fv.data(), len, pc.data());
    }
    const double* __restrict P = ps.data();  // P[k] = sum of x[lo, lo + k)
    const double* __restrict C = pc.data();
    const std::ptrdiff_t ia = std::clamp(lo + h, t0, t1), ib = std::clamp(hi - h, ia, t1);
    auto edge = [&](std::ptrdiff_t i) {
      const std::ptrdiff_t a = std::max(lo, i - h) - lo, b = std::min(hi, i + h + 1) - lo;
      const double cnt = gaps ? C[b] - C[a] : double(b - a);
      put(i, cnt, (P[b] - P[a]) / cnt);
    };
    for (std::ptrdiff_t i = t0; i < ia; ++i) edge(i);
    if (gaps)
      for (std::ptrdiff_t i = ia; i < ib; ++i) {
        const std::ptrdiff_t a = i - h - lo;
        const double cnt = C[a + w] - C[a];
        put(i, cnt, (P[a + w] - P[a]) / cnt);
      }
    else
      for (std::ptrdiff_t i = ia; i < ib; ++i) put(i, double(w), (P[i - h - lo + w] - P[i - h - lo]) * inv_w);
    for (std::ptrdiff_t i = ib; i < t1; ++i) edge(i);
  }
}

template <class T>
void moving_mean(std::span<const T> x, int w, std::span<T> out) {
  windowed_mean<T, false>(x, w, out);
}

template <class T>
void highpass(std::span<const T> x, int w, std::span<T> out) {
  if (w <= 1)
    std::copy(x.begin(), x.end(), out.begin());
  else
    windowed_mean<T, true>(x, w, out);
}

std::vector<double> highpass(std::span<const double> x, int w) {
  std::vector<double> o(x.size());
  highpass<double>(x, w, o);
  return o;
}

// o[i] = sum_j w[j] * x[i - j] for a compile-time tap count: the j loop unrolls and
// the i loop vectorises with every tap in a register.
template <class T, int N>
static void fir_fixed(const T* __restrict x, const T* __restrict w, T* __restrict o, std::ptrdiff_t len) {
  T wr[N];
  for (int j = 0; j < N; ++j) wr[j] = w[j];
  for (std::ptrdiff_t i = 0; i < len; ++i) {
    T acc = 0;
    for (int j = 0; j < N; ++j) acc += wr[j] * x[i - j];
    o[i] = acc;
  }
}

template <class T>
void smooth_like_frames(std::span<const T> v, double frame_s, std::span<T> out) {
  const int n = std::max(1, static_cast<int>(std::nearbyint(frame_s)));
  const std::ptrdiff_t len = static_cast<std::ptrdiff_t>(v.size());
  if (n <= 1) {
    std::copy(v.begin(), v.end(), out.begin());
    return;
  }
  auto hw = hanning(static_cast<std::size_t>(n + 2));
  std::vector<double> w(hw.begin() + 1, hw.end() - 1);
  double sum = 0;
  for (double x : w) sum += x;
  for (double& x : w) x /= sum;
  std::vector<T> wt(w.begin(), w.end());
  const std::ptrdiff_t off = (n - 1) / 2;
  // same[i] = sum_j a[i + off - j] w[j]: an n-tap FIR straight from the input over the
  // interior outputs (whole window inside the data), tiled so the tap loop vectorises
  // over i. A window over a gap comes out NaN here; it and the windows over an end are
  // recomputed below with their valid weight (elsewhere the weight is the full 1).
  static thread_local std::vector<std::ptrdiff_t> gaps;
  gaps.clear();
  for (std::size_t i = next_nan(v.data(), 0, v.size()); i < v.size(); i = next_nan(v.data(), i + 1, v.size()))
    gaps.push_back(std::ptrdiff_t(i));
  const std::ptrdiff_t ia = n - 1 - off, ib = len - off;  // interior outputs [ia, ib)
  if (ib > ia) {
    if (n == 12) {
      fir_fixed<T, 12>(v.data() + off + ia, wt.data(), out.data() + ia, ib - ia);  // the 12 s default: taps in registers
    } else {
      constexpr std::ptrdiff_t TILE = 4096;
      for (std::ptrdiff_t i0 = ia; i0 < ib; i0 += TILE) {
        const std::ptrdiff_t i1 = std::min(ib, i0 + TILE);
        T* __restrict o = out.data();
        for (std::ptrdiff_t i = i0; i < i1; ++i) o[i] = T(0);
        for (int j = 0; j < n; ++j) {
          const T wj = wt[j];
          const T* __restrict src = v.data() + (off - j);
          for (std::ptrdiff_t i = i0; i < i1; ++i) o[i] += wj * src[i];
        }
      }
    }
  }
  // Renormalise the outputs whose window touches a gap or an end: output i sees
  // inputs [i + off - n + 1, i + off], so input k affects outputs [k - off, k - off + n - 1].
  auto bad = [&](std::ptrdiff_t k) { return k < 0 || k >= len || v[std::size_t(k)] != v[std::size_t(k)]; };
  auto fix = [&](std::ptrdiff_t i) {
    double num = 0, den = 0;
    for (int j = 0; j < n; ++j) {
      const std::ptrdiff_t k = i + off - j;
      if (!bad(k)) {
        num += double(v[std::size_t(k)]) * w[j];
        den += w[j];
      }
    }
    // Exactly half the (symmetric) weight is "not under half": decide with a tolerance,
    // not on the last bit of the sum.
    out[std::size_t(i)] = static_cast<T>(den < 0.5 - 1e-9 ? NAN : num / den);
  };
  std::ptrdiff_t done = 0;  // outputs below this are final
  auto fix_range = [&](std::ptrdiff_t a, std::ptrdiff_t b) {
    for (std::ptrdiff_t i = std::max({a, done, std::ptrdiff_t(0)}); i < std::min(b, len); ++i) fix(i);
    done = std::max(done, b);
  };
  fix_range(0, n - off - 1);  // the start touches the padding
  for (auto k : gaps) fix_range(k - off, k - off + n);
  fix_range(len - off, len);  // the end touches the padding
}

std::vector<double> smooth_like_frames(std::span<const double> v, double frame_s) {
  std::vector<double> o(v.size());
  smooth_like_frames<double>(v, frame_s, o);
  return o;
}

template <class T>
void fill_short_gaps(std::span<T> x, int max_gap) {
  const std::size_t n = x.size();
  std::size_t i = 0;
  while (i < n && !finite(x[i])) ++i;  // leading NaNs have no left neighbour
  while (i < n) {
    i = next_nan(x.data(), i, n);  // skip the valid run
    if (i >= n) break;
    std::size_t a = i - 1, b = i;
    while (b < n && !finite(x[b])) ++b;
    if (b == n) break;
    if (b - a - 1 <= static_cast<std::size_t>(max_gap)) {
      const double xa = x[a], xb = x[b];
      const double num = static_cast<double>(b - a);
      for (std::size_t k = a + 1; k < b; ++k) x[k] = static_cast<T>(xa + double(k - a) * ((xb - xa) / num));  // numpy.linspace
    }
    i = b;
  }
}

template void moving_mean<float>(std::span<const float>, int, std::span<float>);
template void moving_mean<double>(std::span<const double>, int, std::span<double>);
template void highpass<float>(std::span<const float>, int, std::span<float>);
template void highpass<double>(std::span<const double>, int, std::span<double>);
template void smooth_like_frames<float>(std::span<const float>, double, std::span<float>);
template void smooth_like_frames<double>(std::span<const double>, double, std::span<double>);
template void fill_short_gaps<float>(std::span<float>, int);
template void fill_short_gaps<double>(std::span<double>, int);

template <class T>
static double median_impl(std::span<T> v) {
  if (v.empty()) return NAN;
  const std::size_t h = v.size() / 2;
  std::nth_element(v.begin(), v.begin() + h, v.end());
  double m = v[h];
  if (v.size() % 2 == 0) m = 0.5 * (m + double(*std::max_element(v.begin(), v.begin() + h)));
  return m;
}

double median_inplace(std::span<double> v) { return median_impl(v); }

// k-th smallest of v (no NaN) by radix selection on order-preserving keys: 11 bits per
// level, a histogram then a filter of the matching bucket, so a handful of predictable
// passes instead of nth_element's mispredicted partitions.
static float kth_smallest(std::span<const float> v, std::size_t k) {
  static thread_local std::vector<std::uint32_t> a, b;
  a.resize(v.size());
  for (std::size_t i = 0; i < v.size(); ++i) {
    const std::uint32_t u = std::bit_cast<std::uint32_t>(v[i]);
    a[i] = u & 0x80000000u ? ~u : u | 0x80000000u;
  }
  for (int shift : {21, 10}) {  // bits 31..21, then 20..10; nth_element finishes
    if (a.size() <= 64) break;
    constexpr std::uint32_t mask = 2047;
    std::uint32_t hist[2048] = {};
    for (std::uint32_t x : a) ++hist[(x >> shift) & mask];
    std::uint32_t d = 0;
    for (; k >= hist[d]; ++d) k -= hist[d];
    b.clear();
    for (std::uint32_t x : a)
      if (((x >> shift) & mask) == d) b.push_back(x);
    a.swap(b);
  }
  std::nth_element(a.begin(), a.begin() + std::ptrdiff_t(k), a.end());
  const std::uint32_t key = a[k];
  return std::bit_cast<float>(key & 0x80000000u ? key & 0x7FFFFFFFu : ~key);
}

double median_inplace(std::span<float> v) {
  if (v.size() < 512) return median_impl(v);
  const std::size_t h = v.size() / 2;
  double m = kth_smallest(v, h);
  if (v.size() % 2 == 0) m = 0.5 * (m + double(kth_smallest(v, h - 1)));
  return m;
}

double median(std::vector<double> v) { return median_inplace(v); }

}  // namespace geo::dsp
