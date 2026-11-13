// WGS84 geodesy for the aircraft search: geodetic <-> ECEF, local east-north-up frames
// (an exact rotation of ECEF, so nothing is approximated by working in one), and the
// small vector type the acoustic model uses.
#pragma once

#include <cmath>
#include <numbers>

namespace geo::air {

inline constexpr double DEG = std::numbers::pi / 180.0;
inline constexpr double WGS84_A = 6378137.0;
inline constexpr double WGS84_F = 1.0 / 298.257223563;
inline constexpr double WGS84_E2 = WGS84_F * (2.0 - WGS84_F);
inline constexpr double FT = 0.3048, KT = 1852.0 / 3600.0, FPM = FT / 60.0;
inline constexpr double EARTH_R = 6371008.8;  // mean radius, for distances and cell sizes

struct Vec3 {
  double x = 0, y = 0, z = 0;
  Vec3 operator+(const Vec3& o) const { return {x + o.x, y + o.y, z + o.z}; }
  Vec3 operator-(const Vec3& o) const { return {x - o.x, y - o.y, z - o.z}; }
  Vec3 operator*(double s) const { return {x * s, y * s, z * s}; }
  Vec3& operator+=(const Vec3& o) { return x += o.x, y += o.y, z += o.z, *this; }
  double dot(const Vec3& o) const { return x * o.x + y * o.y + z * o.z; }
  double norm() const { return std::sqrt(dot(*this)); }
};

inline Vec3 ecef(double lat_deg, double lon_deg, double h) {
  const double la = lat_deg * DEG, lo = lon_deg * DEG;
  const double s = std::sin(la), c = std::cos(la);
  const double n = WGS84_A / std::sqrt(1.0 - WGS84_E2 * s * s);
  return {(n + h) * c * std::cos(lo), (n + h) * c * std::sin(lo), (n * (1.0 - WGS84_E2) + h) * s};
}

struct Geodetic {
  double lat = 0, lon = 0, h = 0;
};
// ECEF -> geodetic (Bowring's method plus one iteration: sub-millimetre at aircraft heights).
inline Geodetic geodetic(const Vec3& p) {
  const double b = WGS84_A * (1 - WGS84_F), ep2 = (WGS84_A * WGS84_A - b * b) / (b * b);
  const double r = std::hypot(p.x, p.y);
  double th = std::atan2(p.z * WGS84_A, r * b);
  double la = std::atan2(p.z + ep2 * b * std::pow(std::sin(th), 3), r - WGS84_E2 * WGS84_A * std::pow(std::cos(th), 3));
  for (int i = 0; i < 2; ++i) {
    const double s = std::sin(la), n = WGS84_A / std::sqrt(1 - WGS84_E2 * s * s);
    const double h = r / std::cos(la) - n;
    la = std::atan2(p.z, r * (1 - WGS84_E2 * n / (n + h)));
  }
  const double s = std::sin(la), n = WGS84_A / std::sqrt(1 - WGS84_E2 * s * s);
  const double h = std::abs(std::cos(la)) > 1e-9 ? r / std::cos(la) - n : std::abs(p.z) - n * (1 - WGS84_E2);
  return {la / DEG, std::atan2(p.y, p.x) / DEG, h};
}

// East-north-up frame at a geodetic point. Vectors convert with the rotation alone;
// positions also subtract the origin.
struct Enu {
  Vec3 o;
  double e[3], n[3], u[3];
  Enu() = default;
  Enu(double lat_deg, double lon_deg, double h = 0.0) {
    const double la = lat_deg * DEG, lo = lon_deg * DEG;
    const double sl = std::sin(la), cl = std::cos(la), so = std::sin(lo), co = std::cos(lo);
    o = ecef(lat_deg, lon_deg, h);
    e[0] = -so, e[1] = co, e[2] = 0;
    n[0] = -sl * co, n[1] = -sl * so, n[2] = cl;
    u[0] = cl * co, u[1] = cl * so, u[2] = sl;
  }
  Vec3 rot(const Vec3& v) const {  // ECEF direction -> ENU
    return {e[0] * v.x + e[1] * v.y + e[2] * v.z, n[0] * v.x + n[1] * v.y + n[2] * v.z, u[0] * v.x + u[1] * v.y + u[2] * v.z};
  }
  Vec3 unrot(const Vec3& v) const {  // ENU direction -> ECEF
    return {e[0] * v.x + n[0] * v.y + u[0] * v.z, e[1] * v.x + n[1] * v.y + u[1] * v.z, e[2] * v.x + n[2] * v.y + u[2] * v.z};
  }
  Vec3 pos(const Vec3& p_ecef) const { return rot(p_ecef - o); }
  Vec3 pos(double lat_deg, double lon_deg, double h) const { return pos(ecef(lat_deg, lon_deg, h)); }
};

// Velocity in the ENU frame at (lat, lon) from ground speed, true track and vertical
// rate (all SI), returned as an ECEF direction.
inline Vec3 velocity_ecef(double lat_deg, double lon_deg, double gs, double track_deg, double vrate) {
  const Enu f(lat_deg, lon_deg);
  const double t = track_deg * DEG;
  return f.unrot({gs * std::sin(t), gs * std::cos(t), vrate});
}

inline double haversine_m(double lat1, double lon1, double lat2, double lon2) {
  const double dla = (lat2 - lat1) * DEG, dlo = (lon2 - lon1) * DEG;
  const double a = std::sin(dla / 2) * std::sin(dla / 2) + std::cos(lat1 * DEG) * std::cos(lat2 * DEG) * std::sin(dlo / 2) * std::sin(dlo / 2);
  return 2 * EARTH_R * std::asin(std::min(1.0, std::sqrt(a)));
}

}  // namespace geo::air
