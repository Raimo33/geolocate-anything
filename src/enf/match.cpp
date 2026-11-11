#include "enf/match.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <immintrin.h>
#include <cmath>
#include <map>
#include <mutex>
#include <numeric>
#include <stdexcept>
#include <thread>
#include <type_traits>

#include "dsp/fft.hpp"
#include "dsp/filters.hpp"
#include "util/files.hpp"
#include "util/parallel.hpp"

namespace geo::match {

using fft::cd;
using fft::cf;

std::string pyf(double x, const char* spec) {
  if (std::isnan(x)) return "nan";
  if (std::isinf(x)) return x > 0 ? "inf" : "-inf";
  char b[64];
  std::snprintf(b, sizeof b, spec, x);
  return b;
}

int search_threads(int requested) { return worker_threads(requested); }

int hp_window_for(int m, int hp_window_s) {
  static constexpr int W[] = {15, 31, 61, 121};
  int best = -1;
  for (int w : W)
    if (w <= std::min(hp_window_s, m / 2)) best = w;
  return best > 0 ? best : std::max(3, (m / 2) | 1);
}

// ---------------------------------------------------------------- query + kernels

namespace {

int choose_nfft(int m) {
  int lo = 256;
  while (lo < 2 * m) lo <<= 1;
  int best = lo;
  double best_cost = INFINITY;
  for (int N = lo; N <= lo * 32 && N <= (1 << 22); N <<= 1) {
    double cost = double(N) * (std::log2(double(N)) + 2.0) / double(N - m + 1);
    if (cost < best_cost * 0.995) {
      best_cost = cost;
      best = N;
    }
  }
  return best;
}

class Query {
 public:
  Query(std::span<const double> q, double min_overlap) : m(static_cast<int>(q.size())), min_overlap(min_overlap) {
    w.resize(q.size());
    qv.resize(q.size());
    for (std::size_t i = 0; i < q.size(); ++i) {
      bool f = std::isfinite(q[i]);
      w[i] = f;
      qv[i] = f ? q[i] : 0.0;
      nq += f;
    }
    if (nq < 10) throw std::invalid_argument("query has too few valid samples");
    full = nq == double(m);
    if (full) {
      double mean = std::accumulate(qv.begin(), qv.end(), 0.0) / m;
      for (auto& v : qv) v -= mean;
    }
    for (double v : qv) {
      vq_full += v * v;
      sum_q += v;
    }
    N = choose_nfft(m);
    qf.assign(qv.begin(), qv.end());
    // The fast path's query: centred over its valid samples, zero where missing. Against
    // a gap-free window, sum(qc * y) is then the covariance term of Pearson r exactly
    // (sum(qc) = 0), and the window's sums over the valid samples come from prefix sums
    // over the valid runs. A full query is one run and qc = qv.
    qc.assign(qv.begin(), qv.end());
    if (!full) {
      const double mean = sum_q / nq;
      for (std::size_t i = 0; i < qc.size(); ++i) qc[i] = w[i] ? qv[i] - mean : 0.0;
    }
    vq_c = 0;
    for (double v : qc) vq_c += v * v;
    if (full) vq_c = vq_full;
    qcf.assign(qc.begin(), qc.end());
    wf.assign(w.begin(), w.end());
    for (int j = 0; j < m;) {
      if (!w[std::size_t(j)]) {
        ++j;
        continue;
      }
      int e = j;
      while (e < m && w[std::size_t(e)]) ++e;
      runs.push_back({j, e});
      j = e;
    }
  }

  // Reversed, zero-padded spectrum of qc (float), pre-scaled by 1/N.
  const fft::Buf<cf>& fast_spec() {
    if (!Fq_.size()) {
      auto& P = fft::RealPlanF::get(N);
      fft::Buf<float> in{std::size_t(N)};
      std::fill(in.begin(), in.end(), 0.0f);
      for (int j = 0; j < m; ++j) in[std::size_t(m - 1 - j)] = static_cast<float>(qc[std::size_t(j)]);
      Fq_ = fft::Buf<cf>(std::size_t(N / 2 + 1));
      P.forward(in.data(), Fq_.data());
      const float s = 1.0f / float(N);
      for (auto& c : Fq_) c *= s;
    }
    return Fq_;
  }
  struct MaskSpec {
    fft::Buf<cd> Fw, Fq, Fq2;
  };
  const MaskSpec& mask_spec() {
    if (!ms_.Fw.size()) {
      auto& P = fft::RealPlanD::get(N);
      fft::Buf<double> in{std::size_t(N)};
      auto make = [&](auto f) {
        std::fill(in.begin(), in.end(), 0.0);
        for (int j = 0; j < m; ++j) in[std::size_t(m - 1 - j)] = f(std::size_t(j));
        fft::Buf<cd> out{std::size_t(N / 2 + 1)};
        P.forward(in.data(), out.data());
        for (auto& c : out) c /= double(N);
        return out;
      };
      ms_.Fw = make([&](std::size_t j) { return w[j]; });
      ms_.Fq = make([&](std::size_t j) { return qv[j]; });
      ms_.Fq2 = make([&](std::size_t j) { return qv[j] * qv[j]; });
    }
    return ms_;
  }

  int m;
  double min_overlap;
  std::vector<double> w, qv;
  std::vector<float> qf;  // qv in single precision
  std::vector<double> qc;  // centred over the valid samples, 0 where missing (= qv when full)
  std::vector<float> qcf, wf;  // qc and the validity mask in single precision
  std::vector<std::pair<int, int>> runs;  // valid samples [a, b)
  double nq = 0, vq_full = 0, sum_q = 0, vq_c = 0;
  bool full = false;
  int N = 0;

