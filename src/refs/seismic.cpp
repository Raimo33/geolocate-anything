#include "refs/seismic.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <complex>
#include <cstring>
#include <numbers>
#include <stdexcept>

#if defined(__SSE__) || defined(_M_X64)
#include <xmmintrin.h>
#endif

namespace geo::seismic {

namespace {

using cd = std::complex<double>;
constexpr double PI = std::numbers::pi;

typedef double v4d __attribute__((vector_size(32)));
inline v4d load4(const double* p) {
  v4d v;
  std::memcpy(&v, p, sizeof v);
  return v;
}

double bessel_i0(double x) {
  double s = 1, t = 1;
  for (int k = 1; k < 40; ++k) {
    t *= (x / (2 * k)) * (x / (2 * k));
    s += t;
  }
  return s;
}

// Kaiser-windowed sinc low-pass, odd length, unit DC gain.
std::vector<double> lowpass(double fs, double cutoff, double transition, double atten_db) {
  const double beta = atten_db > 50 ? 0.1102 * (atten_db - 8.7) : 0.5842 * std::pow(atten_db - 21, 0.4) + 0.07886 * (atten_db - 21);
  int n = int(std::ceil((atten_db - 8) / (2.285 * 2 * PI * transition / fs)));
  n |= 1;
  std::vector<double> h(static_cast<std::size_t>(n));
  const int m = n / 2;
  double sum = 0;
  for (int i = 0; i < n; ++i) {
    const double k = i - m, fc = cutoff / fs;
    const double s = k == 0 ? 2 * fc : std::sin(2 * PI * fc * k) / (PI * k);
    const double r = double(k) / m;
    h[std::size_t(i)] = s * bessel_i0(beta * std::sqrt(std::max(0.0, 1 - r * r))) / bessel_i0(beta);
    sum += h[std::size_t(i)];
  }
  for (auto& v : h) v /= sum;
  return h;
}

// Hann-weighted smoothing of the frame length (what the matcher applies to references).
std::vector<double> hann_smooth(const std::vector<float>& v, int w) {
  const std::size_t n = v.size();
  std::vector<double> k(static_cast<std::size_t>(w));
  for (int i = 0; i < w; ++i) k[std::size_t(i)] = std::sin(PI * (i + 1) / (w + 1)) * std::sin(PI * (i + 1) / (w + 1));
  std::vector<double> out(n, NAN);
  const int h = w / 2;
  for (std::size_t i = 0; i < n; ++i) {
    double s = 0, ws = 0, wt = 0;
    for (int j = 0; j < w; ++j) {
      const long t = long(i) + j - h;
      wt += k[std::size_t(j)];
      if (t < 0 || t >= long(n) || !std::isfinite(v[std::size_t(t)])) continue;
      s += k[std::size_t(j)] * v[std::size_t(t)];
      ws += k[std::size_t(j)];
    }
    if (ws > 0.9 * wt) out[i] = s / ws;
  }
  return out;
}

double median(std::vector<double> v) {
  if (v.empty()) return NAN;
  auto m = v.begin() + std::ptrdiff_t(v.size() / 2);
  std::nth_element(v.begin(), m, v.end());
  return *m;
}

double pearson(const std::vector<double>& a, const std::vector<double>& b, std::size_t lo, std::size_t hi, std::size_t* n_out) {
  double sa = 0, sb = 0, saa = 0, sbb = 0, sab = 0;
  std::size_t n = 0;
  for (std::size_t i = lo; i < hi; ++i) {
    if (!std::isfinite(a[i]) || !std::isfinite(b[i])) continue;
    sa += a[i], sb += b[i], saa += a[i] * a[i], sbb += b[i] * b[i], sab += a[i] * b[i];
    ++n;
  }
  if (n_out) *n_out = n;
  if (n < 30) return NAN;
  const double ca = saa - sa * sa / double(n), cb = sbb - sb * sb / double(n), cab = sab - sa * sb / double(n);
  return ca > 0 && cb > 0 ? cab / std::sqrt(ca * cb) : NAN;
}

}  // namespace

Track track_hum(const std::vector<mseed::Segment>& segs, std::int64_t t0, std::int64_t t1, const TrackOptions& o) {
  Track tr;
  tr.t0 = t0;
  const std::size_t n = std::size_t(std::max<std::int64_t>(0, t1 - t0));
  tr.f_mhz.assign(n, NAN);
  tr.snr_db.assign(n, NAN);
  for (const auto& sg : segs) {
    const double fs = sg.rate;
    if (!(fs > 2 * o.base_rate)) continue;
    // The line nearest nominal, folded into [0, fs/2]; sign flips when folded from above.
    double fa = std::fmod(o.nominal_hz, fs);
    double sign = 1;
    if (fa > fs / 2) fa = fs - fa, sign = -1;
    if (fa < 2 || fa > fs / 2 - 2) continue;  // too close to 0 or Nyquist to separate
    const int q = int(std::lround(fs / o.base_rate));
    const double bfs = fs / q;
    static thread_local std::vector<double> h;
    static thread_local double h_fs = 0;
    if (h_fs != fs) h = lowpass(fs, 2.0, 2.0, 80), h_fs = fs;  // flat to +-1 Hz, -80 dB from 3 Hz
    const std::size_t nh = h.size(), mh = nh / 2;
    if (sg.x.size() < nh + 1) continue;
    // Mean removed per segment (the low-pass rejects what is left of the microseism).
    double mean = 0;
    for (float v : sg.x) mean += v;
    mean /= double(sg.x.size());
    // Baseband sample k sits at input index c = mh + k*q (the filter's centre: no delay).
    const std::size_t nb = (sg.x.size() - nh) / std::size_t(q) + 1;
    std::vector<cd> z(nb);
    const double w = 2 * PI * fa / fs;
    {
      // Mixing down and then filtering at the decimated points is one complex FIR on the
      // raw samples: z[k] = e^{-i w k q} sum_j (x[kq + j] - mean) h[j] e^{-i w j}. The
      // rotation uses the absolute index (exact, no drift from a recursion); the mean
      // comes off through the taps' sum.
      const std::size_t nh4 = (nh + 3) / 4 * 4;
      std::vector<double> hr(nh4, 0.0), hi(nh4, 0.0);
      double hsr = 0, hsi = 0;
      for (std::size_t j = 0; j < nh; ++j) {
        const double ph = std::fmod(w * double(j), 2 * PI);
        hr[j] = h[j] * std::cos(ph), hi[j] = -h[j] * std::sin(ph);
        hsr += hr[j], hsi += hi[j];
      }
      constexpr std::size_t BK = 8192;  // outputs per block
      std::vector<double> xd;
      for (std::size_t k0 = 0; k0 < nb; k0 += BK) {
        const std::size_t k1 = std::min(nb, k0 + BK);
        const std::size_t a = k0 * std::size_t(q), b = (k1 - 1) * std::size_t(q) + nh;
        xd.assign(b - a + 4, 0.0);  // zero tail: the last 4-wide load reads past nh
        for (std::size_t i = a; i < b; ++i) xd[i - a] = double(sg.x[i]);
        for (std::size_t k = k0; k < k1; ++k) {
          const double* px = xd.data() + (k - k0) * std::size_t(q);
          v4d ar0 = {}, ar1 = {}, ai0 = {}, ai1 = {};
          std::size_t j = 0;
          for (; j + 8 <= nh4; j += 8) {
            const v4d x0 = load4(px + j), x1 = load4(px + j + 4);
            ar0 += x0 * load4(hr.data() + j), ar1 += x1 * load4(hr.data() + j + 4);
            ai0 += x0 * load4(hi.data() + j), ai1 += x1 * load4(hi.data() + j + 4);
          }
          for (; j < nh4; j += 4) {
            const v4d x0 = load4(px + j);
            ar0 += x0 * load4(hr.data() + j), ai0 += x0 * load4(hi.data() + j);
          }
          const v4d ar = ar0 + ar1, ai = ai0 + ai1;
          const double re = ar[0] + ar[1] + ar[2] + ar[3] - mean * hsr, im = ai[0] + ai[1] + ai[2] + ai[3] - mean * hsi;
          const double ph = std::fmod(w * double(k * std::size_t(q)), 2 * PI);
          z[k] = cd(re, im) * cd(std::cos(ph), -std::sin(ph));
        }
      }
    }
    // Frame and search grid.
    const int L = int(std::lround(o.frame_s * bfs));
    std::vector<double> win(static_cast<std::size_t>(L));
    for (int i = 0; i < L; ++i) win[std::size_t(i)] = std::pow(std::sin(PI * (i + 0.5) / L), 2);
    const double step = 0.005;
    const int ng = int(std::lround(o.search_hz / step));
    // Noise floor: probes 0.6..1.0 Hz either side (in the flat passband, clear of the
    // hum's main lobe and near side lobes wherever in the search band the hum sits).
    std::vector<double> grid, probe;
    for (int g = -ng; g <= ng; ++g) grid.push_back(g * step);
    for (int g = 0; g < 12; ++g) probe.push_back(0.6 + 0.4 * (g + 0.5) / 12), probe.push_back(-probe.back());
    // Every frequency (search grid, then noise probes) in one table, frame-sample major and
    // padded to whole tiles: each frame is then a run of independent multiply-adds, TILE
    // frequencies at a time held in registers.
    constexpr std::size_t TILE = 8;
    const std::size_t G = grid.size(), P = probe.size(), F = G + P, FP = (F + TILE - 1) / TILE * TILE;
    std::vector<double> trr(std::size_t(L) * FP, 0.0), tri(std::size_t(L) * FP, 0.0);
    for (std::size_t g = 0; g < F; ++g) {
      const double fr = g < G ? grid[g] : probe[g - G];
      for (int m = 0; m < L; ++m) {
        const cd e = std::polar(win[std::size_t(m)], -2 * PI * fr * m / bfs);
        trr[std::size_t(m) * FP + g] = e.real(), tri[std::size_t(m) * FP + g] = e.imag();
      }
    }
    std::vector<double> zr(nb), zi(nb);
    for (std::size_t k = 0; k < nb; ++k) zr[k] = z[k].real(), zi[k] = z[k].imag();
    const double tz0 = sg.t0 + double(mh) / fs;  // time of z[0]
    std::vector<double> pw(FP);
    for (std::size_t s = 0; s < n; ++s) {
      const double tc = double(t0 + std::int64_t(s)) + 0.5;
      // frame of L samples centred on tc
      const double kc = (tc - tz0) * bfs;
      const long a = std::lround(kc - 0.5 * (L - 1));
      if (a < 0 || a + L > long(nb)) continue;
      const double* fr_ = zr.data() + a;
      const double* fi_ = zi.data() + a;
      for (std::size_t g0 = 0; g0 < FP; g0 += TILE) {
        // Two 4-wide vectors per part; per frequency the sum still runs m = 0..L-1 in order.
        v4d r0 = {}, r1 = {}, i0 = {}, i1 = {};
        for (int m = 0; m < L; ++m) {
          const double xr = fr_[m], xi = fi_[m];
          const double* cr = trr.data() + std::size_t(m) * FP + g0;
          const double* ci = tri.data() + std::size_t(m) * FP + g0;
          const v4d cr0 = load4(cr), cr1 = load4(cr + 4), ci0 = load4(ci), ci1 = load4(ci + 4);
          r0 += xr * cr0 - xi * ci0, r1 += xr * cr1 - xi * ci1;
          i0 += xr * ci0 + xi * cr0, i1 += xr * ci1 + xi * cr1;
        }
        for (std::size_t u = 0; u < 4; ++u) {
          pw[g0 + u] = r0[u] * r0[u] + i0[u] * i0[u];
          pw[g0 + 4 + u] = r1[u] * r1[u] + i1[u] * i1[u];
        }
      }
      std::size_t best = 0;
      for (std::size_t g = 1; g < G; ++g)
        if (pw[g] > pw[best]) best = g;
      double f = grid[best];
      if (best > 0 && best + 1 < grid.size()) {
        const double la = std::log(pw[best - 1] + 1e-300), lb = std::log(pw[best] + 1e-300), lc = std::log(pw[best + 1] + 1e-300);
        const double den = la - 2 * lb + lc;
        if (den < 0) f += 0.5 * (la - lc) / den * step;
      } else {
        continue;  // peak at the edge of the search: not the hum
      }
      double floor = 0;
      if (o.robust_floor) {
        // Median of the probe powers; noise power is exponential, whose median is ln 2 of its mean.
        std::array<double, 24> pr;
        std::copy(pw.begin() + std::ptrdiff_t(G), pw.begin() + std::ptrdiff_t(F), pr.begin());
        std::nth_element(pr.begin(), pr.begin() + 12, pr.end());
        const double hi = pr[12], lo = *std::max_element(pr.begin(), pr.begin() + 12);
        floor = 0.5 * (lo + hi) / std::numbers::ln2;
      } else {
        for (std::size_t g = G; g < F; ++g) floor += pw[g];
        floor /= double(P);
      }
      tr.f_mhz[s] = float(sign * f * 1000.0 * o.mains_hz / o.nominal_hz);
      tr.snr_db[s] = float(10 * std::log10(pw[best] / (floor + 1e-300)));
    }
  }
  return tr;
}

// ---------------------------------------------------------------- the line on Nyquist

namespace {

// Kaiser-windowed sinc of a given odd length and beta, unit DC gain.
std::vector<double> kaiser_lowpass(int n, double fc_over_fs, double beta) {
  std::vector<double> h(static_cast<std::size_t>(n));
  const int m = n / 2;
  double sum = 0;
  for (int i = 0; i < n; ++i) {
    const double k = i - m;
    const double s = k == 0 ? 2 * fc_over_fs : std::sin(2 * PI * fc_over_fs * k) / (PI * k);
    const double r = k / m;
    h[std::size_t(i)] = s * bessel_i0(beta * std::sqrt(std::max(0.0, 1 - r * r))) / bessel_i0(beta);
    sum += h[std::size_t(i)];
  }
  for (auto& v : h) v /= sum;
  return h;
}

// NaN-aware boxcar mean of width k (centred); NaN where under 80% of the window is there.
std::vector<double> box_mean(const std::vector<double>& x, std::size_t k) {
  const std::size_t n = x.size();
  std::vector<double> cs(n + 1, 0.0), cn(n + 1, 0.0), out(n, NAN);
  for (std::size_t i = 0; i < n; ++i) {
    const bool f = std::isfinite(x[i]);
    cs[i + 1] = cs[i] + (f ? x[i] : 0.0), cn[i + 1] = cn[i] + (f ? 1.0 : 0.0);
  }
  const std::size_t h = k / 2;
  for (std::size_t i = 0; i < n; ++i) {
    const std::size_t lo = i >= h ? i - h : 0, hi = std::min(n, i + (k - h));
    const double c = cn[hi] - cn[lo];
    if (c >= 0.8 * double(k)) out[i] = (cs[hi] - cs[lo]) / c;
  }
  return out;
}

// The decoder's beliefs about far-off states decay below FLT_MIN, and x86 cores handle
// such denormal floats about a hundred times slower: on real Raspberry Shake chunks the
// decode ran 9 to 17 times longer than with them flushed to zero. A state that unlikely
// adds nothing to a sum normalised to 1 (on five stations' 6 h chunks: the same seconds
// kept, values within 0.2 mHz, against ~10 mHz of decoder noise), so the decode runs with
// flush-to-zero and denormals-are-zero set, and the thread's previous mode is restored after.
struct FlushDenormals {
#if defined(__SSE__) || defined(_M_X64)
  unsigned saved = _mm_getcsr();
  FlushDenormals() { _mm_setcsr(saved | 0x8040u); }  // FTZ (bit 15) | DAZ (bit 6)
  ~FlushDenormals() { _mm_setcsr(saved); }
#else
  FlushDenormals() = default;
#endif
  FlushDenormals(const FlushDenormals&) = delete;
  FlushDenormals& operator=(const FlushDenormals&) = delete;
};

constexpr int WIDE_RATE = 4;    // baseband kept to +-1 Hz for the noise estimate
constexpr int DEC_RATE = 1;     // decoder steps per second
constexpr int STAT_WIN_S = 300; // line and noise are measured per 5 minutes
constexpr double NOISE_BW = 0.30;

// exp(-z^2/2) by table: the decoder evaluates it for every state at every step.
struct GaussTable {
  static constexpr int N = 2048;
  static constexpr float ZMAX = 8.f;
  float v[N + 1];
  GaussTable() {
    for (int i = 0; i <= N; ++i) {
      const double z = double(i) * ZMAX / N;
      v[i] = float(std::exp(-0.5 * z * z));
    }
    for (auto& x : v) x = std::max(x, 1e-12f);  // a wild sample flattens the belief instead of zeroing it
  }
  float operator()(float z) const {  // branch-free, so the loop over states vectorises
    const float a = std::min(std::fabs(z), ZMAX);
    return v[int(a * (N / ZMAX) + 0.5f)];
  }
};

struct Decoder {
  int nd, nt;
  std::vector<float> dl, templ;       // deviation per row; expected beat per state (nd x nt)
  std::vector<int> s0;
  std::vector<float> fr;
  float q0 = 0.25f, q1 = 0.5f, q2 = 0.25f;
  GaussTable gauss;

