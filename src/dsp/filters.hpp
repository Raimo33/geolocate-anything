// O(n) NaN-aware filters shared by extraction and search (NaN = missing sample).
#pragma once

#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <immintrin.h>
#include <type_traits>
#include <span>
#include <vector>

namespace geo::dsp {

inline bool finite(double x) { return std::isfinite(x); }

// numpy.hanning(n): 0.5 - 0.5 cos(2 pi k / (n - 1)).
std::vector<double> hanning(std::size_t n);

// Centred moving average over an odd window w (w |= 1); NaN where fewer than 0.3 w
// samples in the window are valid. Zero padding at the ends (counts as missing).
// Sums are kept in double whatever T is.
template <class T>
void moving_mean(std::span<const T> x, int w, std::span<T> out);
// x minus its centred moving average (NaN-aware); w <= 1 copies.
template <class T>
void highpass(std::span<const T> x, int w, std::span<T> out);
std::vector<double> highpass(std::span<const double> x, int w);

// Hann-weighted average matching what one extractor frame of frame_s seconds sees
// (numpy.convolve(..., "same") semantics); NaN where under half the weight is valid.
template <class T>
void smooth_like_frames(std::span<const T> v, double frame_s, std::span<T> out);
std::vector<double> smooth_like_frames(std::span<const double> v, double frame_s);

// Linearly bridge NaN runs of at most max_gap samples, in place.
template <class T>
void fill_short_gaps(std::span<T> x, int max_gap = 10);

// Any NaN in x[0, n)? Or-ed into a 32-bit lane mask so it vectorises without
// widening (a size_t count costs a zero-extend and a 64-bit add per element).
template <class T>
inline bool has_nan(const T* __restrict x, std::size_t n) {
  std::uint32_t c = 0;
  for (std::size_t i = 0; i < n; ++i) c |= std::uint32_t(x[i] != x[i]);
  return c != 0;
}

// Index of the first NaN in x[i, n), or n. NaN-free stretches are skipped 32 floats
// (64 doubles) per test, so sparse gaps cost almost nothing to find.
template <class T>
inline std::size_t next_nan(const T* x, std::size_t i, std::size_t n) {
  if constexpr (std::is_same_v<T, float>) {
    for (; i + 32 <= n; i += 32) {
      const __m256 a = _mm256_loadu_ps(x + i), b = _mm256_loadu_ps(x + i + 8), c = _mm256_loadu_ps(x + i + 16),
                   d = _mm256_loadu_ps(x + i + 24);
      const __m256 u = _mm256_or_ps(_mm256_or_ps(_mm256_cmp_ps(a, a, _CMP_UNORD_Q), _mm256_cmp_ps(b, b, _CMP_UNORD_Q)),
                                    _mm256_or_ps(_mm256_cmp_ps(c, c, _CMP_UNORD_Q), _mm256_cmp_ps(d, d, _CMP_UNORD_Q)));
      if (_mm256_movemask_ps(u)) break;
    }
  } else {
    constexpr std::size_t B = 64;
    while (i + B <= n && !has_nan(x + i, B)) i += B;
  }
  while (i < n && x[i] == x[i]) ++i;
  return i;
}

// Sum, sum of squares and count of the non-NaN values of x[0, n): float lanes within
// short runs (no widening per element), double across them.
inline void finite_sums(const float* __restrict x, std::size_t n, double& s1, double& s2, std::size_t& cnt) {
  constexpr std::size_t R = 512;
  s1 = s2 = 0;
  cnt = 0;
  for (std::size_t r0 = 0; r0 < n; r0 += R) {
    const std::size_t len = n - r0 < R ? n - r0 : R;
    float a = 0, b = 0;
    std::uint32_t c = 0;
    for (std::size_t i = 0; i < len; ++i) {
      const float v = x[r0 + i];
      const bool f = v == v;
      const float u = f ? v : 0.0f;
      a += u;
      b += u * u;
      c += f;
    }
    s1 += a;
    s2 += b;
    cnt += c;
  }
}

// Inclusive scan of four doubles (in-register).
inline __m256d scan4(__m256d v) {
  v = _mm256_add_pd(v, _mm256_blend_pd(_mm256_permute4x64_pd(v, 0x90), _mm256_setzero_pd(), 0x1));  // + [0 v0 v1 v2]
  return _mm256_add_pd(v, _mm256_permute2f128_pd(v, v, 0x08));                                     // + [0 0 s0 s1]
}

// out[0] = 0, out[i + 1] = x[0] + ... + x[i] (double), with NaN counted as 0 when
// zero_nan. Blocked so additions overlap instead of forming one latency chain.
template <class T>
inline void prefix_sums(const T* __restrict x, std::size_t n, double* __restrict out, bool zero_nan = false) {
  double carry = 0;
  out[0] = 0;
  std::size_t i = 0;
  if constexpr (std::is_same_v<T, float>) {
    // Eight samples per step: two in-register scans, offset by the carry; the carry
    // advances by the step's total, computed off the loop-carried chain.
    __m256d c = _mm256_setzero_pd();
    for (; i + 8 <= n; i += 8) {
      __m256 v = _mm256_loadu_ps(x + i);
      if (zero_nan) v = _mm256_and_ps(v, _mm256_cmp_ps(v, v, _CMP_ORD_Q));
      const __m256d a = scan4(_mm256_cvtps_pd(_mm256_castps256_ps128(v)));
      const __m256d b = scan4(_mm256_cvtps_pd(_mm256_extractf128_ps(v, 1)));
      const __m256d ta = _mm256_permute4x64_pd(a, 0xFF), tb = _mm256_permute4x64_pd(b, 0xFF);
      _mm256_storeu_pd(out + i + 1, _mm256_add_pd(a, c));
      _mm256_storeu_pd(out + i + 5, _mm256_add_pd(_mm256_add_pd(b, ta), c));
      c = _mm256_add_pd(c, _mm256_add_pd(ta, tb));
    }
    carry = _mm256_cvtsd_f64(c);
  }
  for (; i + 4 <= n; i += 4) {
    double a0 = x[i], a1 = x[i + 1], a2 = x[i + 2], a3 = x[i + 3];
    if (zero_nan) {
      a0 = a0 == a0 ? a0 : 0.0;
      a1 = a1 == a1 ? a1 : 0.0;
      a2 = a2 == a2 ? a2 : 0.0;
      a3 = a3 == a3 ? a3 : 0.0;
    }
    const double s1 = a0 + a1, s3 = a2 + a3;
    out[i + 1] = carry + a0;
    out[i + 2] = carry + s1;
    out[i + 3] = carry + (s1 + a2);
    carry += s1 + s3;
    out[i + 4] = carry;
  }
  for (; i < n; ++i) {
    double a = x[i];
    if (zero_nan && a != a) a = 0.0;
    carry += a;
    out[i + 1] = carry;
  }
}

// numpy.median of a copy (mean of the two middle values for even sizes). Empty -> NaN.
double median(std::vector<double> v);
double median_inplace(std::span<double> v);
double median_inplace(std::span<float> v);

// Parabolic vertex offset in [-0.5, 0.5] (0 when undefined).
inline double parabolic(double ym1, double y0, double yp1) {
  double den = ym1 - 2 * y0 + yp1;
  double d = 0.5 * (ym1 - yp1) / den;
  if (!std::isfinite(d)) return 0.0;
  return d < -0.5 ? -0.5 : d > 0.5 ? 0.5 : d;
}

// Vectorisable natural log for positive normal floats (max error ~1 ulp of float).
// Branch-free so loops calling it vectorise; no NaN/inf/denormal handling.
inline float fast_logf(float x) {
  // Range reduction x = m * 2^e, m in [sqrt(1/2), sqrt(2)), then a minimax polynomial
  // (Cephes logf) in m - 1.
  unsigned u = std::bit_cast<unsigned>(x);
  int e = static_cast<int>((u >> 23) & 0xFF) - 126;
  float m0 = std::bit_cast<float>((u & 0x007FFFFFu) | 0x3F000000u);  // m in [0.5, 1)
  const bool lo = m0 < 0.70710678f;
  e -= lo;
  float m = (lo ? m0 + m0 : m0) - 1.0f;
  float z = m * m;
  float y = 7.0376836292E-2f;
  y = y * m - 1.1514610310E-1f;
  y = y * m + 1.1676998740E-1f;
  y = y * m - 1.2420140846E-1f;
  y = y * m + 1.4249322787E-1f;
  y = y * m - 1.6668057665E-1f;
  y = y * m + 2.0000714765E-1f;
  y = y * m - 2.4999993993E-1f;
  y = y * m + 3.3333331174E-1f;
  y = y * m * z;
  float fe = static_cast<float>(e);
  y += -2.12194440e-4f * fe;
  y += -0.5f * z;
  return m + y + 0.693359375f * fe;
}

}  // namespace geo::dsp