 private:
  fft::Buf<cf> Fq_;
  MaskSpec ms_;
};

// Pearson r for lags [0, r.size()) of y (y covers r.size() + m - 1 samples, none
// missing). Overlap-save: each FFT of size N yields N - m + 1 lags of qc * y; window
// sums of y and y^2 over the query's valid samples come from chunk-local prefix sums
// (double), one difference per valid run, so the per-lag loops vectorise.
template <class T>
void fast_kernel(Query& Q, std::span<const T> y, std::span<float> r) {
  const int m = Q.m, N = Q.N, L = N - m + 1;
  const std::size_t n_out = r.size();
  auto& P = fft::RealPlanF::get(N);
  const auto& Fq = Q.fast_spec();
  static thread_local fft::Buf<float> in, out;
  static thread_local fft::Buf<cf> spec;
  static thread_local std::vector<double> p1, p2, s1, s2;
  if (in.size() != std::size_t(N)) {
    in = fft::Buf<float>(std::size_t(N));
    out = fft::Buf<float>(std::size_t(N));
    spec = fft::Buf<cf>(std::size_t(N / 2 + 1));
  }
  p1.resize(std::size_t(N) + 1);
  p2.resize(std::size_t(N) + 1);
  const bool one_run = Q.runs.size() == 1;
  if (!one_run) {
    s1.resize(std::size_t(L));
    s2.resize(std::size_t(L));
  }
  const double vq = Q.vq_c, inv_m = 1.0 / Q.nq, floor_vy = 1e-12 * Q.nq;
  const std::size_t nb = std::size_t(N / 2 + 1);
  for (std::size_t l0 = 0; l0 < n_out; l0 += std::size_t(L)) {
    const std::size_t nl = std::min<std::size_t>(std::size_t(L), n_out - l0);
    const std::size_t len = nl + std::size_t(m) - 1;
    float* __restrict inp = in.data();
    const T* __restrict yp = y.data() + l0;
    for (std::size_t i = 0; i < len; ++i) inp[i] = static_cast<float>(yp[i]);
    // Prefix sums of y and y^2, four samples per step so the adds overlap.
    double a1 = 0, a2 = 0;
    p1[0] = p2[0] = 0;
    std::size_t i = 0;
    for (; i + 4 <= len; i += 4) {
      const double v0 = yp[i], v1 = yp[i + 1], v2 = yp[i + 2], v3 = yp[i + 3];
      const double s01 = v0 + v1, s23 = v2 + v3, q0 = v0 * v0, q01 = q0 + v1 * v1, q23 = v2 * v2 + v3 * v3;
      p1[i + 1] = a1 + v0;
      p1[i + 2] = a1 + s01;
      p1[i + 3] = a1 + (s01 + v2);
      p2[i + 1] = a2 + q0;
      p2[i + 2] = a2 + q01;
      p2[i + 3] = a2 + (q01 + v2 * v2);
      a1 += s01 + s23;
      a2 += q01 + q23;
      p1[i + 4] = a1;
      p2[i + 4] = a2;
    }
    for (; i < len; ++i) {
      const double v = yp[i];
      a1 += v;
      a2 += v * v;
      p1[i + 1] = a1;
      p2[i + 1] = a2;
    }
    std::fill(in.begin() + len, in.end(), 0.0f);
    P.forward(in.data(), spec.data());
    cf* __restrict sp = spec.data();
    const cf* __restrict fq = Fq.data();
    for (std::size_t k = 0; k < nb; ++k) sp[k] *= fq[k];
    P.inverse(spec.data(), out.data());
    const float* __restrict sqy = out.data() + (m - 1);
    const double* __restrict q1 = p1.data();
    const double* __restrict q2 = p2.data();
    float* __restrict ro = r.data() + l0;
    if (one_run) {
      const std::size_t a = std::size_t(Q.runs[0].first), b = std::size_t(Q.runs[0].second);
      for (std::size_t i = 0; i < nl; ++i) {
        const double Sy = q1[i + b] - q1[i + a], Syy = q2[i + b] - q2[i + a];
        const double vy = Syy - Sy * Sy * inv_m;  // double: cancellation
        const float rr = std::clamp(sqy[i] / std::sqrt(static_cast<float>(vq * vy)), -1.0f, 1.0f);
        ro[i] = vy <= floor_vy ? NAN : rr;
      }
      continue;
    }
    double* __restrict w1 = s1.data();
    double* __restrict w2 = s2.data();
    std::fill(w1, w1 + nl, 0.0);
    std::fill(w2, w2 + nl, 0.0);
    for (auto [ra, rb] : Q.runs) {
      const double *__restrict b1 = q1 + rb, *__restrict e1 = q1 + ra, *__restrict b2 = q2 + rb, *__restrict e2 = q2 + ra;
      for (std::size_t i = 0; i < nl; ++i) {
        w1[i] += b1[i] - e1[i];
        w2[i] += b2[i] - e2[i];
      }
    }
    for (std::size_t i = 0; i < nl; ++i) {
      const double Sy = w1[i], vy = w2[i] - Sy * Sy * inv_m;
      const float rr = std::clamp(sqy[i] / std::sqrt(static_cast<float>(vq * vy)), -1.0f, 1.0f);
      ro[i] = vy <= floor_vy ? NAN : rr;
    }
  }
}

template <class T>
void masked_kernel(Query& Q, std::span<const T> y, std::span<float> r) {
  const int m = Q.m, N = Q.N, L = N - m + 1;
  const std::size_t n_out = r.size(), nb = std::size_t(N / 2 + 1);
  auto& P = fft::RealPlanD::get(N);
  const auto& S = Q.mask_spec();
  // Reused across calls (one runs around every gap).
  static thread_local fft::Buf<double> wy, yv, y2;
  static thread_local fft::Buf<cd> Gw, Gy, Gy2, tmp;
  static thread_local std::vector<fft::Buf<double>> outs(6);
  if (wy.size() != std::size_t(N)) {
    for (auto* b : {&wy, &yv, &y2}) *b = fft::Buf<double>(std::size_t(N));
    for (auto* b : {&Gw, &Gy, &Gy2, &tmp}) *b = fft::Buf<cd>(nb);
    for (auto& o : outs) o = fft::Buf<double>(std::size_t(N));
  }
  auto corr = [&](const fft::Buf<cd>& F, const fft::Buf<cd>& G, fft::Buf<double>& o) {
    for (std::size_t k = 0; k < nb; ++k) tmp[k] = F[k] * G[k];
    P.inverse(tmp.data(), o.data());
  };
  const double thr = Q.min_overlap * Q.nq - 0.5;
  double Sqq_full = 0;
  for (double v : Q.qv) Sqq_full += v * v;
  for (std::size_t l0 = 0; l0 < n_out; l0 += std::size_t(L)) {
    const std::size_t nl = std::min<std::size_t>(std::size_t(L), n_out - l0);
    const std::size_t len = nl + std::size_t(m) - 1;
    bool clean = true;
    for (std::size_t i = 0; i < std::size_t(N); ++i) {
      double v = i < len ? double(y[l0 + i]) : NAN;
      bool f = std::isfinite(v);
      if (i < len && !f) clean = false;
      wy[i] = f;
      yv[i] = f ? v : 0.0;
      y2[i] = yv[i] * yv[i];
    }
    P.forward(yv.data(), Gy.data());
    P.forward(y2.data(), Gy2.data());
    auto &Nn = outs[0], &Sy = outs[1], &Syy = outs[2], &Sq = outs[3], &Sqq = outs[4], &Sqy = outs[5];
    corr(S.Fw, Gy, Sy);
    corr(S.Fw, Gy2, Syy);
    corr(S.Fq, Gy, Sqy);
    if (!clean) {
      P.forward(wy.data(), Gw.data());
      corr(S.Fw, Gw, Nn);
      corr(S.Fq, Gw, Sq);
      corr(S.Fq2, Gw, Sqq);
    }
    for (std::size_t i = 0; i < nl; ++i) {
      const std::size_t o = std::size_t(m) - 1 + i;
      const double n = clean ? Q.nq : Nn[o], sq = clean ? Q.sum_q : Sq[o], sqq = clean ? Sqq_full : Sqq[o];
      const double cov = Sqy[o] - sq * Sy[o] / n;
      const double vq = sqq - sq * sq / n;
      const double vy = Syy[o] - Sy[o] * Sy[o] / n;
      double rr = cov / std::sqrt(vq * vy);
      bool bad = n < thr || vq <= 1e-12 || vy <= 1e-12 || !std::isfinite(rr);
      r[l0 + i] = bad ? NAN : static_cast<float>(std::clamp(rr, -1.0, 1.0));
    }
  }
}

// Pearson r at every lag of y (len n - m + 1). Gaps only affect lags whose window
// touches them: those runs (grown by 4m to avoid tiny FFTs) use the exact masked
// kernel, the rest the single-precision fast path. A gap sample k taints lags
// [k - m + 1, k], so the masked runs are the merged intervals around the (sparse) gaps.
template <class T>
void corr_block(Query& Q, std::span<const T> y, std::vector<float>& r) {
  const std::size_t m = std::size_t(Q.m), n = y.size();
  const std::size_t n_out = n >= m ? n - m + 1 : 0;
  r.resize(n_out);  // every lag is written below
  if (n_out == 0) return;
  auto run = [&](std::size_t a, std::size_t b, bool masked) {  // lags [a, b)
    if (a >= b) return;
    auto seg = y.subspan(a, b - a + m - 1);
    std::span<float> out(r.data() + a, b - a);
    masked ? masked_kernel<T>(Q, seg, out) : fast_kernel<T>(Q, seg, out);
  };
  const std::size_t g = 4 * m;
  std::size_t done = 0, da = 0, db = 0;  // lags < done are written; [da, db) is the pending masked run
  for (std::size_t k = dsp::next_nan(y.data(), 0, n); k < n; k = dsp::next_nan(y.data(), k + 1, n)) {
    const std::size_t lo = k + 1 >= m + g ? k + 1 - m - g : 0, hi = std::min(n_out, k + g + 1);
    if (lo > db) {
      run(done, da, false);
      run(da, db, true);
      done = db;
      da = lo;
    }
    db = std::max(db, hi);
  }
  run(done, da, false);
  run(da, db, true);
  run(std::max(done, db), n_out, false);
}

// Level evidence log p_clock(d) for d on the LUT grid (query-independent).
constexpr double LUT_HALF = 0.5, LUT_STEP = 5e-5;
constexpr std::size_t LUT_N = 20001;
const std::vector<double>& log_mix_table() {
  static const std::vector<double> t = [] {
    std::vector<double> v(LUT_N);
    const double W[2] = {0.8, 0.2}, S[2] = {0.002, 0.010};
    for (std::size_t i = 0; i < LUT_N; ++i) {
      double d = -LUT_HALF + double(i) * LUT_STEP;
      double c0 = std::log(W[0]) - 0.5 * (d / S[0]) * (d / S[0]) - std::log(S[0] * std::sqrt(2 * M_PI));
      double c1 = std::log(W[1]) - 0.5 * (d / S[1]) * (d / S[1]) - std::log(S[1] * std::sqrt(2 * M_PI));
      double mx = std::max(c0, c1);
      v[i] = mx + std::log(std::exp(c0 - mx) + std::exp(c1 - mx));
    }
    return v;
  }();
  return t;
}

}  // namespace

// ---------------------------------------------------------------- scoring + peaks

namespace {
// What the joint score of a block was normalised with, so single lags can be scored
// exactly like the block (the two-pass search re-scores its shortlist at full rate).
struct ScoreParams {
  bool ok = false;
  float med = 0, s = 1;
  const float* lut = nullptr;  // LUT_N + 1 entries, valid until the next joint score
};
}  // namespace

// Location and robust spread of the Fisher z of a block's (null) lags.
static void shape_norm(const std::vector<float>& sample, float& fmed, float& fs) {
  // Float selection (the sample is float data): half the bytes nth_element moves.
  static thread_local std::vector<float> dev;
  dev = sample;
  const double med = dsp::median_inplace(std::span<float>(dev));
  for (std::size_t i = 0; i < sample.size(); ++i) dev[i] = static_cast<float>(std::fabs(double(sample[i]) - med));
  double s = 1.4826 * dsp::median_inplace(std::span<float>(dev));
  if (s == 0) {
    double mean = std::accumulate(sample.begin(), sample.end(), 0.0) / double(sample.size()), v = 0;
    for (double x : sample) v += (x - mean) * (x - mean);
    s = std::sqrt(v / double(sample.size()));
  }
  if (s == 0) s = 1.0;
  fmed = static_cast<float>(med);
  fs = static_cast<float>(s);
}

// Level evidence over the offset: clock-error prior vs the grid's own level spread in
// this block. LUT_N + 1 entries (the last: no evidence), valid until the next call.
template <class T>
static const float* level_lut(std::span<const T> level_ref, double q_mean) {
  const std::size_t L = level_ref.size();
  double sum = 0, sum2 = 0;  // one pass: deviations are ~0.1 Hz, no cancellation to fear
  std::size_t cnt = 0;
  if constexpr (std::is_same_v<T, float>) {
    dsp::finite_sums(level_ref.data(), L, sum, sum2, cnt);
  } else {
    for (std::size_t i = 0; i < L; ++i) {
      const double v = level_ref[i];
      const bool f = v == v;
      sum += f ? v : 0.0;
      sum2 += f ? v * v : 0.0;
      cnt += f;
    }
  }
  const double mu = cnt ? sum / double(cnt) : 0.0;
  const double sg = std::max(cnt ? std::sqrt(std::max(0.0, sum2 / double(cnt) - mu * mu)) : 0.0, 0.005);
  const auto& mix = log_mix_table();
  static thread_local std::vector<float> lut(LUT_N + 1);
  const double c = q_mean - mu, norm = std::log(sg * std::sqrt(2 * M_PI));
  for (std::size_t i = 0; i < LUT_N; ++i) {
    const double d = -LUT_HALF + double(i) * LUT_STEP;
    const double nl = -0.5 * ((d - c) / sg) * ((d - c) / sg) - norm;
    lut[i] = static_cast<float>(mix[i] - nl);
  }
  lut[LUT_N] = 0.0f;
  return lut.data();
}

template <class T>
static void joint_score_impl(std::span<const float> r, std::span<const T> level_ref, double q_mean, int m, std::vector<float>& out,
                             ScoreParams* params = nullptr, const float* lut_in = nullptr);

std::vector<float> joint_score(std::span<const float> r, std::span<const double> level_ref, double q_mean, int m) {
  std::vector<float> out;
  joint_score_impl<double>(r, level_ref, q_mean, m, out);
  return out;
}

void joint_score_into(std::span<const float> r, std::span<const float> level_ref, double q_mean, int m, std::vector<float>& out) {
  joint_score_impl<float>(r, level_ref, q_mean, m, out);
}

template <class T>
static void joint_score_impl(std::span<const float> r, std::span<const T> level_ref, double q_mean, int m, std::vector<float>& out,
                             ScoreParams* params, const float* lut_in) {
  if (params) params->ok = false;
  const std::size_t n = r.size();
  out.resize(n);
  float* __restrict z = out.data();
  // Shape: Fisher z, standardised by the robust spread of this block's (null) lags.
  for (std::size_t i = 0; i < n; ++i) {
    const float ri = r[i];
    const bool nan = ri != ri;
    const float x = nan ? 0.0f : std::clamp(ri, -0.9999f, 0.9999f);
    const float zi = 0.5f * dsp::fast_logf((1.0f + x) / (1.0f - x));  // atanh(x), one log
    z[i] = nan ? NAN : zi;  // fast_logf has no NaN path
  }
  const std::size_t step = std::max<std::size_t>(1, n / 20000);
  static thread_local std::vector<float> sample;
  sample.clear();
  for (std::size_t i = 0; i < n; i += step)
    if (std::isfinite(z[i])) sample.push_back(z[i]);
  if (sample.size() < 50) {
    std::fill(out.begin(), out.end(), NAN);
    return;
  }
  float fmed, fs;
  shape_norm(sample, fmed, fs);
  for (std::size_t i = 0; i < n; ++i) {
    const float zs = (z[i] - fmed) / fs;
    z[i] = 0.5f * zs * std::fabs(zs);  // NaN stays NaN
  }
  // Level: the window mean of the reference vs the clip's mean deviation, through a
  // table over the offset (clock-error prior vs the grid's own level spread).
  const float* lut = lut_in ? lut_in : level_lut(level_ref, q_mean);
  if (params) *params = {true, fmed, fs, lut};
  // Window means of the level from chunk-local prefix sums (cache-resident, and the
  // per-lag loop vectorises: no running sum carried from lag to lag). Gap-free chunks
  // skip the valid counts and multiply by 1 / m.
  constexpr std::size_t CH = 16384;
  constexpr double INV_STEP = 1.0 / LUT_STEP;
  const std::size_t mm = std::size_t(m);
  static thread_local std::vector<double> ps, pc;
  static thread_local std::vector<float> fin;
  static thread_local std::vector<int> idx;
  ps.resize(CH + mm + 1);
  pc.resize(CH + mm + 1);
  fin.resize(CH + mm);
  idx.resize(CH);
  const double half_m = 0.5 * m, inv_m = 1.0 / m;
  // Table index of a window (LUT_N = "no level evidence").
  auto index = [&](double cnt, double mean) {
    const double v = std::clamp(std::nearbyint((q_mean - mean + LUT_HALF) * INV_STEP), 0.0, double(LUT_N - 1));
    return cnt > half_m && v == v ? static_cast<int>(v) : int(LUT_N);
  };
  for (std::size_t c0 = 0; c0 < n; c0 += CH) {
    const std::size_t cn = std::min(CH, n - c0), len = cn + mm - 1;
    const T* lv = level_ref.data() + c0;
    const bool gaps = dsp::has_nan(lv, len);
    dsp::prefix_sums(lv, len, ps.data(), gaps);
    const double* __restrict P = ps.data();
    int* __restrict ix = idx.data();
    // Two passes so both vectorise: table indices, then the gather-add.
    if (gaps) {
      for (std::size_t k = 0; k < len; ++k) fin[k] = lv[k] == lv[k] ? 1.0f : 0.0f;
      dsp::prefix_sums(fin.data(), len, pc.data());
      const double* __restrict C = pc.data();
      for (std::size_t i = 0; i < cn; ++i) {
        const double cnt = C[i + mm] - C[i], sum = P[i + mm] - P[i];
        ix[i] = index(cnt, sum / cnt);
      }
    } else {
      for (std::size_t i = 0; i < cn; ++i) {
        const double sum = P[i + mm] - P[i];
        ix[i] = index(double(m), sum * inv_m);
      }
    }
    float* __restrict zo = z + c0;
    const float* __restrict lt = lut;
    for (std::size_t i = 0; i < cn; ++i) zo[i] += lt[ix[i]];
  }
}

std::vector<std::size_t> local_peaks(std::span<const float> score, std::size_t k, int exclusion) {
  // A lag that is the maximum of [i - excl, i + excl] is also the maximum of its own
  // block of excl + 1 lags, so only block maxima (first position) are candidates; each
  // is confirmed against the lags of the two neighbouring blocks within its window.
  // Only the best k survive, so the scan keeps them in a min-heap and skips, 8 lags per
  // compare, everything not above the heap's smallest: few blocks are ever examined.
  const std::ptrdiff_t n = static_cast<std::ptrdiff_t>(score.size()), B = std::ptrdiff_t(exclusion) + 1;
  const float NEG = -INFINITY;
  const float* p = score.data();
  auto at = [&](std::ptrdiff_t j) {
    const float v = p[j];
    return v == v ? v : NEG;
  };
  // Candidates above T; when 2k have gathered, the best k are kept and T rises to the
  // k-th best (no candidate below it can make the final k).
  std::vector<std::pair<float, std::size_t>> best;
  if (k == 0) return {};
  best.reserve(2 * k + 1);
  auto gt = [](const auto& x, const auto& y) { return x.first > y.first; };
  float T = NEG;
  auto prune = [&] {
    std::nth_element(best.begin(), best.begin() + std::ptrdiff_t(k - 1), best.end(), gt);
    T = best[k - 1].first;
    best.resize(k);
  };
  auto consider = [&](std::ptrdiff_t b) {
    const std::ptrdiff_t s0 = b * B, s1 = std::min(n, s0 + B);
    float v = NEG;
    std::ptrdiff_t i = -1;
    for (std::ptrdiff_t j = s0; j < s1; ++j)
      if (p[j] > v) v = p[j], i = j;  // NaN never wins; first position of the maximum
    if (i < 0 || !(v > T)) return;
    for (std::ptrdiff_t nbk : {b - 1, b + 1}) {
      if (nbk < 0 || nbk * B >= n) continue;
      const std::ptrdiff_t lo = std::max(nbk * B, i - B + 1), hi = std::min({n, nbk * B + B, i + B});
      for (std::ptrdiff_t j = lo; j < hi; ++j)
        if (at(j) > v) return;
    }
    best.emplace_back(v, std::size_t(i));
    if (best.size() >= 2 * k) prune();
  };
  std::ptrdiff_t last = -1;  // last block examined
  auto hit = [&](std::ptrdiff_t j) {
    const std::ptrdiff_t b = j / B;
    if (b != last) consider(b), last = b;
  };
  std::ptrdiff_t j = 0;
  for (; j + 8 <= n; j += 8) {
    unsigned mask = unsigned(_mm256_movemask_ps(_mm256_cmp_ps(_mm256_loadu_ps(p + j), _mm256_set1_ps(T), _CMP_GT_OQ)));
    while (mask) {
      const int l = __builtin_ctz(mask);
      mask &= mask - 1;
      if (p[j + l] > T) hit(j + l);  // T may have risen since the compare
    }
  }
  for (; j < n; ++j)
    if (p[j] > T) hit(j);
  if (best.size() > k) prune();
  std::sort(best.begin(), best.end(), gt);
  std::vector<std::size_t> cand(best.size());
  for (std::size_t i = 0; i < best.size(); ++i) cand[i] = best[i].second;
  return cand;
}

bool Candidate::consistent() const { return !(gain < GAIN_MIN || gain > GAIN_MAX || mismatch > MISMATCH_MAX); }

std::vector<Candidate> merge_topk(std::vector<Candidate> c, std::size_t k, double exclusion) {
  std::stable_sort(c.begin(), c.end(), [](auto& a, auto& b) {
    const bool ca = a.consistent(), cb = b.consistent();
    return ca != cb ? ca : a.score > b.score;
  });
  std::vector<Candidate> out;
  for (auto& x : c) {
    bool far = std::all_of(out.begin(), out.end(), [&](auto& u) { return std::fabs(x.start_utc - u.start_utc) >= exclusion; });
    if (far) {
      out.push_back(std::move(x));
      if (out.size() >= k) break;
    }
  }
  return out;
}

static double refine_lag(std::span<const float> r, std::ptrdiff_t i) {
  auto at = [&](std::ptrdiff_t j) { return j >= 0 && j < std::ptrdiff_t(r.size()) ? double(r[std::size_t(j)]) : NAN; };
  double a = at(i - 1), b = at(i), c = at(i + 1);
  double den = a - 2 * b + c;
  if (!std::isfinite(den) || den >= 0) return 0.0;
  return std::clamp(0.5 * (a - c) / den, -0.5, 0.5);
}

// ---------------------------------------------------------------- two-pass search
//
// The wobble the score matches on is band-limited: the reference is smoothed like a
// 12 s frame and the query comes from 12 s frames, and the high-pass removes what is
// slower than about a minute. Block means of D samples keep almost all of it, so a
// first pass correlates the D-times-decimated signals (D times fewer lags, shorter
// FFTs) and keeps a generous shortlist of its peaks; the second pass re-scores the lags
// around each one exactly as the single-pass search would (same Pearson r, same score
// normalisation), so only the shortlist depends on the decimated pass.

constexpr std::size_t MIN_TWO_PASS_LAGS = std::size_t(1) << 18;

template <int D, class T, class U>
static void block_means_d(const T* __restrict x, std::size_t nc, U* __restrict out) {
  constexpr T inv = T(1) / T(D);
  for (std::size_t k = 0; k < nc; ++k) {
    T s = 0;  // NaN if any sample of the block is missing
    for (int j = 0; j < D; ++j) s += x[k * D + std::size_t(j)];
    out[k] = static_cast<U>(s * inv);
  }
}

template <class T, class U>
static void block_means(const T* x, std::size_t n, int D, std::vector<U>& out) {
  const std::size_t nc = n / std::size_t(D);
  out.resize(nc);
  if constexpr (std::is_same_v<T, float> && std::is_same_v<U, float>)
    if (D == 4) {
      // Eight means per step: two horizontal adds pair up the 32 samples, a permute
      // restores the order. Same sums as the scalar loop ((x0 + x1) + (x2 + x3)) * 1/4.
      std::size_t k = 0;
      float* __restrict o = out.data();
      const __m256 q = _mm256_set1_ps(0.25f);
      for (; k + 8 <= nc; k += 8) {
        const float* p = x + 4 * k;
        const __m256 a = _mm256_hadd_ps(_mm256_loadu_ps(p), _mm256_loadu_ps(p + 8));
        const __m256 b = _mm256_hadd_ps(_mm256_loadu_ps(p + 16), _mm256_loadu_ps(p + 24));
        const __m256 c = _mm256_hadd_ps(a, b);  // lanes: m0 m2 m4 m6 | m1 m3 m5 m7 ... per 128-bit half
        _mm256_storeu_ps(o + k, _mm256_mul_ps(_mm256_permutevar8x32_ps(c, _mm256_setr_epi32(0, 4, 1, 5, 2, 6, 3, 7)), q));
      }
      for (; k < nc; ++k) o[k] = ((x[4 * k] + x[4 * k + 1]) + (x[4 * k + 2] + x[4 * k + 3])) * 0.25f;
      return;
    }
  switch (D) {
    case 2: return block_means_d<2>(x, nc, out.data());
    case 3: return block_means_d<3>(x, nc, out.data());
    case 4: return block_means_d<4>(x, nc, out.data());
    default: {
      for (std::size_t k = 0; k < nc; ++k) {
        T s = 0;
        for (int j = 0; j < D; ++j) s += x[k * std::size_t(D) + std::size_t(j)];
        out[k] = static_cast<U>(s / T(D));
      }
    }
  }
}

// For a query with missing samples, over K gap-free windows y[k][0, m) in one pass:
// qc . y (the covariance term), and the window's sum and sum of squares over the
// query's valid samples (weights wf = 0/1). Three 8-wide accumulators per window.
template <int K>
static void masked_sums(const Query& Q, const float* const* y, double* Sqy, double* Sy, double* Syy) {
  const std::size_t m = std::size_t(Q.m);
  const float* __restrict q = Q.qcf.data();
  const float* __restrict w = Q.wf.data();
  __m256 aq[K], ay[K], ayy[K];
  for (int k = 0; k < K; ++k) aq[k] = ay[k] = ayy[k] = _mm256_setzero_ps();
  std::size_t j = 0;
  for (; j + 8 <= m; j += 8) {
    const __m256 q0 = _mm256_loadu_ps(q + j), w0 = _mm256_loadu_ps(w + j);
    for (int k = 0; k < K; ++k) {
      const __m256 v = _mm256_loadu_ps(y[k] + j), wv = _mm256_mul_ps(w0, v);
      aq[k] = _mm256_fmadd_ps(q0, v, aq[k]);
      ay[k] = _mm256_add_ps(ay[k], wv);
      ayy[k] = _mm256_fmadd_ps(wv, v, ayy[k]);
    }
  }
  auto hsum = [](__m256 v) {
    __m128 h = _mm_add_ps(_mm256_castps256_ps128(v), _mm256_extractf128_ps(v, 1));
    h = _mm_add_ps(h, _mm_movehl_ps(h, h));
    h = _mm_add_ss(h, _mm_movehdup_ps(h));
    return _mm_cvtss_f32(h);
  };
  for (int k = 0; k < K; ++k) {
    float tq = hsum(aq[k]), ty = hsum(ay[k]), tyy = hsum(ayy[k]);
    for (std::size_t i = j; i < m; ++i) {
      const float v = y[k][i], wv = w[i] * v;
      tq += q[i] * v;
      ty += wv;
      tyy += wv * v;
    }
    Sqy[k] = tq, Sy[k] = ty, Syy[k] = tyy;
  }
}

// r of a query with missing samples from masked_sums.
static float masked_r(const Query& Q, double Sqy, double Sy, double Syy) {
  const double vy = Syy - Sy * Sy / Q.nq;
  if (vy <= 1e-12 * Q.nq || Q.vq_c <= 1e-12) return NAN;
  return static_cast<float>(std::clamp(Sqy / std::sqrt(Q.vq_c * vy), -1.0, 1.0));
}

// Pearson r of the query against y[L, L + m): the fast kernel's formula on a gap-free
// window, the masked kernel's otherwise.
static float exact_r(const Query& Q, std::span<const float> y, std::size_t L) {
  const std::size_t m = std::size_t(Q.m);
  const float* yp = y.data() + L;
  if (Q.full && !dsp::has_nan(yp, m)) {
    // Float sums (vectorised): the window is high-passed, so its mean is near zero and
    // Syy - Sy^2 / m does not cancel.
    const float* __restrict qf = Q.qf.data();
    float Sy = 0, Syy = 0, Sqy = 0;
    for (std::size_t j = 0; j < m; ++j) {
      const float v = yp[j];
      Sy += v;
      Syy += v * v;
      Sqy += qf[j] * v;
    }
    const double vy = double(Syy) - double(Sy) * double(Sy) / double(m);
    if (vy <= 1e-12 * double(m)) return NAN;
    return static_cast<float>(std::clamp(double(Sqy) / std::sqrt(Q.vq_full * vy), -1.0, 1.0));
  }
  if (!Q.full && !dsp::has_nan(yp, m)) {
    const float* w[1] = {yp};
    double a, b, c;
    masked_sums<1>(Q, w, &a, &b, &c);
    return masked_r(Q, a, b, c);
  }
  double n = 0, Sy = 0, Syy = 0, Sqy = 0, Sq = 0, Sqq = 0;
  for (std::size_t j = 0; j < m; ++j) {
    const double v = yp[j];
    if (!Q.w[j] || !std::isfinite(v)) continue;
    const double q = Q.qv[j];
    n += 1;
    Sy += v;
    Syy += v * v;
    Sqy += q * v;
    Sq += q;
    Sqq += q * q;
  }
  if (n < Q.min_overlap * Q.nq - 0.5) return NAN;
  const double cov = Sqy - Sq * Sy / n, vq = Sqq - Sq * Sq / n, vy = Syy - Sy * Sy / n;
  const double rr = cov / std::sqrt(vq * vy);
  if (vq <= 1e-12 || vy <= 1e-12 || !std::isfinite(rr)) return NAN;
  return static_cast<float>(std::clamp(rr, -1.0, 1.0));
}

// q . y[L_k, L_k + m) for K windows in one pass over the query: two 8-wide
// accumulators per window, so the FMAs are not latency-bound (2K chains in flight).
template <int K>
static void dots(const float* __restrict q, const float* const* y, std::size_t m, float* out) {
  __m256 acc[K][2];
  for (int k = 0; k < K; ++k) acc[k][0] = acc[k][1] = _mm256_setzero_ps();
  std::size_t j = 0;
  for (; j + 16 <= m; j += 16) {
    const __m256 q0 = _mm256_loadu_ps(q + j), q1 = _mm256_loadu_ps(q + j + 8);
    for (int k = 0; k < K; ++k) {
      acc[k][0] = _mm256_fmadd_ps(q0, _mm256_loadu_ps(y[k] + j), acc[k][0]);
      acc[k][1] = _mm256_fmadd_ps(q1, _mm256_loadu_ps(y[k] + j + 8), acc[k][1]);
    }
  }
  for (; j + 8 <= m; j += 8) {
    const __m256 q0 = _mm256_loadu_ps(q + j);
    for (int k = 0; k < K; ++k) acc[k][0] = _mm256_fmadd_ps(q0, _mm256_loadu_ps(y[k] + j), acc[k][0]);
  }
  for (int k = 0; k < K; ++k) {
    const __m256 v = _mm256_add_ps(acc[k][0], acc[k][1]);
    __m128 h = _mm_add_ps(_mm256_castps256_ps128(v), _mm256_extractf128_ps(v, 1));
    h = _mm_add_ps(h, _mm_movehl_ps(h, h));
    h = _mm_add_ss(h, _mm_movehdup_ps(h));
    float t = _mm_cvtss_f32(h);
    for (std::size_t i = j; i < m; ++i) t += q[i] * y[k][i];
    out[k] = t;
  }
}

// r of a full query from the window's dot product and its (double) sums, as exact_r.
static float r_from_sums(const Query& Q, float Sqy, double Sy, double Syy) {
  const double m = double(Q.m), vy = Syy - Sy * Sy / m;
  if (vy <= 1e-12 * m) return NAN;
  return static_cast<float>(std::clamp(double(Sqy) / std::sqrt(Q.vq_full * vy), -1.0, 1.0));
}

// r at the gap-free lags `lags` (sums given): dot products four windows at a time.
static void r_at(const Query& Q, std::span<const float> y, const std::size_t* lags, std::size_t n, const double* Sy,
                 const double* Syy, float* out) {
  const std::size_t m = std::size_t(Q.m);
  const float* qf = Q.qf.data();
  std::size_t i = 0;
  for (; i + 4 <= n; i += 4) {
    const float* w[4] = {y.data() + lags[i], y.data() + lags[i + 1], y.data() + lags[i + 2], y.data() + lags[i + 3]};
    float d[4];
    dots<4>(qf, w, m, d);
    for (int k = 0; k < 4; ++k) out[i + std::size_t(k)] = r_from_sums(Q, d[k], Sy[i + std::size_t(k)], Syy[i + std::size_t(k)]);
  }
  for (; i < n; ++i) {
    const float* w[1] = {y.data() + lags[i]};
    float d;
    dots<1>(qf, w, m, &d);
    out[i] = r_from_sums(Q, d, Sy[i], Syy[i]);
  }
}

// exact_r at `lags` for a query with missing samples: gap-free windows four at a time.
static void gappy_r_at(const Query& Q, std::span<const float> y, const std::size_t* lags, std::size_t n, float* out) {
  const std::size_t m = std::size_t(Q.m);
  std::size_t i = 0;
  for (; i + 4 <= n; i += 4) {
    const float* w[4] = {y.data() + lags[i], y.data() + lags[i + 1], y.data() + lags[i + 2], y.data() + lags[i + 3]};
    bool clean = true;
    for (int k = 0; k < 4 && clean; ++k) clean = !dsp::has_nan(w[k], m);
    if (!clean) {
      for (int k = 0; k < 4; ++k) out[i + std::size_t(k)] = exact_r(Q, y, lags[i + std::size_t(k)]);
      continue;
    }
    double a[4], b[4], c[4];
    masked_sums<4>(Q, w, a, b, c);
    for (int k = 0; k < 4; ++k) out[i + std::size_t(k)] = masked_r(Q, a[k], b[k], c[k]);
  }
  for (; i < n; ++i) out[i] = exact_r(Q, y, lags[i]);
}

// exact_r for lags [a, b]. A full query over a gap-free stretch slides the window sums
// (double) from lag to lag, so only the dot products cost O(m) per lag.
static void exact_r_range(const Query& Q, std::span<const float> y, std::size_t a, std::size_t b, float* out) {
  const std::size_t m = std::size_t(Q.m), n = b - a + 1;
  static thread_local std::vector<std::size_t> lags;
  if (!Q.full) {
    lags.resize(n);
    for (std::size_t k = 0; k < n; ++k) lags[k] = a + k;
    gappy_r_at(Q, y, lags.data(), n, out);
    return;
  }
  if (dsp::has_nan(y.data() + a, n + m - 1)) {
    for (std::size_t L = a; L <= b; ++L) out[L - a] = exact_r(Q, y, L);
    return;
  }
  static thread_local std::vector<double> s1, s2;
  lags.resize(n);
  s1.resize(n);
  s2.resize(n);
  const float* yp = y.data();
  double a1 = 0, a2 = 0;
  for (std::size_t j = a; j < a + m; ++j) {
    const double v = yp[j];
    a1 += v;
    a2 += v * v;
  }
  for (std::size_t k = 0; k < n; ++k) {
    if (k) {
      const double vi = yp[a + k + m - 1], vo = yp[a + k - 1];
      a1 += vi - vo;
      a2 += vi * vi - vo * vo;
    }
    lags[k] = a + k;
    s1[k] = a1;
    s2[k] = a2;
  }
  r_at(Q, y, lags.data(), n, s1.data(), s2.data(), out);
}

// exact_r at the lags 0, step, 2 step, ... < n_out (r[k] for lag k * step). Window sums
// come from per-chunk sums (chunks of `step` samples) plus the window's partial last
// chunk, and gap-free windows are found from per-chunk NaN counts, so each window costs
// one dot product instead of three sums and a NaN scan over m samples.
static void sampled_r(const Query& Q, std::span<const float> y, std::size_t n_out, std::size_t step, std::vector<float>& r) {
  const std::size_t m = std::size_t(Q.m), ns = (n_out + step - 1) / step;
  r.resize(ns);
  if (!Q.full) {
    static thread_local std::vector<std::size_t> gl;
    gl.resize(ns);
    for (std::size_t k = 0; k < ns; ++k) gl[k] = k * step;
    gappy_r_at(Q, y, gl.data(), ns, r.data());
    return;
  }
  const std::size_t qc = m / step, rem = m % step;
  const std::size_t nch = std::min(ns + qc, y.size() / step);  // whole chunks [k step, (k + 1) step)
  static thread_local std::vector<double> P1, P2, s1, s2;
  static thread_local std::vector<std::uint32_t> PN;
  static thread_local std::vector<std::size_t> lags, idx;
  P1.resize(nch + 1);
  P2.resize(nch + 1);
  PN.resize(nch + 1);
  P1[0] = P2[0] = 0;
  PN[0] = 0;
  const float* yp = y.data();
  for (std::size_t c = 0; c < nch; ++c) {
    const float* __restrict x = yp + c * step;
    float c1 = 0, c2 = 0;  // a chunk is short and high-passed (mean ~0): float is exact enough
    std::uint32_t bad = 0;
    for (std::size_t j = 0; j < step; ++j) {
      const float v = x[j];
      const bool f = v == v;
      const float d = f ? v : 0.0f;
      c1 += d;
      c2 += d * d;
      bad += !f;
    }
    P1[c + 1] = P1[c] + c1;
    P2[c + 1] = P2[c] + c2;
    PN[c + 1] = PN[c] + bad;
  }
  lags.clear();
  idx.clear();
  s1.clear();
  s2.clear();
  for (std::size_t k = 0; k < ns; ++k) {
    const std::size_t L = k * step;
    bool clean = k + qc <= nch && PN[k + qc] == PN[k];
    double t1 = 0, t2 = 0;
    if (clean) {
      const float* __restrict x = yp + L + qc * step;
      std::uint32_t bad = 0;
      for (std::size_t j = 0; j < rem; ++j) {
        const float v = x[j];
        bad |= std::uint32_t(v != v);
        t1 += v;
        t2 += double(v) * v;
      }
      clean = !bad;
    }
    if (!clean) {
      r[k] = exact_r(Q, y, L);
      continue;
    }
    lags.push_back(L);
    idx.push_back(k);
    s1.push_back(P1[k + qc] - P1[k] + t1);
    s2.push_back(P2[k + qc] - P2[k] + t2);
  }
  static thread_local std::vector<float> rr;
  rr.resize(lags.size());
  r_at(Q, y, lags.data(), lags.size(), s1.data(), s2.data(), rr.data());
  for (std::size_t i = 0; i < idx.size(); ++i) r[idx[i]] = rr[i];
}

// Fisher z of r, as joint_score_impl computes it.
static float fisher_z(float r) {
  const float x = std::clamp(r, -0.9999f, 0.9999f);
  return 0.5f * dsp::fast_logf((1.0f + x) / (1.0f - x));
}

// The joint score of one lag from its r and its window's level count and sum,
// normalised like its block (see joint_score_impl).
static float exact_score(float r, double cnt, double sum, int m, double q_mean, const ScoreParams& P) {
  if (r != r) return NAN;
  const float zs = (fisher_z(r) - P.med) / P.s;
  int idx = int(LUT_N);
  if (cnt > 0.5 * m) {
    const double v = std::clamp(std::nearbyint((q_mean - sum / cnt + LUT_HALF) * (1.0 / LUT_STEP)), 0.0, double(LUT_N - 1));
    if (v == v) idx = static_cast<int>(v);
  }
  return 0.5f * zs * std::fabs(zs) + P.lut[idx];
}

template <class T>
static void clip_outliers_impl(std::span<T> x, double k) {
  // Robust spread from every 16th finite sample: deterministic, and cheap on a block.
  std::vector<float> v;
  const std::size_t step = x.size() > (std::size_t(1) << 16) ? 16 : 1;
  for (std::size_t i = 0; i < x.size(); i += step)
    if (std::isfinite(x[i])) v.push_back(float(x[i]));
  if (v.size() < 20) return;
  const double med = dsp::median_inplace(std::span<float>(v));
  for (auto& u : v) u = float(std::fabs(double(u) - med));
  const double sd = 1.4826 * dsp::median_inplace(std::span<float>(v));
  if (!(sd > 0)) return;
  const T lo = T(med - k * sd), hi = T(med + k * sd);
  for (auto& u : x)
    if (u == u) u = std::clamp(u, lo, hi);  // NaN stays NaN
}
void clip_outliers(std::span<double> x, double k) { clip_outliers_impl(x, k); }
void clip_outliers(std::span<float> x, double k) { clip_outliers_impl(x, k); }

static void describe(Candidate& c, const ENFTrace& tr, const std::string& grid, int hp) {
  const double first = tr.times[0];
  const i64 t0 = static_cast<i64>(std::floor(c.start_utc + first + 0.5));
  const std::size_t m = tr.freq.size();
  auto raw = store::read(grid, t0 - 16, t0 + i64(m) + 16);
  std::vector<double> rd(raw.begin(), raw.end());
  auto sm = dsp::smooth_like_frames(rd, tr.frame_s);
  std::vector<double> d, qa, sa;
  for (std::size_t i = 0; i < m; ++i) {
    double q = tr.freq[i] - tr.nominal, s = sm[16 + i];
    if (std::isfinite(q) && std::isfinite(s)) {
      d.push_back(q - s);
      qa.push_back(q);
      sa.push_back(s);
    }
  }
  if (d.size() < 10) return;
  const double n = double(d.size());
  double md = std::accumulate(d.begin(), d.end(), 0.0) / n;
  double ss = 0;
  for (double x : d) ss += (x - md) * (x - md);
  c.offset_hz = md;
  c.rmse_hz = std::sqrt(ss / n);
  double mq = std::accumulate(qa.begin(), qa.end(), 0.0) / n, ms = std::accumulate(sa.begin(), sa.end(), 0.0) / n;
  double ab = 0, aa = 0, bb = 0;
  for (std::size_t i = 0; i < qa.size(); ++i) {
    double a = qa[i] - mq, b = sa[i] - ms;
    ab += a * b;
    aa += a * a;
    bb += b * b;
  }
  double den = std::sqrt(aa * bb);
  c.r_raw = den > 0 ? ab / den : NAN;
  std::size_t off = 0;
  for (double x : d) off += std::fabs(x - md) > MISMATCH_HZ;
  c.mismatch = double(off) / n;
  // Gain: least-squares slope of the clip's wobble on the reference's (the search's
  // high-pass). Noise in the clip doesn't bias it; it is ~1 at the true time.
  std::vector<double> qd(m, NAN), sd(m, NAN);
  for (std::size_t i = 0; i < m; ++i) {
    qd[i] = tr.freq[i] - tr.nominal;
    sd[i] = sm[16 + i];
  }
  auto qh = dsp::highpass(qd, hp), sh = dsp::highpass(sd, hp);
  double sq = 0, ss2 = 0, sxy = 0, sxx = 0, k = 0;
  for (std::size_t i = 0; i < m; ++i)
    if (std::isfinite(qh[i]) && std::isfinite(sh[i])) sq += qh[i], ss2 += sh[i], ++k;
  if (k < 10) return;
  sq /= k, ss2 /= k;
  for (std::size_t i = 0; i < m; ++i)
    if (std::isfinite(qh[i]) && std::isfinite(sh[i])) {
      sxy += (qh[i] - sq) * (sh[i] - ss2);
      sxx += (sh[i] - ss2) * (sh[i] - ss2);
    }
  if (sxx > 0) c.gain = sxy / sxx;
}

// ---------------------------------------------------------------- search

std::vector<GridResult> search_grid(std::span<const ENFTrace* const> traces, const std::string& grid, const SearchOptions& o) {
  const auto tic = std::chrono::steady_clock::now();
  struct Q {
    const ENFTrace* tr;
    int m, hp, excl, margin;
    double first, q_mean;
    std::unique_ptr<Query> query, coarse;  // coarse: block means of D samples (two-pass search)
    int D = 1;
    std::vector<Candidate> cands;
    i64 n_lags = 0, finite_cnt = 0, total_cnt = 0;
    double from = NAN, to = NAN;
    std::string error;
  };
  std::vector<Q> qs(traces.size());
  for (std::size_t i = 0; i < traces.size(); ++i) {
    const auto& tr = *traces[i];
    if (std::fabs(tr.hop_s - 1.0) > 1e-9) throw std::invalid_argument("matcher expects a 1 s hop");
    auto& q = qs[i];
    q.tr = &tr;
    q.m = static_cast<int>(tr.freq.size());
    q.hp = hp_window_for(q.m, o.hp_window_s);
    auto dev = tr.deviation();
    try {
      auto hq = dsp::highpass(dev, q.hp);
      clip_outliers(std::span<double>(hq));
      q.query = std::make_unique<Query>(hq, o.min_overlap);
      for (int D = o.decimate; D > 1 && !q.coarse; --D) {
        if (q.m / D < o.min_coarse_len) continue;
        std::vector<double> c;
        block_means(hq.data(), hq.size(), D, c);
        try {
          q.coarse = std::make_unique<Query>(c, o.min_overlap);
          q.D = D;
        } catch (const std::invalid_argument&) {
        }
      }
    } catch (const std::invalid_argument& e) {
      q.error = e.what();
    }
    q.first = tr.times.empty() ? 0 : tr.times[0];
    q.excl = std::max(30, q.m / 2);
    double s = 0;
    std::size_t c = 0;
    for (double v : dev)
      if (std::isfinite(v)) {
        s += v;
        ++c;
      }
    q.q_mean = c ? s / double(c) : NAN;
    q.margin = q.hp / 2 + static_cast<int>(std::ceil(tr.frame_s)) + 2;
  }

  // Queries that share drift window and frame length share the prepared reference.
  std::map<std::pair<int, double>, std::vector<std::size_t>> groups;
  for (std::size_t i = 0; i < qs.size(); ++i)
    if (qs[i].query) groups[{qs[i].hp, qs[i].tr->frame_s}].push_back(i);

  // The query spectra (and the FFT plans behind them) are built lazily; build them now,
  // before the blocks are spread over threads that only read them.
  for (auto& q : qs)
    for (Query* Qp : {q.query.get(), q.coarse.get()})
      if (Qp) {
        Qp->fast_spec();
        Qp->mask_spec();
      }

  // Every block of every coverage span, with the queries it serves. A query drops out of
  // a span once it no longer fits before the span's end, or once a block reached it.
  struct Job {
    int hp, margin;
    double frame_s;
    i64 s, b;
    std::vector<std::size_t> active;  // indices into qs
  };
  std::vector<Job> jobs;
  const auto spans = store::coverage_intervals(grid);
  for (auto& [key, members] : groups) {
    std::vector<char> active(qs.size(), 0);
    for (auto iv : spans) {
      const i64 a = o.from ? std::max(iv.a, *o.from) : iv.a;
      const i64 b = o.to ? std::min(iv.b, *o.to) : iv.b;
      for (auto qi : members) active[qi] = 1;
      for (i64 s = a;; s += o.block_s) {
        Job j{key.first, qs[members[0]].margin, key.second, s, b, {}};
        for (auto qi : members) {
          if (active[qi] && s + qs[qi].m <= b) j.active.push_back(qi);
          else active[qi] = 0;
        }
        if (j.active.empty()) break;
        for (auto qi : j.active)
          if (std::min(b, s + o.block_s + qs[qi].m - 1) >= b) active[qi] = 0;  // this block reaches the end
        jobs.push_back(std::move(j));
      }
    }
  }

  // What one block found for one query; merged in block order below, so the result is
  // the same whatever the number of threads.
  struct Part {
    std::vector<Candidate> cands;
    i64 n_lags = 0, finite_cnt = 0, total_cnt = 0;
    double from = NAN, to = NAN;
  };
  std::vector<std::vector<Part>> parts(jobs.size());

  // Per-thread working buffers: a block's reference at each filter stage, its scores.
  struct Buffers {
    std::vector<float> raw, smooth, hpv;  // float: the store is 1 mHz-quantised float data
    std::vector<std::pair<std::size_t, std::size_t>> nan_runs;  // [a, b) of raw, before filling
    std::vector<float> r, score;
    std::vector<float> yc, lvc, rc, scc, rx;  // two-pass search
  };

  // Two-pass block search (see "two-pass search" above): decimated correlation and
  // score, then the exact score on the lags around each shortlisted peak.
  auto two_pass = [&](Buffers& w, Q& q, int D, std::span<const float> y, std::span<const float> lv, std::size_t n_out, i64 s,
                      std::size_t qi, std::vector<Candidate>& out) {
    const std::size_t ud = std::size_t(D), m = std::size_t(q.m);
    ScoreParams P;
    if (o.joint) {
      // Normalise exactly like the single pass: Fisher z of the exact r at the same
      // sampled lags, and the level table of the full-rate reference.
      static thread_local std::vector<float> sample;
      sample.clear();
      const std::size_t step = std::max<std::size_t>(1, n_out / 20000);
      auto add = [&](float rr) {
        if (rr == rr) sample.push_back(fisher_z(rr));
      };
      static thread_local std::vector<float> rs;
      sampled_r(*q.query, y, n_out, step, rs);
      for (float rr : rs) add(rr);
      if (sample.size() < 50) return;  // the single pass finds nothing here either
      shape_norm(sample, P.med, P.s);
      P.lut = level_lut(lv, q.q_mean);
    }
    std::vector<std::size_t> peaks;
    {
      block_means(y.data(), y.size(), D, w.yc);
      corr_block<float>(*q.coarse, std::span<const float>(w.yc), w.rc);
      std::span<const float> sc = w.rc;
      if (o.joint) {
        block_means(lv.data(), lv.size(), D, w.lvc);
        ScoreParams Pc;
        joint_score_impl<float>(w.rc, std::span<const float>(w.lvc), q.q_mean, q.coarse->m, w.scc, &Pc, P.lut);
        if (!Pc.ok) return;
        sc = w.scc;
      }
      // Every bump (a frame apart), not only the regional maxima: which bump of a region
      // is highest can differ between the decimated and the exact score.
      const int bump = std::max(1, static_cast<int>(std::lround(q.tr->frame_s / D)));
      peaks = local_peaks(sc, std::size_t(std::max(o.shortlist, o.top_k)), bump);
    }
    std::vector<Candidate> found;
    for (auto j : peaks) {
      // Lags [lo, hi] around the coarse peak, plus one each side for the refinement.
      const std::size_t c = j * ud, lo = c >= ud ? c - ud : 0, hi = std::min(n_out - 1, c + ud);
      const std::size_t a = lo ? lo - 1 : 0, b = std::min(n_out - 1, hi + 1);
      w.rx.resize(b - a + 1);
      exact_r_range(*q.query, y, a, b, w.rx.data());
      // Level window sums, slid from lag to lag.
      double cnt = 0, sum = 0;
      if (o.joint) {
        const float* __restrict lp = lv.data() + lo;
        for (std::size_t k = 0; k < m; ++k) {
          const float v = lp[k];
          const bool f = v == v;
          cnt += f ? 1.0 : 0.0;
          sum += f ? double(v) : 0.0;
        }
      }
      float best = -INFINITY, best_r = NAN;
      std::size_t bi = 0;
      for (std::size_t L = lo; L <= hi; ++L) {
        if (o.joint && L > lo) {
          const float in = lv[L + m - 1], out = lv[L - 1];
          if (in == in) cnt += 1, sum += in;
          if (out == out) cnt -= 1, sum -= out;
        }
        const float rr = w.rx[L - a];
        const float v = o.joint ? exact_score(rr, cnt, sum, q.m, q.q_mean, P) : rr;
        if (v > best) best = v, best_r = rr, bi = L;
      }
      if (!(best > -INFINITY)) continue;
      Candidate cd;
      cd.grid = grid;
      cd.start_utc = double(s) + double(bi) + refine_lag(std::span<const float>(w.rx), std::ptrdiff_t(bi - a)) - q.first;
      cd.r = best_r;
      cd.score = best;
      cd.origin = static_cast<int>(qi);
      found.push_back(std::move(cd));
    }
    // Like local_peaks: keep the lags that beat every other one within the exclusion
    // window, then the block's best top_k.
    std::sort(found.begin(), found.end(), [](auto& x, auto& y) { return x.score > y.score; });
    std::size_t kept = 0;
    for (std::size_t i = 0; i < found.size() && kept < std::size_t(o.top_k); ++i) {
      bool top = true;
      for (std::size_t j = 0; j < i && top; ++j) top = std::fabs(found[j].start_utc - found[i].start_utc) > q.excl;
      if (top) out.push_back(std::move(found[i])), ++kept;
    }
  };

  // One block: read and filter the reference once for every query it serves, then
  // correlate and shortlist each query against it.
  auto run_job = [&](Buffers& w, std::size_t ji) {
    const Job& job = jobs[ji];
    const i64 s = job.s, b = job.b;
    i64 e_max = s;
    for (auto qi : job.active) e_max = std::max(e_max, std::min(b, s + o.block_s + qs[qi].m - 1));
    const i64 r0 = s - job.margin, r1 = e_max + job.margin;
    auto& raw = w.raw;
    raw.resize(std::size_t(r1 - r0));
    store::read(grid, r0, r1, raw);
    // Missing samples as runs (for the coverage counts), then the gaps bridged in place.
    w.nan_runs.clear();
    for (std::size_t k = dsp::next_nan(raw.data(), 0, raw.size()); k < raw.size();) {
      std::size_t e = k;
      while (e < raw.size() && raw[e] != raw[e]) ++e;
      w.nan_runs.emplace_back(k, e);
      k = dsp::next_nan(raw.data(), e, raw.size());
    }
    dsp::fill_short_gaps<float>(raw);
    w.smooth.resize(raw.size());
    dsp::smooth_like_frames<float>(raw, job.frame_s, w.smooth);
    w.hpv.resize(w.smooth.size());
    dsp::highpass<float>(w.smooth, job.hp, w.hpv);
    clip_outliers(std::span<float>(w.hpv));
    auto& out = parts[ji];
    out.resize(job.active.size());
    for (std::size_t k = 0; k < job.active.size(); ++k) {
      const std::size_t qi = job.active[k];
      auto& q = qs[qi];
      auto& part = out[k];
      const i64 e = std::min(b, s + o.block_s + q.m - 1);
      const std::size_t off = std::size_t(job.margin), n = std::size_t(e - s);
      std::span<const float> y(w.hpv.data() + off, n), lv(w.smooth.data() + off, n);
      const std::size_t n_out = n >= std::size_t(q.m) ? n - std::size_t(q.m) + 1 : 0;
      const int D = q.D;
      // Short blocks (a stretch of data between long gaps) stay single-pass: the
      // exact normalisation sample (20,000 lags) would cost more than it saves.
      if (D > 1 && n_out >= MIN_TWO_PASS_LAGS) {
        two_pass(w, q, D, y, lv, n_out, s, qi, part.cands);
      } else {
        corr_block<float>(*q.query, y, w.r);
        std::span<const float> sc = w.r;
        if (o.joint) {
          joint_score_into(w.r, lv, q.q_mean, q.m, w.score);
          sc = w.score;
        }
        for (auto i : local_peaks(sc, std::size_t(o.top_k), q.excl)) {
          Candidate c;
          c.grid = grid;
          c.start_utc = double(s) + double(i) + refine_lag(w.r, std::ptrdiff_t(i)) - q.first;
          c.r = w.r[i];
          c.score = sc[i];
          c.origin = static_cast<int>(qi);
          part.cands.push_back(std::move(c));
        }
      }
      part.n_lags = i64(n_out);
      std::size_t missing = 0;
      for (auto [ra, rb] : w.nan_runs) {
        const std::size_t lo = std::max(ra, off), hi = std::min(rb, off + n);
        missing += hi > lo ? hi - lo : 0;
      }
      part.finite_cnt = i64(n - missing);
      part.total_cnt = i64(n);
      part.from = double(s);
      part.to = double(e);
    }
  };

  // Blocks are independent: spread them over the cores, each thread taking the next
  // block not yet started.
  const int n_threads = std::max(1, std::min<int>(search_threads(o.threads), int(jobs.size())));
  std::atomic<std::size_t> next{0};
  std::exception_ptr err;
  std::mutex err_mu;
  auto worker = [&] {
    Buffers w;
    for (std::size_t ji; (ji = next.fetch_add(1)) < jobs.size();) {
      try {
        run_job(w, ji);
      } catch (...) {
        std::lock_guard lk(err_mu);
        if (!err) err = std::current_exception();
        next = jobs.size();
      }
    }
  };
  if (n_threads == 1) {
    worker();
  } else {
    std::vector<std::jthread> pool;
    for (int t = 0; t < n_threads; ++t) pool.emplace_back(worker);
  }
  if (err) std::rethrow_exception(err);
  for (std::size_t ji = 0; ji < jobs.size(); ++ji)
    for (std::size_t k = 0; k < jobs[ji].active.size(); ++k) {
      auto& q = qs[jobs[ji].active[k]];
      auto& part = parts[ji][k];
      q.cands.insert(q.cands.end(), std::make_move_iterator(part.cands.begin()), std::make_move_iterator(part.cands.end()));
      q.n_lags += part.n_lags;
      q.finite_cnt += part.finite_cnt;
      q.total_cnt += part.total_cnt;
      if (std::isnan(q.from)) q.from = part.from;
      q.to = part.to;
    }

  const double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - tic).count();
  std::vector<GridResult> out(qs.size());
  for (std::size_t i = 0; i < qs.size(); ++i) {
    auto& q = qs[i];
    auto& res = out[i];
    res.grid = grid;
    res.searched_from = q.from;
    res.searched_to = q.to;
    res.coverage = q.total_cnt ? double(q.finite_cnt) / double(q.total_cnt) : 0.0;
    res.n_lags = q.n_lags;
    res.seconds = secs;
    if (!q.error.empty()) {
      res.note = q.error;
      continue;
    }
    res.candidates = merge_topk(std::move(q.cands), std::size_t(o.top_k), q.excl);
    for (auto& c : res.candidates) describe(c, *q.tr, grid, q.hp);
    std::stable_partition(res.candidates.begin(), res.candidates.end(), [](auto& c) { return c.consistent(); });
    if (res.candidates.empty()) res.note = "no overlap with valid reference data";
  }
  fft::save_wisdom();
  return out;
}

