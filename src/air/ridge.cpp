#include "air/ridge.hpp"

#include <algorithm>
#include <array>
#include <cmath>

namespace geo::air {

double ca_ratio(double t, double tc, double v, double d, double a, double c) {
  // Emission time tau (relative to closest approach) of the sound heard at t:
  // t - tc = tau + (r(tau) - d) / c, s(tau) = v tau + a tau^2 / 2, r = sqrt(s^2 + d^2).
  const double dt = t - tc;
  double tau = dt;
  if (a == 0.0) return cv_ratio(t, tc, v, d, c);
  for (int it = 0; it < 30; ++it) {
    const double s = v * tau + 0.5 * a * tau * tau, u = v + a * tau, r = std::hypot(s, d);
    const double g = tau + (r - d) / c - dt, dg = 1 + s * u / (r * c);
    const double step = g / std::max(0.05, dg);
    tau -= step;
    if (std::abs(step) < 1e-6) break;
  }
  const double s = v * tau + 0.5 * a * tau * tau, u = v + a * tau, r = std::hypot(s, d);
  if (u < 5.0) return NAN;
  return c / (c + s * u / r);
}

Ridge trace_ridge(const Spectrogram& s, double tc, double v, double d, double f_line, double half_window_s) {
  Ridge out;
  const double W = 1.5 * half_window_s;
  const int j0 = std::max(0, int(std::ceil(s.frame_at(tc - W)))), j1 = std::min(s.nf - 1, int(std::floor(s.frame_at(tc + W))));
  if (j1 - j0 < 8) return out;
  const int B = std::clamp(int(std::ceil(0.04 * f_line * 1.1 / s.df)), 3, 40), NS = 2 * B + 1, NJ = j1 - j0 + 1;
  constexpr double LAMBDA = 0.7;  // cost per bin of change between frames
  constexpr int MAXSTEP = 2;      // bins per frame (0.128 s) the tone may move off the template's curve
  std::vector<double> fp(static_cast<std::size_t>(NJ));
  std::vector<float> em(static_cast<std::size_t>(NJ * NS));
  for (int j = 0; j < NJ; ++j) {
    fp[std::size_t(j)] = f_line * cv_ratio(s.frame_time(j0 + j), tc, v, d);
    for (int o = 0; o < NS; ++o) {
      const float z = s.at(j0 + j, fp[std::size_t(j)] + (o - B) * s.df);
      em[std::size_t(j * NS + o)] = std::isfinite(z) ? std::max(z, -1.0f) : -1.0f;
    }
  }
  std::vector<double> acc(static_cast<std::size_t>(NS)), nxt(static_cast<std::size_t>(NS));
  std::vector<int> back(static_cast<std::size_t>(NJ * NS), 0);
  for (int o = 0; o < NS; ++o) acc[std::size_t(o)] = em[std::size_t(o)] - 0.05 * std::abs(o - B);  // mild pull to the template
  for (int j = 1; j < NJ; ++j) {
    for (int o = 0; o < NS; ++o) {
      double best = -INFINITY;
      int bo = o;
      for (int p = std::max(0, o - MAXSTEP); p <= std::min(NS - 1, o + MAXSTEP); ++p) {
        const double c = acc[std::size_t(p)] - LAMBDA * std::abs(o - p);
        if (c > best) best = c, bo = p;
      }
      nxt[std::size_t(o)] = best + em[std::size_t(j * NS + o)];
      back[std::size_t(j * NS + o)] = bo;
    }
    std::swap(acc, nxt);
  }
  int o = int(std::max_element(acc.begin(), acc.end()) - acc.begin());
  std::vector<int> path(static_cast<std::size_t>(NJ));
  for (int j = NJ - 1; j >= 0; --j) {
    path[std::size_t(j)] = o;
    o = back[std::size_t(j * NS + o)];
  }
  int core = 0, held = 0;
  for (int j = 0; j < NJ; ++j) {
    const int q = path[std::size_t(j)];
    const double t = s.frame_time(j0 + j);
    const double zc = em[std::size_t(j * NS + q)];
    double off = 0;  // parabolic peak between neighbouring bins
    if (q > 0 && q < NS - 1) {
      const double zl = em[std::size_t(j * NS + q - 1)], zr = em[std::size_t(j * NS + q + 1)], den = zl - 2 * zc + zr;
      if (den < 0) off = std::clamp(0.5 * (zl - zr) / den, -0.5, 0.5);
    }
    out.t.push_back(t);
    out.f.push_back(fp[std::size_t(j)] + (q - B + off) * s.df);
    out.z.push_back(zc);
    const bool h = zc >= s.held_level(j0 + j);
    out.held.push_back(h);
    if (std::abs(t - tc) <= half_window_s) {
      ++core;
      held += h;
    }
  }
  out.coverage = core ? double(held) / core : 0.0;
  return out;
}

namespace {

using P5 = std::array<double, 5>;  // tc, v, log d, f0, a

double objective(const Ridge& r, const std::vector<int>& use, const P5& p) {
  const double v = p[1], d = std::exp(p[2]), a = p[4];
  if (v < 10 || v > 320 || d < 20 || d > 30000 || std::abs(a) > 3.0 || p[3] <= 0) return INFINITY;
  double num = 0, den = 0;
  for (int i : use) {
    const double m = p[3] * ca_ratio(r.t[std::size_t(i)], p[0], v, d, a);
    if (!std::isfinite(m)) return INFINITY;
    const double w = std::min(r.z[std::size_t(i)], 6.0), e = m - r.f[std::size_t(i)];
    num += w * e * e, den += w;
  }
  return den > 0 ? num / den : INFINITY;
}

P5 nelder_mead(const Ridge& r, const std::vector<int>& use, P5 x0, const P5& step, double& fbest) {
  std::array<P5, 6> X;
  std::array<double, 6> F;
  X[0] = x0;
  for (int i = 0; i < 5; ++i) {
    X[std::size_t(i + 1)] = x0;
    X[std::size_t(i + 1)][std::size_t(i)] += step[std::size_t(i)];
  }
  for (int i = 0; i < 6; ++i) F[std::size_t(i)] = objective(r, use, X[std::size_t(i)]);
  for (int it = 0; it < 600; ++it) {
    std::array<int, 6> idx{0, 1, 2, 3, 4, 5};
    std::sort(idx.begin(), idx.end(), [&](int a, int b) { return F[std::size_t(a)] < F[std::size_t(b)]; });
    std::array<P5, 6> Xs;
    std::array<double, 6> Fs;
    for (int i = 0; i < 6; ++i) Xs[std::size_t(i)] = X[std::size_t(idx[std::size_t(i)])], Fs[std::size_t(i)] = F[std::size_t(idx[std::size_t(i)])];
    X = Xs, F = Fs;
    if (std::isfinite(F[0]) && F[5] - F[0] < 1e-7 * (1 + F[0])) break;
    P5 c{};
    for (int i = 0; i < 5; ++i)
      for (int k = 0; k < 5; ++k) c[std::size_t(k)] += X[std::size_t(i)][std::size_t(k)] / 5;
    auto along = [&](double t) {
      P5 y;
      for (int k = 0; k < 5; ++k) y[std::size_t(k)] = c[std::size_t(k)] + t * (X[5][std::size_t(k)] - c[std::size_t(k)]);
      return y;
    };
    const P5 xr = along(-1);
    const double fr = objective(r, use, xr);
    if (fr < F[0]) {
      const P5 xe = along(-2);
      const double fe = objective(r, use, xe);
      if (fe < fr) X[5] = xe, F[5] = fe;
      else X[5] = xr, F[5] = fr;
    } else if (fr < F[4]) {
      X[5] = xr, F[5] = fr;
    } else {
      const P5 xc = along(fr < F[5] ? -0.5 : 0.5);
      const double fc = objective(r, use, xc);
      if (fc < std::min(fr, F[5])) {
        X[5] = xc, F[5] = fc;
      } else {
        for (int i = 1; i < 6; ++i) {
          for (int k = 0; k < 5; ++k) X[std::size_t(i)][std::size_t(k)] = X[0][std::size_t(k)] + 0.5 * (X[std::size_t(i)][std::size_t(k)] - X[0][std::size_t(k)]);
          F[std::size_t(i)] = objective(r, use, X[std::size_t(i)]);
        }
      }
    }
  }
  const int ib = int(std::min_element(F.begin(), F.end()) - F.begin());
  fbest = F[std::size_t(ib)];
  return X[std::size_t(ib)];
}

}  // namespace

PassFit fit_pass(const Ridge& r, double tc, double v, double d, double f0) {
  PassFit out;
  std::vector<int> use;
  for (std::size_t i = 0; i < r.t.size(); ++i)
    if (r.held[i]) use.push_back(int(i));
  if (use.size() < 12) return out;
  const P5 step{0.5, 5.0, 0.15, 0.002 * f0, 0.3};
  double fbest = INFINITY;
  P5 best{};
  for (int pass = 0; pass < 2; ++pass) {
    fbest = INFINITY;
    for (double a0 : {0.0, -0.7, 0.7}) {
      double f;
      const P5 x = nelder_mead(r, use, pass == 0 ? P5{tc, v, std::log(d), f0, a0} : P5{best[0], best[1], best[2], best[3], a0}, step, f);
      double f2;
      const P5 y = nelder_mead(r, use, x, step, f2);  // restart: Nelder-Mead stalls on long valleys
      if (f2 < fbest) fbest = f2, best = y;
    }
    if (pass == 1 || !std::isfinite(fbest)) break;
    // Drop outliers (a frame where the path jumped to another tone), refit once.
    const double cut = std::max(3.0 * std::sqrt(fbest), 1.5);
    std::vector<int> keep;
    for (int i : use) {
      const double m = best[3] * ca_ratio(r.t[std::size_t(i)], best[0], best[1], std::exp(best[2]), best[4]);
      if (std::abs(m - r.f[std::size_t(i)]) <= cut) keep.push_back(i);
    }
    if (keep.size() < 12) break;
    use = std::move(keep);
  }
  if (!std::isfinite(fbest)) return out;
  out.ok = true;
  out.tc = best[0], out.v = best[1], out.d = std::exp(best[2]), out.f0 = best[3], out.a = best[4];
  out.rmse_hz = std::sqrt(fbest), out.n = int(use.size());
  return out;
}

}  // namespace geo::air
