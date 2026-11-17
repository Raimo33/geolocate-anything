// What tone a propeller aircraft can make: a per-type prior on the rest frequency.
//
// A propeller's blade-pass frequency is RPM x blades / 60, and a constant-speed
// propeller holds its RPM within a narrow band set by the type (a PC-12 turns 1,700 rpm
// with four blades: 113 Hz). A direct-drive piston engine adds its firing frequency,
// RPM / 60 x cylinders / 2, on the same shaft. The table (bladepass.cpp, sources in
// docs/aircraft/blade-pass.md) holds, per ICAO type designator, these tone sources as
// (order, RPM range): order = blades, or cylinders / 2. The RPM range spans descent to
// take-off power, not only cruise.
//
// The detector may lock onto a harmonic or the subharmonic of the true fundamental, so
// a measured f0 is compatible with a type if f0 / k falls in one of its tone ranges
// (with a margin) for k in {1/2, 1, 2, 3}. Types not in the table (helicopters,
// geared engines, rare types, empty designators) are always compatible.
//
// Learned observations (`aircraft learn-f0`) extend the table: each confirmed match's
// rest frequency becomes a narrow extra range for its type, kept in
// <cache>/adsb/blade_pass_learned.json.
#pragma once

#include <map>
#include <string>
#include <string_view>
#include <vector>

#include "util/files.hpp"
#include "util/json.hpp"

namespace geo::air {

struct ToneSource {
  double order = 0;            // tones per shaft revolution (blades, or cylinders / 2)
  double rpm_lo = 0, rpm_hi = 0;
  double f_lo() const { return order * rpm_lo / 60.0; }
  double f_hi() const { return order * rpm_hi / 60.0; }
};

struct TypePrior {
  std::vector<ToneSource> sources;
  std::vector<double> learned;  // confirmed rest frequencies, Hz (as measured: may be a harmonic)
};

struct PriorOptions {
  double margin = 0.04;                             // relative, on every range (RPM beyond the book, f0 estimate)
  std::vector<double> multiples = {0.5, 1, 2, 3};   // measured f0 = k x the true fundamental
  double learned_halfwidth = 0.03;                  // relative, around each learned frequency
  int learned_min = 3;                              // observations needed for a type absent from the table
};

class BladePassTable {
 public:
  static BladePassTable builtin();
  // The built-in table plus the learned observations in the cache (if any).
  static BladePassTable load();
  static fs::path learned_path();

  // Fundamental ranges [lo, hi] in Hz (no margin); empty when the type gives no constraint.
  std::vector<std::pair<double, double>> ranges(std::string_view type, const PriorOptions& o = {}) const;
  // Can an aircraft of this type produce an event whose lines are `f0s` (strongest first)?
  // The strongest line is tried at every multiple, the others as fundamentals only.
  bool compatible(std::string_view type, const std::vector<double>& f0s, const PriorOptions& o = {}) const;
  bool known(std::string_view type) const;

  void add(const std::string& type, std::vector<ToneSource> sources);
  void learn(const std::string& type, double f0);
  json::object learned_json() const;
  void merge_learned(const json::value& v);  // {"PC12": [113.8, ...], ...}
  std::size_t size() const { return types_.size(); }
  const std::map<std::string, TypePrior>& types() const { return types_; }

 private:
  std::map<std::string, TypePrior> types_;
};

// Two events explained by the same aircraft: their tones must agree, up to a simple
// ratio (1, 2, 3, 1/2, 1/3, 3/2, 2/3) between any of their lines, within `tol`.
bool f0_consistent(const std::vector<double>& a, const std::vector<double>& b, double tol = 0.05);

}  // namespace geo::air