GridResult search_grid(const ENFTrace& trace, const std::string& grid, const SearchOptions& o) {
  const ENFTrace* p = &trace;
  auto v = search_grid(std::span<const ENFTrace* const>(&p, 1), grid, o);
  if (!v[0].note.empty() && v[0].n_lags == 0 && v[0].candidates.empty() && v[0].note != "no overlap with valid reference data")
    throw std::invalid_argument(v[0].note);
  return std::move(v[0]);
}

std::vector<double> sliding_corr(std::span<const double> q, std::span<const double> y, double min_overlap) {
  Query Q(q, min_overlap);
  std::vector<float> r;
  corr_block<double>(Q, y, r);
  return {r.begin(), r.end()};
}

// ---------------------------------------------------------------- verdict

double false_match_probability(std::vector<double> scores, std::size_t k_max) {
  std::erase_if(scores, [](double x) { return !std::isfinite(x); });
  std::sort(scores.rbegin(), scores.rend());
  if (scores.size() > k_max) scores.resize(k_max);
  if (scores.size() < 6) return NAN;
  std::vector<double> d(scores.size() - 1);
  for (std::size_t i = 0; i + 1 < scores.size(); ++i) d[i] = scores[i] - scores[i + 1];
  double beta = 0;
  for (std::size_t j = 1; j < d.size(); ++j) beta += double(j + 1) * d[j];
  beta /= double(d.size() - 1);
  if (beta <= 0) return d[0] > 0 ? 0.0 : 1.0;
  return std::exp(-d[0] / beta);
}

