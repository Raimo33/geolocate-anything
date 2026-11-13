#include "air/events.hpp"

#include <algorithm>
#include <cmath>
#include <mutex>

#include "air/doppler.hpp"
#include "util/parallel.hpp"

namespace geo::air {

namespace {

struct Template {
  double v, d, hw;
  int m0;                // first pooled-frame offset relative to the CPA frame
  std::vector<int> sh;   // log-bin shift for each offset m0 .. m0 + size - 1
  int lo, hi;            // min / max shift
};

double median(std::vector<double> v) {
  if (v.empty()) return NAN;
  auto mid = v.begin() + std::ptrdiff_t(v.size() / 2);
  std::nth_element(v.begin(), mid, v.end());
  return *mid;
}

double half_window(double v, double d) { return std::clamp(3.0 * d / v, 8.0, 40.0); }

}  // namespace

double cv_score(const Spectrogram& s, double tc, double v, double d, double f0, double hw) {
  const int j0 = std::max(0, int(std::ceil(s.frame_at(tc - hw)))), j1 = std::min(s.nf - 1, int(std::floor(s.frame_at(tc + hw))));
  double sum = 0;
  int n = 0;
  for (int j = j0; j <= j1; ++j) {
    const float z = s.at(j, f0 * cv_ratio(s.frame_time(j), tc, v, d));
    if (std::isfinite(z)) sum += z, ++n;
  }
  return n ? sum / std::sqrt(double(n)) : -INFINITY;
}

Detection detect_events(const Spectrogram& s, const EventOptions& o) {
  Detection res;
  std::vector<Event>& out = res.events;
  if (s.nf < 16) return res;
  // 1. Pooled log-frequency map: P frames per step, linear interpolation in frequency;
  //    pooled values are scaled by sqrt(P) so noise stays ~unit variance.
  const int P = std::max(1, int(std::lround(o.pool_s * s.rate / s.hop)));
  const double dt = double(P) * s.hop / s.rate;
  const int NT = s.nf / P;
  const double fmax = std::min(o.fmax * 1.35, s.fmax());  // room for the approaching side
  const double fmin = std::max(o.fmin / 1.35, s.freq(0));
  const int K = int(std::floor(std::log(fmax / fmin) / o.logf_step)) + 1;
  std::vector<float> L(static_cast<std::size_t>(NT) * static_cast<std::size_t>(K));
  parallel_for(std::size_t(NT), [&](std::size_t t) {
    float* row = L.data() + t * std::size_t(K);
    for (int k = 0; k < K; ++k) {
      const double f = fmin * std::exp(k * o.logf_step);
      double acc = 0;
      for (int p = 0; p < P; ++p) acc += s.at(int(t) * P + p, f);
      row[k] = float(acc / std::sqrt(double(P)));
    }
  });
  auto t_of = [&](int t) { return s.frame_time(t * P + 0.5 * (P - 1)); };

  // 2. Templates over (v, d): log-bin shift of the curve at each pooled offset.
  std::vector<Template> tmpl;
  for (double v = o.vmin; v <= o.vmax + 1e-9; v += 10.0)
    for (double d = o.dmin; d <= o.dmax * 1.0001; d *= 1.4) {
      Template T{v, d, half_window(v, d), 0, {}, 0, 0};
      const int M = int(std::ceil(T.hw / dt));
      T.m0 = -M;
      double smin = INFINITY, smax = -INFINITY;
      for (int m = -M; m <= M; ++m) {
        const double lr = std::log(cv_ratio(m * dt, 0.0, v, d));
        T.sh.push_back(int(std::lround(lr / o.logf_step)));
        smin = std::min(smin, lr), smax = std::max(smax, lr);
      }
      if (smax - smin < o.min_swing) continue;
      T.lo = *std::min_element(T.sh.begin(), T.sh.end());
      T.hi = *std::max_element(T.sh.begin(), T.sh.end());
      tmpl.push_back(std::move(T));
    }
  // Rest-frequency bins searched; harmonic comb offsets (x2, x3).
  const int kf0 = int(std::ceil(std::log(o.fmin / fmin) / o.logf_step));
  const int kf1 = int(std::floor(std::log(o.fmax / fmin) / o.logf_step));
  const int h2 = int(std::lround(std::log(2.0) / o.logf_step)), h3 = int(std::lround(std::log(3.0) / o.logf_step));

  struct Best {
    float score = -INFINITY;
    int tmpl = -1, k = 0;
  };
  std::vector<Best> best(static_cast<std::size_t>(NT));
  auto in_windows = [&](double t) {
    if (o.windows.empty()) return true;
    for (auto& [a, b] : o.windows)
      if (t >= a && t <= b) return true;
    return false;
  };
  parallel_for(std::size_t(NT), [&](std::size_t tu) {
    const int t = int(tu);
    if (!in_windows(t_of(t))) return;
    std::vector<float> acc(static_cast<std::size_t>(K));
    Best b;
    for (std::size_t ti = 0; ti < tmpl.size(); ++ti) {
      const Template& T = tmpl[ti];
      std::fill(acc.begin(), acc.end(), 0.0f);
      int n = 0;
      // acc[k] = sum_m L[t+m][k + sh[m]] for k with every shifted index inside [0, K).
      const int ka = std::max(0, -T.lo), kb = std::min(K, K - T.hi);
      if (kb <= ka) continue;
      for (std::size_t i = 0; i < T.sh.size(); ++i) {
        const int tt = t + T.m0 + int(i);
        if (tt < 0 || tt >= NT) continue;
        const float* row = L.data() + std::size_t(tt) * std::size_t(K) + T.sh[i];
        for (int k = ka; k < kb; ++k) acc[std::size_t(k)] += row[k];
        ++n;
      }
      if (n < int(T.sh.size()) / 2) continue;
      const float norm = 1.0f / std::sqrt(float(n));
      for (int k = std::max(ka, kf0); k <= std::min(kb - 1, kf1); ++k) {
        float sc = acc[std::size_t(k)] * norm;
        if (k + h3 < kb) {  // comb of three harmonics with k the fundamental
          const float comb = (acc[std::size_t(k)] + acc[std::size_t(k + h2)] + acc[std::size_t(k + h3)]) * norm / std::sqrt(3.0f);
          sc = std::max(sc, comb);
        }
        if (sc > b.score) b = {sc, int(ti), k};
      }
    }
    best[tu] = b;
  });

  // 3. Threshold from the recording's own distribution: most instants hold no flyover,
  //    so the median and MAD of the per-instant best describe the noise maximum.
  std::vector<double> sc;
  for (auto& b : best)
    if (std::isfinite(b.score)) sc.push_back(b.score);
  if (sc.size() < 8) return res;
  const double med = median(sc);
  std::vector<double> dev;
  for (double x : sc) dev.push_back(std::abs(x - med));
  const double mad = 1.4826 * median(dev);
  // The per-instant best is a maximum over many templates and frequencies, and clutter
  // (boats, cars, birds) fattens its upper tail: k = 6 robust deviations keeps clear
  // flyovers and some clutter, which the search ignores when no aircraft explains it.
  const double thr = o.threshold > 0 ? o.threshold : std::max(med + 6.0 * mad, 6.0);
  res.threshold = thr, res.noise_median = med, res.noise_mad = mad;
  std::vector<int> order;
  for (int t = 0; t < NT; ++t)
    if (best[std::size_t(t)].score >= thr) order.push_back(t);
  std::sort(order.begin(), order.end(), [&](int a, int b) { return best[std::size_t(a)].score > best[std::size_t(b)].score; });
  std::vector<int> peaks;
  for (int t : order) {
    bool near = false;
    for (int p : peaks) near |= std::abs(t_of(t) - t_of(p)) < o.min_separation_s;
    if (!near) peaks.push_back(t);
    if (int(peaks.size()) >= o.max_events) break;
  }

  // 4. Refinement on the full spectrogram, then the near-optimal parameter ranges.
  for (int t : peaks) {
    const Best& b = best[std::size_t(t)];
    const Template& T = tmpl[std::size_t(b.tmpl)];
    Event e;
    e.detect_score = b.score;
    e.significance = mad > 0 ? (b.score - med) / mad : 0.0;
    e.half_window_s = T.hw;
    const double f0_init = fmin * std::exp(b.k * o.logf_step);
    const double tc_init = t_of(t);
    // A coarse-to-fine grid over (tc, v, d, f0).
    struct P {
      double tc, v, d, f0, sc;
    };
    std::vector<P> grid;
    std::mutex mu;
    std::vector<double> vs, ds;
    for (double v = std::max(o.vmin * 0.8, T.v - 16); v <= std::min(o.vmax * 1.1, T.v + 16) + 1e-9; v += 2.0) vs.push_back(v);
    for (double d = T.d / 1.8; d <= T.d * 1.8; d *= 1.06) ds.push_back(d);
    parallel_for(vs.size(), [&](std::size_t iv) {
      std::vector<P> loc;
      for (double d : ds)
        for (double tc = tc_init - 2.0; tc <= tc_init + 2.0 + 1e-9; tc += 0.25)
          for (double rf = -0.015; rf <= 0.015 + 1e-9; rf += 0.001) {
            const double f0 = f0_init * (1 + rf);
            loc.push_back({tc, vs[iv], d, f0, cv_score(s, tc, vs[iv], d, f0, T.hw)});
          }
      std::lock_guard lk(mu);
      grid.insert(grid.end(), loc.begin(), loc.end());
    });
    auto top = *std::max_element(grid.begin(), grid.end(), [](const P& a, const P& b) { return a.sc < b.sc; });
    // Final polish of tc and f0 at frame resolution.
    for (double tc = top.tc - 0.25; tc <= top.tc + 0.25 + 1e-9; tc += 0.032)
      for (double rf = -0.0012; rf <= 0.0012 + 1e-9; rf += 0.0002) {
        const double f0 = top.f0 * (1 + rf), sc2 = cv_score(s, tc, top.v, top.d, f0, T.hw);
        if (sc2 > top.sc) top = {tc, top.v, top.d, f0, sc2};
      }
    e.tc = top.tc, e.v = top.v, e.d = top.d, e.f0 = top.f0, e.score = top.sc;
    const double delta = std::max(3.0, 0.15 * top.sc);
    e.tc_lo = e.tc_hi = e.tc, e.v_lo = e.v_hi = e.v, e.d_lo = e.d_hi = e.d;
    for (auto& p : grid)
      if (p.sc >= top.sc - delta) {
        e.tc_lo = std::min(e.tc_lo, p.tc), e.tc_hi = std::max(e.tc_hi, p.tc);
        e.v_lo = std::min(e.v_lo, p.v), e.v_hi = std::max(e.v_hi, p.v);
        e.d_lo = std::min(e.d_lo, p.d), e.d_hi = std::max(e.d_hi, p.d);
      }
    // Other lines of the same source: same shape, rest frequency in a simple ratio.
    e.lines.push_back({e.f0, e.score});
    for (double r : {1.0 / 4, 1.0 / 3, 1.0 / 2, 2.0 / 3, 3.0 / 4, 4.0 / 3, 3.0 / 2, 2.0, 5.0 / 2, 3.0, 4.0, 5.0, 6.0}) {
      const double fr = e.f0 * r;
      if (fr < s.freq(2) || fr * 1.35 > s.fmax()) continue;
      Line best_line{fr, -INFINITY};
      for (double rf = -0.004; rf <= 0.004 + 1e-9; rf += 0.0005) {
        const double sc2 = cv_score(s, e.tc, e.v, e.d, fr * (1 + rf), e.half_window_s);
        if (sc2 > best_line.score) best_line = {fr * (1 + rf), sc2};
      }
      if (best_line.score >= std::max(5.0, 0.3 * e.score)) e.lines.push_back(best_line);
    }
    std::sort(e.lines.begin(), e.lines.end(), [](const Line& a, const Line& b) { return a.score > b.score; });
    out.push_back(std::move(e));
  }
  std::sort(out.begin(), out.end(), [](const Event& a, const Event& b) { return a.tc < b.tc; });
  return res;
}

json::object event_json(const Event& e) {
  json::array lines;
  for (auto& l : e.lines) lines.push_back(json::object{{"f0_hz", l.f0}, {"score", l.score}});
  return {{"tc_s", e.tc},       {"tc_range_s", json::array{e.tc_lo, e.tc_hi}},
          {"speed_ms", e.v},    {"speed_range_ms", json::array{e.v_lo, e.v_hi}},
          {"cpa_m", e.d},       {"cpa_range_m", json::array{e.d_lo, e.d_hi}},
          {"f0_hz", e.f0},      {"score", e.score},
          {"detect_score", e.detect_score}, {"significance", e.significance}, {"half_window_s", e.half_window_s},
          {"lines", lines}};
}

Event event_from_json(const json::value& v) {
  Event e;
  auto range = [&](std::string_view key, double& lo, double& hi, double mid) {
    const json::value* r = jget(v, key);
    if (r && r->is_array() && r->as_array().size() == 2) lo = as_number(r->as_array()[0]), hi = as_number(r->as_array()[1]);
    else lo = hi = mid;
  };
  e.tc = jnum(v, "tc_s"), e.v = jnum(v, "speed_ms"), e.d = jnum(v, "cpa_m"), e.f0 = jnum(v, "f0_hz");
  range("tc_range_s", e.tc_lo, e.tc_hi, e.tc);
  range("speed_range_ms", e.v_lo, e.v_hi, e.v);
  range("cpa_range_m", e.d_lo, e.d_hi, e.d);
  e.score = jnum(v, "score", 0), e.detect_score = jnum(v, "detect_score", 0), e.significance = jnum(v, "significance", 0);
  e.half_window_s = jnum(v, "half_window_s", half_window(e.v, e.d));
  if (const json::value* ls = jget(v, "lines"); ls && ls->is_array())
    for (auto& l : ls->as_array()) e.lines.push_back({jnum(l, "f0_hz"), jnum(l, "score", 0)});
  if (e.lines.empty()) e.lines.push_back({e.f0, e.score});
  return e;
}

}  // namespace geo::air
