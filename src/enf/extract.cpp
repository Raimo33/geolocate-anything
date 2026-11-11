#include "enf/extract.hpp"

#include <algorithm>
#include <cmath>
#include <numeric>
#include <stdexcept>

#include "dsp/filters.hpp"
#include "util/parallel.hpp"

namespace geo::enf {

using fft::cd;

// ---------------------------------------------------------------- small helpers

std::vector<HarmonicStat> HumDetection::usable(std::size_t max_harmonics) const {
  std::vector<HarmonicStat> hs;
  if (!nominal) return hs;
  for (auto& h : harmonics)
    if (h.nominal == *nominal && h.snr_db >= min_snr_db) hs.push_back(h);
  std::stable_sort(hs.begin(), hs.end(), [](auto& a, auto& b) { return a.snr_db > b.snr_db; });
  if (hs.size() > max_harmonics) hs.resize(max_harmonics);
  return hs;
}

std::vector<double> ENFTrace::deviation() const {
  std::vector<double> d(freq.size());
  for (std::size_t i = 0; i < freq.size(); ++i) d[i] = freq[i] - nominal;
  return d;
}

double ENFTrace::valid_fraction() const {
  if (freq.empty()) return 0.0;
  std::size_t k = 0;
  for (double f : freq) k += std::isfinite(f);
  return double(k) / double(freq.size());
}

Spectrum Spectrum::of(std::span<const double> x, double fs) {
  double mean = 0;
  for (double v : x) mean += v;
  mean /= double(std::max<std::size_t>(1, x.size()));
  Spectrum s;
  s.X = fft::rfft_inplace(x.size(), [&](double* d) {
    for (std::size_t i = 0; i < x.size(); ++i) d[i] = x[i] - mean;
  });
  s.n = x.size();
  s.fs = fs;
  s.T = double(x.size()) / fs;
  return s;
}

std::optional<double> nyquist_limit(std::optional<int> source_rate) {
  if (!source_rate || *source_rate <= 0) return std::nullopt;
  return 0.45 * *source_rate;
}

std::vector<int> plausible_nominals(const HumDetection& d, double floor_db) {
  if (!d.nominal) return {};
  std::vector<int> out{*d.nominal};
  int other = *d.nominal == 50 ? 60 : 50;
  int n = 0;
  for (auto& h : d.harmonics) n += h.nominal == other && h.snr_db >= floor_db;
  if (n >= 2) out.push_back(other);
  return out;
}

// numpy.arange, value for value: numpy steps by delta = (a + step) - a, not by step,
// and the null-frequency spacing test (> 0.3 Hz) depends on that last bit.
static std::vector<double> arange(double a, double b, double step) {
  auto n = static_cast<std::size_t>(std::max(0.0, std::ceil((b - a) / step)));
  volatile double second = a + step;  // not folded to `step`, even under reassociation
  const double delta = second - a;
  std::vector<double> v(n);
  for (std::size_t i = 0; i < n; ++i) v[i] = a + double(i) * delta;
  return v;
}

// ---------------------------------------------------------------- detection

namespace {

struct Band {
  int nominal, harmonic;
  double centre;
  std::size_t lo, hi;          // index span covering centre and ring, inclusive lo, exclusive hi
  std::vector<std::size_t> centre_idx, ring_idx;
};

}  // namespace

HumDetection detect_hum(std::span<const double> x, double fs, const DetectOptions& o) { return detect_hum(x, fs, nullptr, o); }

HumDetection detect_hum(std::span<const double> x, double fs, const Spectrum* spec, const DetectOptions& o) {
  const std::size_t n = x.size();
  const std::size_t nper = std::min<std::size_t>(n, static_cast<std::size_t>(std::max(256.0, 16.0 * fs)));
  const std::size_t nb = nper / 2 + 1;
  auto freq = [&](std::size_t k) { return double(k) * fs / double(nper); };

  std::vector<Band> bands;
  for (int nom : NOMINALS) {
    for (int h = 1; h <= o.max_harmonic; ++h) {
      double c = double(nom) * h;
      if (c > fs / 2 - 5 || (o.max_hz && c > *o.max_hz)) break;
      double half = o.band_hz * h, outer = std::max(3.0, 3 * half);
      Band b{nom, h, c, nb, 0, {}, {}};
      for (std::size_t k = 0; k < nb; ++k) {
        double f = freq(k), d = std::fabs(f - c);
        bool cen = f >= c - half && f <= c + half;
        bool ring = d > half + 0.25 && d <= outer;
        if (cen) b.centre_idx.push_back(k);
        if (ring) b.ring_idx.push_back(k);
        if (cen || ring) {
          b.lo = std::min(b.lo, k);
          b.hi = std::max(b.hi, k + 1);
        }
      }
      if (b.centre_idx.empty() || b.ring_idx.size() < 4) continue;
      bands.push_back(std::move(b));
    }
  }

  // Whitened PSD: every segment's band divided by that segment's own floor (median of
  // the ring), averaged over segments that are not silent.
  std::vector<double> psd(nb, 0.0);
  {
    const std::size_t step = std::max<std::size_t>(1, nper / 2);
    auto win = dsp::hanning(nper);
    fft::RealPlan plan(static_cast<int>(nper));
    fft::Buf<double> in(nper);
    fft::Buf<cd> out(nb);
    std::vector<std::vector<double>> vals(bands.size());   // per band: segments x span
    std::vector<std::vector<double>> floors(bands.size());
    std::vector<double> ring;
    std::vector<double> p(nb);
    for (std::size_t s = 0; s + nper <= n; s += step) {
      double mean = 0;
      for (std::size_t i = 0; i < nper; ++i) mean += x[s + i];
      mean /= double(nper);
      for (std::size_t i = 0; i < nper; ++i) in[i] = (x[s + i] - mean) * win[i];
      plan.forward(in.data(), out.data());
      for (std::size_t bi = 0; bi < bands.size(); ++bi) {
        auto& b = bands[bi];
        for (std::size_t k = b.lo; k < b.hi; ++k) p[k] = std::norm(out[k]);
        ring.clear();
        for (auto k : b.ring_idx) ring.push_back(p[k]);
        floors[bi].push_back(dsp::median_inplace(ring));
        vals[bi].insert(vals[bi].end(), p.begin() + b.lo, p.begin() + b.hi);
      }
    }
    for (std::size_t bi = 0; bi < bands.size(); ++bi) {
      auto& b = bands[bi];
      auto& fl = floors[bi];
      if (fl.empty()) continue;
      double med = dsp::median(fl);
      const std::size_t w = b.hi - b.lo;
      std::vector<double> acc(w, 0.0);
      std::size_t live = 0;
      for (std::size_t sgi = 0; sgi < fl.size(); ++sgi) {
        if (!(fl[sgi] > 1e-6 * med + 1e-30)) continue;
        ++live;
        for (std::size_t j = 0; j < w; ++j) acc[j] += vals[bi][sgi * w + j] / fl[sgi];
      }
      if (!live) continue;
      // Only the band's own bins (centre and ring) are written, as numpy's out[sel] = ...
      auto put = [&](std::size_t k) { psd[k] = acc[k - b.lo] / double(live); };
      for (auto k : b.centre_idx) put(k);
      for (auto k : b.ring_idx) put(k);
    }
  }

  HumDetection det;
  det.min_snr_db = o.min_snr_db;
  std::vector<double> ring;
  for (auto& b : bands) {
    std::size_t ipk = b.centre_idx[0];
    for (auto k : b.centre_idx)
      if (psd[k] > psd[ipk]) ipk = k;
    ring.clear();
    for (auto k : b.ring_idx) ring.push_back(psd[k]);
    double noise = dsp::median_inplace(ring) + 1e-30;
    double snr = 10 * std::log10(psd[ipk] / noise + 1e-30);
    det.harmonics.push_back({b.nominal, b.harmonic, b.centre, freq(ipk), snr, std::fmod(b.centre, 300.0) == 0});
  }
  for (int nom : NOMINALS) {
    std::vector<double> own;
    for (auto& s : det.harmonics)
      if (s.nominal == nom && !s.shared) own.push_back(s.snr_db);
    std::sort(own.rbegin(), own.rend());
    double sc = 0;
    for (std::size_t i = 0; i < std::min<std::size_t>(3, own.size()); ++i) sc += std::max(own[i], 0.0);
    det.score_db[nom] = sc;
  }
  int best = det.score_db[50] >= det.score_db[60] ? 50 : 60;   // first max wins
  int other = det.score_db[50] <= det.score_db[60] ? 50 : 60;  // first min wins
  double best_own = -INFINITY;
  for (auto& s : det.harmonics)
    if (s.nominal == best && !s.shared) best_own = std::max(best_own, s.snr_db);
  if (best_own >= o.detect_snr_db && det.score_db[best] > det.score_db[other]) det.nominal = best;
  if (!det.nominal && o.ridge) {
    det.ridge_log10p = ridge_detect(x, fs, det.harmonics, spec, o.band_hz);
    if (!det.ridge_log10p.empty()) {
      auto top = det.ridge_log10p[0];
      double mn = top.second;
      for (auto& kv : det.ridge_log10p) {
        if (kv.second > top.second) top = kv;
        mn = std::min(mn, kv.second);
      }
      if (top.second >= RIDGE_LOG10P && top.second > mn) det.nominal = top.first;
    }
  }
  return det;
}

// ---------------------------------------------------------------- spectrogram

namespace {

struct Baseband {
  std::vector<cd> z;
  double fs_d, fc;
};

Baseband baseband(const Spectrum& s, double centre_hz, double half_bw, bool hann_band = false) {
  const long k0 = std::lrint(centre_hz * s.T);
  const long K = std::lrint(half_bw * s.T);
  const long lo = k0 - K, hi = k0 + K + 1;
  if (lo < 1 || hi > static_cast<long>(s.X.size())) throw std::invalid_argument("harmonic band outside the spectrum");
  const std::size_t M = static_cast<std::size_t>(hi - lo);
  std::vector<cd> z(M);
  std::vector<double> w;
  if (hann_band) w = dsp::hanning(M);
  for (std::size_t i = 0; i < M; ++i) {
    std::size_t src = (i + static_cast<std::size_t>(K)) % M;  // ifftshift
    z[i] = s.X[static_cast<std::size_t>(lo) + src] * (hann_band ? w[src] : 1.0);
  }
  fft::backward_inplace(z);
  return {std::move(z), double(M) / s.T, double(k0) / s.T};
}

}  // namespace

std::vector<float> harmonic_spectrogram(const Spectrum& s, double nominal, int h, std::span<const double> offsets, double frame_s,
                                        double hop_s, std::size_t n_frames, double band_hz, int n_noise) {
  const double lobe = 2.0 / frame_s;
  const double half_bw = band_hz * h + lobe + 1.0;
  auto bb = baseband(s, h * nominal, half_bw);
  const double fs_d = bb.fs_d;
  const std::size_t flen = static_cast<std::size_t>(std::max(8L, std::lrint(frame_s * fs_d)));
  const std::size_t K = offsets.size(), half = static_cast<std::size_t>(n_noise / 2), Kt = K + 2 * half;
  const double inner = band_hz * h + lobe, outer = half_bw - 0.2;
  const double hi_side = std::max(inner + 0.1, outer);
  std::vector<double> nu(Kt);
  for (std::size_t k = 0; k < K; ++k) nu[k] = h * (nominal + offsets[k]) - bb.fc;
  for (std::size_t j = 0; j < half; ++j) {
    double sd = half > 1 ? inner + (hi_side - inner) * double(j) / double(half - 1) : inner;
    nu[K + j] = h * nominal - bb.fc - sd;
    nu[K + half + j] = h * nominal - bb.fc + sd;
  }
  // E[n, k] = exp(-2 pi i n nu_k / fs_d), split real/imaginary for vectorisation.
  std::vector<double> er(flen * Kt), ei(flen * Kt);
  for (std::size_t n = 0; n < flen; ++n)
    for (std::size_t k = 0; k < Kt; ++k) {
      double a = -2.0 * M_PI * double(n) * nu[k] / fs_d;
      er[n * Kt + k] = std::cos(a);
      ei[n * Kt + k] = std::sin(a);
    }
  auto win = dsp::hanning(flen);
  std::vector<float> out(n_frames * K);
  std::vector<double> ar(Kt), ai(Kt), fr(flen), fi(flen), noise(2 * half);
  const long zmax = static_cast<long>(bb.z.size()) - static_cast<long>(flen);
  for (std::size_t t = 0; t < n_frames; ++t) {
    long st = std::min(std::lrint(double(t) * hop_s * fs_d), zmax);
    for (std::size_t n = 0; n < flen; ++n) {
      cd v = bb.z[static_cast<std::size_t>(st) + n] * win[n];
      fr[n] = v.real();
      fi[n] = v.imag();
    }
    std::fill(ar.begin(), ar.end(), 0.0);
    std::fill(ai.begin(), ai.end(), 0.0);
    for (std::size_t n = 0; n < flen; ++n) {
      const double a = fr[n], b = fi[n];
      const double* __restrict r = er.data() + n * Kt;
      const double* __restrict im = ei.data() + n * Kt;
      double* __restrict xr = ar.data();
      double* __restrict xi = ai.data();
      for (std::size_t k = 0; k < Kt; ++k) {
        xr[k] += a * r[k] - b * im[k];
        xi[k] += a * im[k] + b * r[k];
      }
    }
    for (std::size_t j = 0; j < 2 * half; ++j) noise[j] = ar[K + j] * ar[K + j] + ai[K + j] * ai[K + j];
    double nf = std::max(dsp::median_inplace(noise) / std::log(2.0), 1e-300);
    float* o = out.data() + t * K;
    for (std::size_t k = 0; k < K; ++k) o[k] = static_cast<float>((ar[k] * ar[k] + ai[k] * ai[k]) / nf);
  }
  return out;
}

// ---------------------------------------------------------------- Viterbi

template <class T>
static std::vector<int> viterbi_impl(std::span<const T> score, std::size_t K, double step_hz, double lam, double max_jump_hz) {
  const std::size_t Tn = K ? score.size() / K : 0;
  std::vector<int> path(Tn, 0);
  if (Tn == 0) return path;
  const int J = std::max(1, static_cast<int>(std::lrint(max_jump_hz / step_hz)));
  std::vector<double> pen(2 * J + 1);
  for (int j = -J; j <= J; ++j) {
    double d = j * step_hz * 1000.0;
    pen[j + J] = lam * d * d;
  }
  std::vector<std::int16_t> back(Tn * K);
  std::vector<double> D(K), best(K);
  std::vector<int> bj(K);
  for (std::size_t k = 0; k < K; ++k) D[k] = score[k];
  const long Kl = static_cast<long>(K);
  for (std::size_t t = 1; t < Tn; ++t) {
    std::fill(best.begin(), best.end(), -INFINITY);
    std::fill(bj.begin(), bj.end(), 0);
    for (int j = 0; j <= 2 * J; ++j) {
      const long sh = j - J;  // predecessor k + sh
      const long k0 = std::max(0L, -sh), k1 = std::min(Kl, Kl - sh);
      const double pj = pen[j];
      const double* __restrict Dp = D.data() + sh;
      double* __restrict b = best.data();
      int* __restrict bjp = bj.data();
      for (long k = k0; k < k1; ++k) {
        double v = Dp[k] - pj;
        bool up = v > b[k];
        b[k] = up ? v : b[k];
        bjp[k] = up ? j : bjp[k];
      }
    }
    const T* sc = score.data() + t * K;
    std::int16_t* bk = back.data() + t * K;
    for (std::size_t k = 0; k < K; ++k) {
      D[k] = best[k] + double(sc[k]);
      bk[k] = static_cast<std::int16_t>(bj[k] - J);
    }
  }
  path[Tn - 1] = static_cast<int>(std::max_element(D.begin(), D.end()) - D.begin());
  for (std::size_t t = Tn - 1; t > 0; --t) path[t - 1] = path[t] + back[t * K + static_cast<std::size_t>(path[t])];
  return path;
}

std::vector<int> viterbi_ridge(std::span<const float> score, std::size_t K, double step_hz, double lam, double max_jump_hz) {
  return viterbi_impl(score, K, step_hz, lam, max_jump_hz);
}
static std::vector<int> viterbi_ridge_d(std::span<const double> score, std::size_t K, double step_hz, double lam, double max_jump_hz) {
  return viterbi_impl(score, K, step_hz, lam, max_jump_hz);
}

// ---------------------------------------------------------------- consensus

static double dot(const std::vector<double>& a, const std::vector<double>& b) {
  double s = 0;
  for (std::size_t i = 0; i < a.size(); ++i) s += a[i] * b[i];
  return s;
}

Consensus consensus_harmonics(const std::map<int, std::vector<double>>& tracks, double smooth_frames, double min_r, int hp_frames,
                              const std::vector<int>& lenient, double admit_r) {
  std::vector<int> keys;
  for (auto& kv : tracks) keys.push_back(kv.first);
  const int k = std::max(1, static_cast<int>(std::lrint(smooth_frames)));
  std::map<int, std::vector<double>> sm;
  std::map<int, double> level;
  for (int h : keys) {
    const auto& tr = tracks.at(h);
    std::vector<double> v;
    if (tr.size() >= std::size_t(k)) {
      v.resize(tr.size() - std::size_t(k) + 1);
      for (std::size_t i = 0; i < v.size(); ++i) {
        double s = 0;
        for (int j = 0; j < k; ++j) s += tr[i + std::size_t(j)] * (1.0 / k);
        v[i] = s;
      }
    }
    if (v.size() > std::size_t(2 * hp_frames)) {
      const std::size_t L = v.size(), hh = std::size_t(hp_frames / 2);
      std::vector<double> u(L - std::size_t(hp_frames) + 1);
      for (std::size_t i = 0; i < u.size(); ++i) {
        double s = 0;
        for (int j = 0; j < hp_frames; ++j) s += v[i + std::size_t(j)] * (1.0 / hp_frames);
        u[i] = v[i + hh] - s;
      }
      v = std::move(u);
    }
    double mean = v.empty() ? 0 : std::accumulate(v.begin(), v.end(), 0.0) / double(v.size());
    for (auto& x : v) x -= mean;
    sm[h] = std::move(v);
    level[h] = std::fabs(dsp::median(tr));
  }
  std::map<std::pair<int, int>, double> r;
  for (std::size_t i = 0; i < keys.size(); ++i)
    for (std::size_t j = i + 1; j < keys.size(); ++j) {
      auto &a = sm[keys[i]], &b = sm[keys[j]];
      double den = std::sqrt(dot(a, a)) * std::sqrt(dot(b, b));
      r[{keys[i], keys[j]}] = den > 0 ? dot(a, b) / den : 0.0;
    }
  auto best_group = [&](const std::vector<int>& avail) {
    // key (size, -round(mean level / 0.05), mean r), larger is better; first wins ties.
    bool have = false;
    std::tuple<std::size_t, long, double> best{0, 0, 0};
    std::vector<int> out;
    const std::size_t n = avail.size();
    for (std::uint32_t mask = 1; mask < (1u << n); ++mask) {
      std::vector<int> grp;
      for (std::size_t i = 0; i < n; ++i)
        if (mask >> i & 1) grp.push_back(avail[i]);
      if (grp.size() < 2) continue;
      double mn = INFINITY, sum = 0;
      std::size_t cnt = 0;
      for (std::size_t i = 0; i < grp.size(); ++i)
        for (std::size_t j = i + 1; j < grp.size(); ++j) {
          double v = r[{grp[i], grp[j]}];
          mn = std::min(mn, v);
          sum += v;
          ++cnt;
        }
      if (mn < min_r) continue;
      double lv = 0;
      for (int h : grp) lv += level[h];
      lv /= double(grp.size());
      std::tuple<std::size_t, long, double> key{grp.size(), -std::lrint(lv / 0.05), sum / double(cnt)};
      if (!have || key > best) {
        have = true;
        best = key;
        out = grp;
      }
    }
    return out;
  };
  Consensus c;
  std::vector<int> avail = keys;
  while (avail.size() >= 2) {
    auto g = best_group(avail);
    if (g.empty()) break;
    c.groups.push_back(g);
    std::vector<int> rest;
    for (int h : avail)
      if (std::find(g.begin(), g.end(), h) == g.end()) rest.push_back(h);
    avail = std::move(rest);
  }
  if (!c.groups.empty() && !lenient.empty()) {
    const auto& g0 = c.groups[0];
    std::vector<double> cons(sm[g0[0]].size(), 0.0);
    for (int h : g0)
      for (std::size_t i = 0; i < cons.size(); ++i) cons[i] += sm[h][i];
    for (auto& x : cons) x /= double(g0.size());
    std::vector<int> cand;
    for (int h : lenient)
      if (std::find(avail.begin(), avail.end(), h) != avail.end()) cand.push_back(h);
    std::sort(cand.begin(), cand.end());
    cand.erase(std::unique(cand.begin(), cand.end()), cand.end());
    for (int h : cand) {
      double den = std::sqrt(dot(sm[h], sm[h])) * std::sqrt(dot(cons, cons));
      if (den > 0 && dot(sm[h], cons) / den >= admit_r) c.groups[0].push_back(h);
    }
  }
  return c;
}

// ---------------------------------------------------------------- ridge detection

std::vector<double> null_nominals(const std::vector<int>& harmonics, double band_hz, double frame_s, double lo, double hi, std::size_t n) {
  std::vector<double> out;
  for (double m : arange(lo, hi, 0.05)) {
    bool ok = true;
    for (int h : harmonics) {
      double reach = band_hz * h + 2.0 / frame_s + 1.5;
      double c = h * m;
      double d50 = std::fabs(c - 50 * std::nearbyint(c / 50)), d60 = std::fabs(c - 60 * std::nearbyint(c / 60));
      if (std::min(d50, d60) <= reach) {
        ok = false;
        break;
      }
    }
    if (ok && std::all_of(out.begin(), out.end(), [&](double k) { return std::fabs(m - k) > 0.3; })) out.push_back(m);
  }
  if (out.size() > n) {
    std::vector<double> pick;
    const double step = double(out.size() - 1) / double(n - 1);  // numpy.linspace(0, len - 1, n).round()
    for (std::size_t i = 0; i < n; ++i) {
      double pos = i + 1 == n ? double(out.size() - 1) : double(i) * step;
      pick.push_back(out[static_cast<std::size_t>(std::nearbyint(pos))]);
    }
    out = std::move(pick);
  }
  return out;
}

static double ridge_evidence(const Spectrum& s, double nominal, const std::vector<int>& harmonics, double frame_s, double hop_s,
                             std::size_t n_frames, double band_hz, double step_hz, std::size_t max_k) {
  auto offsets = arange(-band_hz, band_hz + step_hz / 2, step_hz);
  const std::size_t K = offsets.size();
  const double drift = 2.0 * std::sqrt(hop_s);
  const double lam = 1.0 / (2.0 * drift * drift), jump = 0.02 * hop_s;
  std::vector<std::vector<float>> Qs;
  std::vector<double> own;
  for (int h : harmonics) {
    std::vector<float> Q;
    try {
      Q = harmonic_spectrogram(s, nominal, h, offsets, frame_s, hop_s, n_frames, band_hz);
    } catch (const std::invalid_argument&) {
      continue;
    }
    for (auto& q : Q) q -= 1.0f;
    auto path = viterbi_ridge(Q, K, step_hz, lam, jump);
    double sum = 0;
    for (std::size_t t = 0; t < n_frames; ++t) sum += Q[t * K + std::size_t(path[t])];
    own.push_back(sum);
    Qs.push_back(std::move(Q));
  }
  std::vector<std::size_t> order(own.size());
  std::iota(order.begin(), order.end(), 0);
  std::stable_sort(order.begin(), order.end(), [&](auto a, auto b) { return own[a] > own[b]; });
  double best = -INFINITY;
  std::vector<float> acc(n_frames * K, 0.0f);
  for (std::size_t k = 1; k <= std::min(max_k, order.size()); ++k) {
    const auto& Q = Qs[order[k - 1]];
    for (std::size_t i = 0; i < acc.size(); ++i) acc[i] += Q[i];
    auto path = viterbi_ridge(acc, K, step_hz, lam, jump);
    double sum = 0;
    for (std::size_t t = 0; t < n_frames; ++t) sum += acc[t * K + std::size_t(path[t])];
    best = std::max(best, sum / std::sqrt(double(k * n_frames)));
  }
  return best;
}

std::vector<std::pair<int, double>> ridge_detect(std::span<const double> x, double fs, const std::vector<HarmonicStat>& stats,
                                                 const Spectrum* spec, double band_hz) {
  const double frame_s = DEFAULT_FRAME_S, step_hz = 0.005;
  const std::size_t max_k = 4, max_frames = 900;
  double hop_s = 2.0;
  const double T = double(x.size()) / fs;
  if (T < std::max(RIDGE_MIN_S, frame_s + 4 * hop_s)) return {};
  hop_s = std::max(hop_s, (T - frame_s) / double(max_frames));
  const auto n_frames = static_cast<std::size_t>(std::floor((T - frame_s) / hop_s)) + 1;
  std::map<int, std::vector<int>> fams;
  std::vector<int> hs;
  for (int nom : NOMINALS) {
    auto& f = fams[nom];
    for (auto& s : stats)
      if (s.nominal == nom && !s.shared) f.push_back(s.harmonic);
    std::sort(f.begin(), f.end());
    hs.insert(hs.end(), f.begin(), f.end());
  }
  std::sort(hs.begin(), hs.end());
  hs.erase(std::unique(hs.begin(), hs.end()), hs.end());
  if (hs.empty()) return {};
  const double top = fs / 2 - 5;
  const int hmax = hs.back();
  std::vector<double> nulls;
  for (double m : null_nominals(hs, band_hz, frame_s))
    if (hmax * m + band_hz * hmax + 4 < top) nulls.push_back(m);
  if (nulls.size() < 8) return {};
  std::optional<Spectrum> own;
  if (!spec) own = Spectrum::of(x, fs);
  const Spectrum& S = spec ? *spec : *own;
  std::vector<std::pair<int, double>> out;
  for (int nom : NOMINALS) {
    const auto& fh = fams[nom];
    if (fh.empty()) continue;
    // The family's own score and its nulls' are independent: one per thread.
    std::vector<double> ev(nulls.size() + 1);
    parallel_for(ev.size(), [&](std::size_t i) {
      ev[i] = ridge_evidence(S, i ? nulls[i - 1] : double(nom), fh, frame_s, hop_s, n_frames, band_hz, step_hz, max_k);
    });
    const double sc = ev[0];
    std::vector<double> null(ev.begin() + 1, ev.end());
    double mean = std::accumulate(null.begin(), null.end(), 0.0) / double(null.size());
    double var = 0;
    for (double v : null) var += (v - mean) * (v - mean);
    double sd = std::sqrt(var / double(null.size()));
    double beta = std::max(sd, 1e-9) * std::sqrt(6.0) / M_PI;
    double mu = mean - 0.5772 * beta;
    double p = -std::expm1(-std::exp(-(sc - mu) / beta));
    out.emplace_back(nom, -std::log10(std::max(p, 1e-300)));
  }
  return out;
}

// ---------------------------------------------------------------- extraction

ENFTrace extract_enf(const Spectrum& spec, const HumDetection& det, const ExtractOptions& o) {
  const double T = spec.T;
  char msg[128];
  if (T < o.frame_s + o.hop_s) {
    std::snprintf(msg, sizeof msg, "recording too short (%.1fs) for %gs frames", T, o.frame_s);
    throw std::invalid_argument(msg);
  }
  std::optional<int> nominal = o.nominal ? o.nominal : det.nominal;
  if (!nominal) throw std::invalid_argument("no mains hum detected (neither 50 Hz nor 60 Hz family stands out)");
  const int nom = *nominal;

  std::vector<HarmonicStat> hs;
  for (auto& h : det.harmonics)
    if (h.nominal == nom) hs.push_back(h);
  std::stable_sort(hs.begin(), hs.end(), [](auto& a, auto& b) { return a.snr_db > b.snr_db; });
  bool consensus = o.consensus;
  if (o.harmonics) {
    std::vector<HarmonicStat> keep;
    for (auto& h : hs)
      if (std::find(o.harmonics->begin(), o.harmonics->end(), h.harmonic) != o.harmonics->end()) keep.push_back(h);
    hs = std::move(keep);
    consensus = false;
  }
  std::vector<HarmonicStat> above;
  for (auto& h : hs)
    if (h.snr_db >= o.min_snr_db) above.push_back(h);
  auto head = [](const std::vector<HarmonicStat>& v, std::size_t n) {
    return std::vector<HarmonicStat>(v.begin(), v.begin() + std::min(n, v.size()));
  };
  std::vector<HarmonicStat> pool;
  if (o.harmonics) {
    pool = hs;
  } else if (!consensus) {
    pool = head(above, o.max_harmonics);
    if (pool.empty()) pool = head(hs, 2);
  } else {
    double floor = std::min(o.pool_snr_db, o.min_snr_db);
    for (auto& h : hs)
      if (h.snr_db >= floor) pool.push_back(h);
    pool = head(pool, 8);
    if (pool.empty()) pool = head(hs, 2);
  }

  const auto n_frames = static_cast<std::size_t>(std::floor((T - o.frame_s) / o.hop_s)) + 1;
  auto offsets = arange(-o.band_hz, o.band_hz + o.step_hz / 2, o.step_hz);
  const std::size_t K = offsets.size();
  const double lam = 1.0 / (2.0 * o.drift_mhz * o.drift_mhz);
  auto spectrogram = [&](int h) {
    return harmonic_spectrogram(spec, nom, h, offsets, o.frame_s, o.hop_s, n_frames, o.band_hz);
  };

  std::vector<int> chosen;
  ENFTrace tr;
  if (consensus && pool.size() >= 2) {
    // Track each harmonic on its own; keep the largest group whose wander agrees. The
    // spectrograms are recomputed for the chosen ones below rather than all kept.
    std::map<int, std::vector<double>> tracks;
    for (auto& h : pool) {
      auto Q = spectrogram(h.harmonic);
      for (auto& q : Q) q = static_cast<float>(o.evidence_scale * (double(q) - 1.0));
      auto path = viterbi_ridge(Q, K, o.step_hz, lam, o.max_jump_hz);
      std::vector<double> t(n_frames);
      for (std::size_t i = 0; i < n_frames; ++i) t[i] = offsets[std::size_t(path[i])];
      tracks[h.harmonic] = std::move(t);
    }
    std::vector<int> lenient;
    for (auto& h : head(above, o.max_harmonics)) lenient.push_back(h.harmonic);
    auto c = consensus_harmonics(tracks, o.frame_s / o.hop_s, o.consensus_r, 61, lenient);
    if (!c.groups.empty()) {
      chosen = c.groups[0];
      tr.alternatives.assign(c.groups.begin() + 1, c.groups.end());
    } else {
      auto fb = head(above, o.max_harmonics);
      if (fb.empty()) fb = head(pool, 2);
      for (auto& h : fb) chosen.push_back(h.harmonic);
    }
  } else {
    for (auto& h : pool) chosen.push_back(h.harmonic);
  }
  std::vector<HarmonicStat> usable;
  for (auto& h : pool)
    if (std::find(chosen.begin(), chosen.end(), h.harmonic) != chosen.end()) usable.push_back(h);

  std::vector<double> combined(n_frames * K, 0.0);
  double wsum = 0;
  for (auto& h : usable) {
    auto Q = spectrogram(h.harmonic);
    const double w = o.snr_weighting ? std::pow(10.0, std::max(h.snr_db, 0.0) / 10) : 1.0;
    for (std::size_t i = 0; i < combined.size(); ++i) combined[i] += w * Q[i];
    wsum += w;
  }
  for (auto& v : combined) v /= wsum;

  std::vector<double> llr(combined.size());
  const double sc = o.evidence_scale * double(usable.size());
  for (std::size_t i = 0; i < llr.size(); ++i) llr[i] = sc * (combined[i] - 1.0);
  auto path = viterbi_ridge_d(llr, K, o.step_hz, lam, o.max_jump_hz);

  tr.nominal = nom;
  tr.frame_s = o.frame_s;
  tr.hop_s = o.hop_s;
  for (auto& h : usable) tr.harmonics.push_back(h.harmonic);
  tr.times.resize(n_frames);
  tr.freq.resize(n_frames);
  tr.snr_db.resize(n_frames);
  auto logc = [&](std::size_t t, std::size_t k) { return std::log(std::max(combined[t * K + k], 1e-12)); };
  for (std::size_t t = 0; t < n_frames; ++t) {
    const auto p = static_cast<std::size_t>(path[t]);
    double d = 0;
    if (p > 0 && p < K - 1) d = dsp::parabolic(logc(t, p - 1), logc(t, p), logc(t, p + 1));
    double f = nom + offsets[p] + d * o.step_hz;
    double snr = 10 * std::log10(std::max(combined[t * K + p], 1e-12));
    tr.times[t] = o.frame_s / 2 + double(t) * o.hop_s;
    tr.snr_db[t] = snr;
    tr.freq[t] = snr >= o.min_frame_snr_db ? f : NAN;
  }
  return tr;
}

}  // namespace geo::enf