  Decoder(const NyquistOptions& o, double kappa) {
    nd = int(std::lround(2 * o.dmax_mhz / o.dstep_mhz)) + 1, nt = o.n_theta;
    dl.resize(std::size_t(nd)), templ.resize(std::size_t(nd) * std::size_t(nt)), s0.resize(std::size_t(nd)), fr.resize(std::size_t(nd));
    for (int d = 0; d < nd; ++d) {
      dl[std::size_t(d)] = float(-o.dmax_mhz + d * o.dstep_mhz);
      const double g = std::exp(-kappa * dl[std::size_t(d)]);
      for (int t = 0; t < nt; ++t) templ[std::size_t(d * nt + t)] = float(g * std::cos((t + 0.5) * 2 * PI / nt));
      const double shift = dl[std::size_t(d)] * 1e-3 / DEC_RATE * nt;  // bins the phase advances per step
      s0[std::size_t(d)] = int(std::floor(shift)), fr[std::size_t(d)] = float(shift - std::floor(shift));
      if (s0[std::size_t(d)] < -PADT || s0[std::size_t(d)] > PADT - 1 || nt > 256) throw std::invalid_argument("track_nyquist: deviation range too wide for the phase grid");
    }
  }
  std::size_t states() const { return std::size_t(nd) * std::size_t(nt); }
  void diffuse(const float* p, float* out, float a, float c) const {  // a: weight from the row below, c: from above
    const std::size_t T = std::size_t(nt);
    for (int d = 0; d < nd; ++d) {
      const float* lo = p + std::size_t(d > 0 ? d - 1 : 0) * T;
      const float* hi = p + std::size_t(d + 1 < nd ? d + 1 : nd - 1) * T;
      const float* me = p + std::size_t(d) * T;
      float* o = out + std::size_t(d) * T;
      for (std::size_t t = 0; t < T; ++t) o[t] = q1 * me[t] + a * lo[t] + c * hi[t];
    }
  }
  // A row with PADT wrapped samples either side, so a turn of a few bins needs no modulo.
  static constexpr int PADT = 4;
  void pad_row(const float* row, float* ext) const {
    for (int i = 0; i < PADT; ++i) ext[i] = row[nt - PADT + i], ext[nt + PADT + i] = row[i];
    std::memcpy(ext + PADT, row, std::size_t(nt) * sizeof(float));
  }
  // One step forward: the phase turns by each row's deviation, then the deviation diffuses.
  void advance(const float* p, float* tmp, float* out) const {
    float ext[256 + 2 * PADT];
    for (int d = 0; d < nd; ++d) {
      const float f = fr[std::size_t(d)];
      pad_row(p + std::size_t(d * nt), ext);
      const float* e = ext + PADT - s0[std::size_t(d)];  // e[t] = row[t - s0]
      float* o = tmp + std::size_t(d * nt);
      for (int t = 0; t < nt; ++t) o[t] = (1 - f) * e[t] + f * e[t - 1];
    }
    diffuse(tmp, out, q0, q2);
  }
  // Its adjoint, for the backward pass.
  void retreat(const float* p, float* tmp, float* out) const {
    diffuse(p, tmp, q2, q0);
    float ext[256 + 2 * PADT];
    for (int d = 0; d < nd; ++d) {
      const float f = fr[std::size_t(d)];
      pad_row(tmp + std::size_t(d * nt), ext);
      const float* e = ext + PADT + s0[std::size_t(d)];  // e[t] = row[t + s0]
      float* o = out + std::size_t(d * nt);
      for (int t = 0; t < nt; ++t) o[t] = (1 - f) * e[t] + f * e[t + 1];
    }
  }
  void weigh(float* p, float y, float inv_s) const {
    const std::size_t S = states();
    for (std::size_t i = 0; i < S; ++i) p[i] *= gauss((y - templ[i]) * inv_s);
  }
  static double normalise(float* p, std::size_t S) {
    float sum = 0;
    for (std::size_t i = 0; i < S; ++i) sum += p[i];
    const float inv = 1.f / (sum + 1e-30f);
    for (std::size_t i = 0; i < S; ++i) p[i] *= inv;
    return sum;
  }

