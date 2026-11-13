#include "air/doppler.hpp"

#include <algorithm>
#include <cmath>

namespace geo::air {

double cv_ratio(double t, double tc, double v, double d, double c) {
  // tau_c = tc - d/c; with s = tau - tau_c and T = t - tau_c, the retarded-time
  // equation c (T - s) = sqrt(d^2 + v^2 s^2) has the root (s < T):
  //   s = (c^2 T - sqrt(c^2 v^2 T^2 + d^2 (c^2 - v^2))) / (c^2 - v^2)
  const double T = t - tc + d / c;
  const double c2 = c * c, v2 = v * v;
  const double s = (c2 * T - std::sqrt(c2 * v2 * T * T + d * d * (c2 - v2))) / (c2 - v2);
  const double r = std::sqrt(d * d + v2 * s * s);
  const double ur = r > 0 ? v2 * s / r : 0.0;
  return c / (c + ur);
}

Path::Path(std::vector<PathPoint> pts) : pts_(std::move(pts)) {
  // A reported velocity is kept only where it agrees with the neighbouring positions;
  // otherwise the node takes the finite-difference slope (Catmull-Rom style).
  const std::size_t n = pts_.size();
  for (std::size_t i = 0; i < n && n >= 2; ++i) {
    const std::size_t a = i ? i - 1 : 0, b = std::min(n - 1, i + 1);
    const double dt = pts_[b].t - pts_[a].t;
    if (dt <= 0) continue;
    const Vec3 fd = (pts_[b].p - pts_[a].p) * (1.0 / dt);
    const Vec3& v = pts_[i].v;
    const bool missing = !std::isfinite(v.x) || !std::isfinite(v.y) || !std::isfinite(v.z);
    const double tol = 0.25 * fd.norm() + 15.0;
    if (missing || (v - fd).norm() > tol) pts_[i].v = fd;
  }
}

void Path::state(double t, Vec3& p, Vec3& v) const {
  auto it = std::upper_bound(pts_.begin(), pts_.end(), t, [](double x, const PathPoint& q) { return x < q.t; });
  const std::size_t i = std::size_t(std::clamp<std::ptrdiff_t>(it - pts_.begin() - 1, 0, std::ptrdiff_t(pts_.size()) - 2));
  const PathPoint &a = pts_[i], &b = pts_[i + 1];
  const double h = b.t - a.t, s = h > 0 ? (t - a.t) / h : 0.0;
  const double s2 = s * s, s3 = s2 * s;
  const double h00 = 2 * s3 - 3 * s2 + 1, h10 = s3 - 2 * s2 + s, h01 = -2 * s3 + 3 * s2, h11 = s3 - s2;
  p = a.p * h00 + a.v * (h10 * h) + b.p * h01 + b.v * (h11 * h);
  if (h <= 0) {
    v = a.v;
    return;
  }
  const double d00 = 6 * s2 - 6 * s, d10 = 3 * s2 - 4 * s + 1, d01 = -6 * s2 + 6 * s, d11 = 3 * s2 - 2 * s;
  v = a.p * (d00 / h) + a.v * d10 + b.p * (d01 / h) + b.v * d11;
}

std::optional<Emission> emission_for(const Path& path, const Vec3& o, double t, double c, double guess) {
  if (path.empty()) return std::nullopt;
  // Newton on g(tau) = tau + r(tau)/c - t, g' = 1 + u_r / c (> 0 for subsonic motion).
  double tau = std::clamp(std::isfinite(guess) ? guess : t, path.t0(), path.t1());
  Vec3 p, v;
  for (int it = 0; it < 12; ++it) {
    path.state(tau, p, v);
    const Vec3 d = p - o;
    const double r = d.norm(), ur = r > 0 ? v.dot(d) / r : 0.0;
    const double g = tau + r / c - t, step = g / (1.0 + ur / c);
    tau = std::clamp(tau - step, path.t0(), path.t1());
    if (std::abs(step) < 1e-4) break;
  }
  path.state(tau, p, v);
  const Vec3 d = p - o;
  const double r = d.norm();
  if (std::abs(tau + r / c - t) > 0.05) return std::nullopt;  // reception outside the path's coverage
  return Emission{tau, r, r > 0 ? v.dot(d) / r : 0.0};
}

Approach closest_approach(const Path& path, const Vec3& o, double c) {
  Approach best{.r = INFINITY};
  if (path.empty()) return best;
  Vec3 p, v;
  auto range = [&](double t) {
    path.state(t, p, v);
    return (p - o).norm();
  };
  const double t0 = path.t0(), t1 = path.t1();
  const int n = std::max(2, int(std::ceil((t1 - t0) / 1.0)));
  double tb = t0;
  for (int i = 0; i <= n; ++i) {
    const double t = t0 + (t1 - t0) * i / n, r = range(t);
    if (r < best.r) best.r = r, tb = t;
  }
  // Golden-section refinement within one step either side.
  double a = std::max(t0, tb - (t1 - t0) / n), b = std::min(t1, tb + (t1 - t0) / n);
  const double g = 0.5 * (std::sqrt(5.0) - 1.0);
  double x1 = b - g * (b - a), x2 = a + g * (b - a), f1 = range(x1), f2 = range(x2);
  for (int it = 0; it < 40 && b - a > 1e-3; ++it) {
    if (f1 < f2) b = x2, x2 = x1, f2 = f1, x1 = b - g * (b - a), f1 = range(x1);
    else a = x1, x1 = x2, f1 = f2, x2 = a + g * (b - a), f2 = range(x2);
  }
  const double tm = 0.5 * (a + b), rm = range(tm);
  if (rm < best.r) best.r = rm, tb = tm;
  path.state(tb, p, v);
  best.tau = tb;
  best.t_rx = tb + best.r / c;
  best.speed = v.norm();
  best.interior = tb > t0 + 0.5 && tb < t1 - 0.5;
  return best;
}

}  // namespace geo::air