const char* level_name(Level l) {
  switch (l) {
    case Level::Strong: return "strong";
    case Level::Moderate: return "moderate";
    case Level::Weak: return "weak";
    case Level::None: break;
  }
  return "none";
}

Level confidence_level(double p, double r) {
  if (!std::isfinite(p)) return r >= 0.8 ? Level::Weak : Level::None;
  if (p < 1e-4 && r >= 0.6) return Level::Strong;
  if (p < 1e-2 && r >= 0.5) return Level::Moderate;
  // r floor as for the stronger levels: without it a poor-shape match (r=0.40) became WEAK
  // once enough grids were pooled to make its lead look rare (a Middelfart livestream).
  if (p < 0.1 && r >= 0.5) return Level::Weak;
  return Level::None;
}

Verdict judge(const std::vector<GridResult>& results, double duration_s) {
  std::vector<const GridResult*> scored;
  for (auto& g : results)
    if (g.best()) scored.push_back(&g);
  Verdict v;
  if (scored.empty()) {
    v.reasons = {"no reference data overlapped the search window"};
    return v;
  }
  std::stable_sort(scored.begin(), scored.end(), [](auto* a, auto* b) { return a->best()->score > b->best()->score; });
  const auto* top = scored[0];
  const Candidate& c = *top->best();
  // The odds that a wrong time leads by this much come from the winning grid's own
  // wrong matches, then corrected for the number of grids searched: grids differ in how
  // high noise scores (a fast-wobbling small grid scores higher than a big one), so a
  // pool across grids made the leader of a high-scoring grid look rarer than it is.
  std::vector<double> own, pool;
  for (auto& cd : top->candidates) own.push_back(cd.score);
  std::size_t n_grids = 0;
  for (auto& g : results) {
    n_grids += g.n_lags > 0 || !g.candidates.empty();
    for (auto& cd : g.candidates) pool.push_back(cd.score);
  }
  double p = false_match_probability(own);
  if (std::isfinite(p)) p = 1 - std::pow(1 - p, double(std::max<std::size_t>(1, n_grids)));
  std::vector<double> sorted = pool;
  std::sort(sorted.rbegin(), sorted.rend());
  std::erase(sorted, c.score);  // the leader itself; the rest are wrong times or grids
  double runner = sorted.empty() ? NAN : sorted[0];
  v.reasons.push_back("best match in " + top->grid + ": score " + pyf(c.score, "%.1f") + ", wobble r=" + pyf(c.r, "%.3f") +
                      ", raw r=" + pyf(c.r_raw, "%.3f"));
  v.reasons.push_back("best wrong time/grid scored " + pyf(runner, "%.1f"));
  if (std::isfinite(p)) v.reasons.push_back("chance that a wrong match leads by this much: " + pyf(p, "%.1e"));
  if (scored.size() > 1) v.reasons.push_back("best other grid (" + scored[1]->grid + "): score " + pyf(scored[1]->best()->score, "%.1f"));
  if (std::isfinite(c.offset_hz) && std::fabs(c.offset_hz) > 0.02)
    v.reasons.push_back("large constant offset " + pyf(c.offset_hz * 1000, "%+.1f") + " mHz (recorder clock error or wrong match)");
  if (duration_s < 120) v.reasons.push_back("recording is under 2 minutes: matches are less reliable");
  v.level = confidence_level(p, c.r);
  v.p_false = p;
  if (v.level != Level::None) {
    v.grid = top->grid;
    v.candidate = c;
  }
  return v;
}

}  // namespace geo::match