  // b, sig: the beat and its noise sd per step (NaN = missing). Fills the posterior mean
  // and sd of the deviation per step; returns the log evidence summed over [lo, hi).
  // Without `posterior` (only the evidence is wanted) the last backward pass is skipped.
  double run(const std::vector<double>& b, const std::vector<double>& sig, const NyquistOptions& o, std::size_t lo, std::size_t hi,
             std::vector<double>& md, std::vector<double>& sd, bool posterior = true) const {
    const std::size_t n = b.size(), S = states(), k = std::size_t(o.amp_win_s) * DEC_RATE;
    std::vector<char> ok(n);
    std::vector<double> b2(n), s2(n);
    for (std::size_t i = 0; i < n; ++i) {
      ok[i] = std::isfinite(b[i]) && std::isfinite(sig[i]);
      b2[i] = ok[i] ? b[i] * b[i] : NAN, s2[i] = ok[i] ? sig[i] * sig[i] : NAN;
    }
    // Amplitude to start from: the beat's power less the noise's, over amp_win_s.
    std::vector<double> a(n);
    {
      auto pb = box_mean(b2, k), pn = box_mean(s2, k);
      std::vector<double> fin;
      for (std::size_t i = 0; i < n; ++i) {
        a[i] = std::sqrt(2 * std::max(pb[i] - pn[i], 1e-30));
        if (std::isfinite(a[i]) && a[i] > 0) fin.push_back(a[i]);
      }
      const double med = fin.empty() ? 1.0 : median(fin);
      for (auto& v : a)
        if (!(std::isfinite(v) && v > 0)) v = med;
    }
    std::vector<float> prior(S);
    for (int d = 0; d < nd; ++d)
      for (int t = 0; t < nt; ++t) prior[std::size_t(d * nt + t)] = float(std::exp(-0.5 * (dl[std::size_t(d)] / 25.0) * (dl[std::size_t(d)] / 25.0)));
    normalise(prior.data(), S);
    md.assign(n, NAN), sd.assign(n, NAN);
    std::vector<float> F(n * S), p(S), tmp(S), nxt(S), beta(S);
    std::vector<double> mc(n), ev(n);
    for (int it = 0; it < 2; ++it) {
      p = prior;
      for (std::size_t t = 0; t < n; ++t) {
        if (t) advance(p.data(), tmp.data(), nxt.data()), p.swap(nxt);
        if (ok[t]) weigh(p.data(), float(b[t] / a[t]), float(1.0 / std::max(sig[t] / a[t], o.sigma_floor)));
        ev[t] = std::log(normalise(p.data(), S) + 1e-300);
        std::memcpy(F.data() + t * S, p.data(), S * sizeof(float));
      }
      if (it == 1 && !posterior) break;  // the evidence is the forward pass's
      std::fill(beta.begin(), beta.end(), 1.f);
      for (std::size_t t = n; t-- > 0;) {
        const float* f = F.data() + t * S;
        double sum = 0, m1 = 0, m2 = 0, c = 0;
        for (int d = 0; d < nd; ++d) {
          const float* fd = f + std::size_t(d * nt);
          const float* bd = beta.data() + std::size_t(d * nt);
          const float* td = templ.data() + std::size_t(d * nt);
          float pd = 0, cd_ = 0;
          for (int th = 0; th < nt; ++th) {
            const float v = fd[th] * bd[th];
            pd += v, cd_ += v * td[th];
          }
          sum += pd, c += cd_, m1 += double(pd) * dl[std::size_t(d)], m2 += double(pd) * dl[std::size_t(d)] * dl[std::size_t(d)];
        }
        const double inv = 1.0 / (sum + 1e-300);
        m1 *= inv, m2 *= inv;
        md[t] = m1, sd[t] = std::sqrt(std::max(m2 - m1 * m1, 0.0)), mc[t] = c * inv;
        if (ok[t]) weigh(beta.data(), float(b[t] / a[t]), float(1.0 / std::max(sig[t] / a[t], o.sigma_floor)));
        retreat(beta.data(), tmp.data(), nxt.data()), beta.swap(nxt);
        normalise(beta.data(), S);
      }
      if (it == 0) {  // amplitude again, now against the decoded beat
        std::vector<double> num(n), den(n);
        for (std::size_t i = 0; i < n; ++i) num[i] = ok[i] ? b[i] * mc[i] : NAN, den[i] = ok[i] ? mc[i] * mc[i] : NAN;
        auto sn = box_mean(num, k), sdn = box_mean(den, k);
        for (std::size_t i = 0; i < n; ++i) {
          const double v = sn[i] / std::max(sdn[i], 1e-6);
          if (std::isfinite(v) && v > 0) a[i] = v;
        }
      }
    }
    double total = 0;
    for (std::size_t t = lo; t < std::min(hi, n); ++t) total += ev[t];
    for (std::size_t t = 0; t < n; ++t)
      if (!ok[t]) md[t] = sd[t] = NAN;
    return total;
  }
};

}  // namespace

Track track_nyquist(const std::vector<mseed::Segment>& segs, std::int64_t t0, std::int64_t t1, const NyquistOptions& o) {
  Track tr;
  tr.t0 = t0;
  const std::size_t n = std::size_t(std::max<std::int64_t>(0, t1 - t0));
  tr.f_mhz.assign(n, NAN);
  tr.snr_db.assign(n, NAN);
  if (!n) return tr;
  const std::int64_t ta = t0 - NYQUIST_PAD_S, tb = t1 + NYQUIST_PAD_S;
  const std::size_t ns = std::size_t(tb - ta);
  // 1. The line mixed to 0 Hz ((-1)^k on the absolute sample count), low-passed to +-1 Hz,
  //    on a UTC grid of WIDE_RATE samples a second.
  std::vector<double> w(ns * WIDE_RATE, NAN);
  static const std::vector<double> h = lowpass(2 * 50.0, 1.0, 1.0, 70);
  const std::size_t nh = h.size(), mh = nh / 2;
  for (const auto& sg : segs) {
    const double fs = sg.rate;
    if (std::fabs(fs - 2 * o.mains_hz) > 1e-6 || sg.x.size() < nh + std::size_t(fs) * 30) continue;
    double mean = 0;
    for (float v : sg.x) mean += v;
    mean /= double(sg.x.size());
    const long long k0 = std::llround(sg.t0 * fs);
    std::vector<double> xs(sg.x.size());
    for (std::size_t i = 0; i < xs.size(); ++i) xs[i] = ((k0 + (long long)i) & 1) ? mean - sg.x[i] : sg.x[i] - mean;
    const long j_lo = std::max<long>(0, long(std::ceil((sg.t0 + double(mh) / fs - double(ta)) * WIDE_RATE)));
    for (long j = j_lo; j < long(w.size()); ++j) {
      const long c = std::lround((double(ta) + double(j) / WIDE_RATE - sg.t0) * fs);
      if (c < long(mh)) continue;
      if (c + long(mh) >= long(xs.size())) break;
      const double* px = xs.data() + (c - long(mh));
      double acc = 0;
      for (std::size_t i = 0; i < nh; ++i) acc += px[i] * h[i];
      w[std::size_t(j)] = acc;
    }
  }
  // 2. Per 5 minutes: the line's power (within 0.12 Hz) and the noise density beside it
  //    (0.14..0.24 Hz), by Welch periodograms of 64 s.
  const std::size_t nwin = ns / STAT_WIN_S;
  std::vector<double> line_db(nwin, NAN), noise_sd(nwin, NAN);
  {
    constexpr int L = 64 * WIDE_RATE, HOP = L / 2, NB = 16;
    std::vector<double> win(L), cr(std::size_t(NB) * L), ci(std::size_t(NB) * L);
    double wss = 0;
    for (int i = 0; i < L; ++i) win[std::size_t(i)] = 0.5 - 0.5 * std::cos(2 * PI * i / L), wss += win[std::size_t(i)] * win[std::size_t(i)];
    for (int kb = 0; kb < NB; ++kb)
      for (int i = 0; i < L; ++i) cr[std::size_t(kb * L + i)] = win[std::size_t(i)] * std::cos(2 * PI * kb * i / L), ci[std::size_t(kb * L + i)] = -win[std::size_t(i)] * std::sin(2 * PI * kb * i / L);
    const double df = double(WIDE_RATE) / L;
    for (std::size_t wi = 0; wi < nwin; ++wi) {
      const std::size_t a = wi * STAT_WIN_S * WIDE_RATE, len = std::size_t(STAT_WIN_S) * WIDE_RATE;
      std::size_t fin = 0;
      for (std::size_t i = a; i < a + len; ++i) fin += std::isfinite(w[i]);
      if (double(fin) < 0.9 * double(len)) continue;
      std::vector<double> S(NB, 0.0), seg(L);
      int cnt = 0;
      for (std::size_t s = a; s + L <= a + len; s += HOP, ++cnt) {
        double m = 0;
        for (int i = 0; i < L; ++i) seg[std::size_t(i)] = std::isfinite(w[s + std::size_t(i)]) ? w[s + std::size_t(i)] : 0.0, m += seg[std::size_t(i)];
        m /= L;  // scipy's welch removes each segment's mean
        for (int kb = 0; kb < NB; ++kb) {
          double re = 0, im = 0;
          for (int i = 0; i < L; ++i) re += (seg[std::size_t(i)] - m) * cr[std::size_t(kb * L + i)], im += (seg[std::size_t(i)] - m) * ci[std::size_t(kb * L + i)];
          S[std::size_t(kb)] += (kb ? 2.0 : 1.0) * (re * re + im * im) / (WIDE_RATE * wss);
        }
      }
      if (!cnt) continue;
      for (auto& v : S) v /= cnt;
      std::vector<double> nb;
      for (int kb = 0; kb < NB; ++kb)
        if (kb * df >= 0.14 && kb * df <= 0.24) nb.push_back(S[std::size_t(kb)]);
      std::sort(nb.begin(), nb.end());
      const double n0 = nb.size() % 2 ? nb[nb.size() / 2] : 0.5 * (nb[nb.size() / 2 - 1] + nb[nb.size() / 2]);
      double pl = 0;
      for (int kb = 0; kb < NB; ++kb)
        if (kb * df < 0.12) pl += (S[std::size_t(kb)] - n0) * df;
      line_db[wi] = 10 * std::log10(std::max(std::max(pl, 0.0) / std::max(n0 * NOISE_BW, 1e-30), 1e-3));
      noise_sd[wi] = std::sqrt(n0 * NOISE_BW);
    }
  }
  // 2b. Where the grid has left the decoder's range. Beyond +-dmax the beat is faster than
  //     any state can follow, and the decoder then returns values near zero (at every
  //     station alike, so agreement does not catch it): 2023-02-15 in Western Australia,
  //     +130 mHz by a direct line, 0 by three Shakes. Per 64 s (hop 32 s): if the strongest
  //     bin beyond three quarters of the range, out to 0.5 Hz, is a line (12 dB over the
  //     median there) and stands above the strongest bin inside, those seconds and the
  //     half minute either side are not kept (the way out and back is lost with them: the
  //     decoder is already wrong at 75 mHz on its way to 130).
  std::vector<char> beyond(ns, 0);
  {
    constexpr int L = 64 * WIDE_RATE, HOP = L / 2, NB = 33;
    const double df = double(WIDE_RATE) / L;
    std::vector<double> cr(std::size_t(NB) * L), ci(std::size_t(NB) * L);
    for (int kb = 0; kb < NB; ++kb)
      for (int i = 0; i < L; ++i) {
        const double win = 0.5 - 0.5 * std::cos(2 * PI * i / L);
        cr[std::size_t(kb * L + i)] = win * std::cos(2 * PI * kb * i / L), ci[std::size_t(kb * L + i)] = -win * std::sin(2 * PI * kb * i / L);
      }
    const int k_in = int(std::floor((0.75 * o.dmax_mhz * 1e-3 + 0.5 * df) / df));  // last bin counted as inside
    std::vector<double> S(NB), outer;
    for (std::size_t a = 0; a + L <= w.size(); a += HOP) {
      bool good = true;
      for (int i = 0; i < L && good; ++i) good = std::isfinite(w[a + std::size_t(i)]);
      if (!good) continue;
      for (int kb = 0; kb < NB; ++kb) {
        double re = 0, im = 0;
        for (int i = 0; i < L; ++i) re += w[a + std::size_t(i)] * cr[std::size_t(kb * L + i)], im += w[a + std::size_t(i)] * ci[std::size_t(kb * L + i)];
        S[std::size_t(kb)] = re * re + im * im;
      }
      const double in_max = *std::max_element(S.begin(), S.begin() + k_in + 1);
      outer.assign(S.begin() + k_in + 1, S.end());
      const double out_max = *std::max_element(outer.begin(), outer.end());
      std::nth_element(outer.begin(), outer.begin() + long(outer.size() / 2), outer.end());
      if (out_max > in_max && out_max > 16 * outer[outer.size() / 2])
        for (std::size_t sec = a / WIDE_RATE - std::min<std::size_t>(a / WIDE_RATE, 32); sec < std::min(ns, (a + L) / WIDE_RATE + 32); ++sec) beyond[sec] = 1;
    }
  }
  // 3. The beat itself: within 0.15 Hz, DEC_RATE samples a second.
  static const std::vector<double> hn = kaiser_lowpass(WIDE_RATE * 24 + 1, 0.15 / WIDE_RATE, 7.0);
  const std::size_t nn = hn.size(), mn = nn / 2, nbt = ns * DEC_RATE;
  std::vector<double> b(nbt, NAN), sig(nbt, NAN);
  for (std::size_t m = 0; m < nbt; ++m) {
    const std::size_t c = m * (WIDE_RATE / DEC_RATE);
    if (c < mn || c + mn >= w.size()) continue;
    double acc = 0;
    bool good = true;
    for (std::size_t i = 0; i < nn && good; ++i) {
      const double v = w[c - mn + i];
      good = std::isfinite(v);
      acc += v * hn[i];
    }
    if (!good) continue;
    b[m] = acc;
    const std::size_t wi = m / (std::size_t(STAT_WIN_S) * DEC_RATE);
    if (wi < nwin) sig[m] = noise_sd[wi];
  }
  // 4. Decode two hours at a time, each block with 10 minutes of lead-in and lead-out, once with
  //    the roll-off and once without: a block is kept where the roll-off earns its place.
  const Decoder with(o, o.kappa), without(o, 0.0);
  const FlushDenormals ftz;
  constexpr std::int64_t BLOCK_S = 2 * 3600;
  for (std::int64_t c0 = t0; c0 < t1; c0 += BLOCK_S) {
    const std::int64_t c1 = std::min(t1, c0 + BLOCK_S);
    const std::size_t lo = std::size_t(c0 - NYQUIST_PAD_S - ta) * DEC_RATE, hi = std::size_t(c1 + NYQUIST_PAD_S - ta) * DEC_RATE;
    std::size_t fin = 0;
    for (std::size_t i = lo; i < hi; ++i) fin += std::isfinite(b[i]) && std::isfinite(sig[i]);
    if (fin < std::size_t(600) * DEC_RATE) continue;
    // Only seconds whose 5 minutes carry the line are kept: without one, the block's
    // decode (the costly part) would be thrown away.
    bool line = false;
    for (std::int64_t s = c0; s < c1 && !line; s += STAT_WIN_S) {
      const std::size_t wi = std::size_t(s - ta) / STAT_WIN_S;
      line = wi < nwin && line_db[wi] >= o.min_line_db;
    }
    if (!line) continue;
    const std::vector<double> bb(b.begin() + long(lo), b.begin() + long(hi)), ss(sig.begin() + long(lo), sig.begin() + long(hi));
    const std::size_t klo = std::size_t(NYQUIST_PAD_S) * DEC_RATE, khi = klo + std::size_t(c1 - c0) * DEC_RATE;
    std::vector<double> md, sd, md0, sd0;
    const double e1 = with.run(bb, ss, o, klo, khi, md, sd), e0 = without.run(bb, ss, o, klo, khi, md0, sd0, false);
    if (e1 - e0 < o.min_evidence * double(c1 - c0) / 3600.0) continue;
    for (std::int64_t s = c0; s < c1; ++s) {
      const std::size_t wi = std::size_t(s - ta) / STAT_WIN_S;
      if (wi >= nwin || !(line_db[wi] >= o.min_line_db) || beyond[std::size_t(s - ta)]) continue;
      double acc = 0;
      int cnt = 0;
      for (int u = 0; u < DEC_RATE; ++u) {
        const double v = md[klo + std::size_t(s - c0) * DEC_RATE + std::size_t(u)];
        if (std::isfinite(v)) acc += v, ++cnt;
      }
      if (!cnt) continue;
      tr.f_mhz[std::size_t(s - t0)] = float(acc / cnt);
      tr.snr_db[std::size_t(s - t0)] = float(std::min(line_db[wi], o.max_snr_db));
    }
  }
  return tr;
}

std::vector<float> fuse(const std::vector<Track>& tracks, const std::vector<std::string>& ids, FuseStats* stats, const FuseOptions& o) {
  const std::size_t S = tracks.size();
  const std::size_t n = S ? tracks[0].f_mhz.size() : 0;
  std::vector<float> out(n, NAN);
  if (stats) {
    stats->ids = ids;
    stats->used_s.assign(S, 0);
    stats->multi_s = stats->single_s = 0;
    stats->n_used.assign(n, 0);
  }
  if (!S) return out;
  // Values that pass the per-second gates.
  std::vector<std::vector<float>> v(S);
  for (std::size_t s = 0; s < S; ++s) {
    v[s] = tracks[s].f_mhz;
    for (std::size_t i = 0; i < n; ++i)
      if (!(tracks[s].snr_db[i] >= o.min_snr_db) || !(std::fabs(v[s][i]) <= o.max_abs_mhz)) v[s][i] = NAN;
  }
  std::vector<std::vector<double>> sm(S);
  for (std::size_t s = 0; s < S; ++s) sm[s] = hann_smooth(v[s], 12);
  const bool mixed = !o.nyquist.empty();
  auto nyq = [&](std::size_t s) { return mixed && s < o.nyquist.size() && o.nyquist[s]; };
  auto sd_of = [](const std::vector<double>& x, std::size_t lo, std::size_t hi) {
    double s1 = 0, s2 = 0;
    std::size_t k = 0;
    for (std::size_t i = lo; i < hi; ++i)
      if (std::isfinite(x[i])) s1 += x[i], s2 += x[i] * x[i], ++k;
    return k > 1 ? std::sqrt(std::max(0.0, s2 / double(k) - (s1 / double(k)) * (s1 / double(k)))) : NAN;
  };
  const std::size_t W = std::size_t(o.window_s), hop = W / 2;
  // Per window (hop W/2, centre half used) the accepted stations and their weights.
  std::vector<std::vector<double>> wt(S, std::vector<double>(n, 0.0));
  for (std::size_t c = 0; c < n; c += hop) {
    const std::size_t lo = c >= hop / 2 ? c - hop / 2 : 0, hi = std::min(n, c + hop + hop / 2);  // stats window ~W
    const std::size_t ulo = c, uhi = std::min(n, c + hop);                                     // seconds it decides
    // A station's noise from its SNR (a tone's frequency error falls as 1/sqrt(SNR)).
    std::vector<double> noise(S, NAN);
    for (std::size_t s = 0; s < S; ++s) {
      std::vector<double> lin;
      for (std::size_t i = lo; i < hi; ++i)
        if (std::isfinite(v[s][i])) lin.push_back(std::pow(10.0, tracks[s].snr_db[i] / 10.0));
      if (lin.size() >= (hi - lo) / 3) noise[s] = o.noise_c_mhz / std::sqrt(median(lin));
    }
    // agreement graph on the smoothed tracks
    std::vector<int> agree(S, 0);
    std::vector<char> live(S, 0), mirrored(S, 0);
    std::vector<std::vector<char>> pair_ok(S, std::vector<char>(S, 0));
    for (std::size_t s = 0; s < S; ++s) live[s] = std::isfinite(noise[s]);
    int n_live = 0;
    for (std::size_t s = 0; s < S; ++s) n_live += live[s];
    for (std::size_t a = 0; a < S; ++a)
      for (std::size_t b = a + 1; b < S; ++b) {
        if (!live[a] || !live[b]) continue;
        std::size_t k = 0;
        const double r = pearson(sm[a], sm[b], lo, hi, &k);
        if (k < (hi - lo) / 3) continue;
        const bool na = nyq(a), nb = nyq(b);
        const double thr = mixed && (na || nb) ? o.agree_r_nyquist : o.agree_r;
        if (r >= thr) ++agree[a], ++agree[b], pair_ok[a][b] = pair_ok[b][a] = 1;
        else if (mixed && na != nb && r <= -thr) {  // a Shake mirroring a direct station
          ++agree[na ? b : a];
          mirrored[na ? a : b] = 1;
        }
      }
    // Where a direct station is confirmed, the direct stations decide.
    int max_direct = 0;
    for (std::size_t s = 0; s < S; ++s)
      if (live[s] && !nyq(s)) max_direct = std::max(max_direct, agree[s]);
    std::vector<char> anchor(S, 0);
    if (mixed && max_direct > 0)
      for (std::size_t s = 0; s < S; ++s) anchor[s] = live[s] && !nyq(s) && agree[s] > 0 && agree[s] * 2 >= max_direct;
    int max_agree = 0;
    for (std::size_t s = 0; s < S; ++s) max_agree = std::max(max_agree, agree[s]);
    // A lone track: the quietest of those its kind lets stand alone, if it wanders as a grid does.
    std::size_t lone = S;
    if (max_agree == 0)
      for (std::size_t s = 0; s < S; ++s) {
        if (!live[s] || !(noise[s] <= (nyq(s) ? o.lone_max_noise_nyquist_mhz : o.lone_max_noise_mhz))) continue;
        if (o.lone_min_sd_mhz > 0 && !(sd_of(sm[s], lo, hi) >= o.lone_min_sd_mhz)) continue;
        if (lone == S || noise[s] < noise[lone]) lone = s;
      }
    for (std::size_t s = 0; s < S; ++s) {
      if (!live[s]) continue;
      bool ok;
      if (max_direct > 0 && mixed) {
        ok = anchor[s];
        if (nyq(s) && !mirrored[s])
          for (std::size_t d = 0; d < S && !ok; ++d) ok = anchor[d] && pair_ok[s][d];
      } else if (max_agree > 0) ok = agree[s] > 0 && agree[s] * 2 >= max_agree && !mirrored[s];  // in the agreeing group
      else if (mixed || o.lone_min_sd_mhz > 0) ok = s == lone;
      else ok = noise[s] <= o.lone_max_noise_mhz && (n_live == 1 || noise[s] == *std::min_element(noise.begin(), noise.end(), [](double x, double y) {
                                                                   return !std::isfinite(y) || (std::isfinite(x) && x < y);
                                                                 }));
      if (!ok) continue;
      const double w = 1.0 / std::max(noise[s] * noise[s], 0.04);
      for (std::size_t i = ulo; i < uhi; ++i) wt[s][i] = w;
    }
  }
  for (std::size_t i = 0; i < n; ++i) {
    // provisional: weighted median of the accepted values
    std::vector<std::pair<double, double>> xs;
    for (std::size_t s = 0; s < S; ++s)
      if (wt[s][i] > 0 && std::isfinite(v[s][i])) xs.push_back({v[s][i], wt[s][i]});
    if (xs.empty()) continue;
    std::sort(xs.begin(), xs.end());
    double tot = 0, acc = 0, med = xs[0].first;
    for (auto& [x, w] : xs) tot += w;
    for (auto& [x, w] : xs)
      if ((acc += w) >= tot / 2) {
        med = x;
        break;
      }
    double num = 0, den = 0;
    int used = 0;
    for (std::size_t s = 0; s < S; ++s) {
      if (!(wt[s][i] > 0) || !std::isfinite(v[s][i])) continue;
      const double tol = std::max(5.0, 4.0 / std::sqrt(wt[s][i]));
      if (xs.size() > 1 && std::fabs(v[s][i] - med) > tol) continue;
      num += wt[s][i] * v[s][i];
      den += wt[s][i];
      ++used;
      if (stats) ++stats->used_s[s];
    }
    if (den <= 0) continue;
    out[i] = float(num / den);
    if (stats) (used >= 2 ? stats->multi_s : stats->single_s)++, stats->n_used[i] = std::uint8_t(std::min(used, 255));
  }
  return out;
}

}  // namespace geo::seismic
