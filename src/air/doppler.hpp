// Acoustic model of a flyover heard by a fixed listener.
//
// An aircraft at x(tau) with velocity v(tau) emits a tone f0. Sound emitted at tau
// arrives at listener o at t = tau + r(tau)/c, r = |x - o|, with frequency
//
//     f(t) = f0 * c / (c + u_r(tau)),   u_r = v . (x - o) / r   (positive receding).
//
// The frequency heard at t depends on the motion when the sound left the aircraft, not
// on where it is at t: the emission time is solved for every reception time (a
// constant delay gives a different, wrong curve). Wind, refraction and a moving
// listener are ignored; c is one effective speed for the path.
#pragma once

#include <optional>
#include <vector>

#include "air/geodesy.hpp"

namespace geo::air {

inline constexpr double SOUND_SPEED = 340.0;  // m/s, dry air near 15 C

// Straight, constant-speed pass: speed v, closest slant distance d, and tc the reception
// time of the sound emitted at closest approach (the "delayed CPA"), when f = f0.
// Returns f / f0 at reception time t (closed form of the retarded-time equation).
double cv_ratio(double t, double tc, double v, double d, double c = SOUND_SPEED);

struct PathPoint {
  double t = 0;  // epoch seconds
  Vec3 p, v;     // position and velocity in one local frame (m, m/s)
};

// A gap-free flight segment, interpolated as a cubic Hermite curve through the
// reported positions and velocities (finite differences where a reported velocity is
// missing or disagrees with the positions). Never extrapolated.
class Path {
 public:
  Path() = default;
  explicit Path(std::vector<PathPoint> pts);
  bool empty() const { return pts_.size() < 2; }
  double t0() const { return pts_.front().t; }
  double t1() const { return pts_.back().t; }
  const std::vector<PathPoint>& points() const { return pts_; }
  // Position and velocity at t in [t0, t1] (thread-safe: no cached state).
  void state(double t, Vec3& p, Vec3& v) const;

 private:
  std::vector<PathPoint> pts_;
};

struct Emission {
  double tau = 0;  // emission time
  double r = 0;    // slant range at emission
  double ur = 0;   // radial velocity at emission (positive receding)
  double ratio(double c = SOUND_SPEED) const { return c / (c + ur); }
};
// The emission heard at listener o at reception time t, if the path covers it. `guess`
// (a nearby emission time, e.g. the previous sample's) saves Newton steps.
std::optional<Emission> emission_for(const Path& path, const Vec3& o, double t, double c = SOUND_SPEED, double guess = NAN);

struct Approach {
  double tau = 0;   // time of closest approach (emission)
  double r = 0;     // slant range then
  double t_rx = 0;  // its reception time, tau + r / c
  double speed = 0;
  bool interior = false;  // the minimum is inside the path, not at an end
};
Approach closest_approach(const Path& path, const Vec3& o, double c = SOUND_SPEED);

}  // namespace geo::air
