#include "air/bladepass.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <sstream>

#include "air/adsb.hpp"

namespace geo::air {

namespace {

// Seed table. RPM ranges are propeller RPM from descent/low cruise to the take-off
// limit (type certificate data sheets, POH limitations; docs/aircraft/blade-pass.md).
// `blades` lists every blade count fitted (factory and common STCs); `cylinders` is
// for direct-drive piston engines (firing frequency = cylinders / 2 per revolution).
// Geared engines (Rotax 912, GTSIO-520, diesels) list blade-pass only: their firing
// tone sits on the crankshaft, at a different RPM; helicopters are left out (main-rotor
// harmonics cover the whole band).
struct Seed {
  const char* types;
  const char* blades;
  double rpm_lo, rpm_hi;
  const char* cylinders;
};

constexpr Seed SEEDS[] = {
    // --- Piston singles, direct drive -------------------------------------------------
    {"C150 C152", "2", 1900, 2750, "4"},
    {"C172 C72R C177", "2", 1900, 2700, "4 6"},  // O-320/O-360/IO-360; early 172s O-300 (6 cyl)
    {"C170", "2", 1900, 2700, "6"},
    {"C182 C82R C82T", "2 3", 1900, 2600, "6"},
    {"C180 C185 C206 T206 C207 C210 T210 P210 C337", "2 3", 1900, 2850, "6"},
    {"P28A PA18 PA22 PA20 AA5 AA1 BE23 BE76 BE77 TB10", "2", 1900, 2700, "4"},
    {"P28B P28R P28T PA24 PA32 P32R PA46 TB20 TB21 GA8", "2 3", 1900, 2800, "4 6"},
    {"BE33 BE35 BE36 BE24", "2 3", 1900, 2700, "4 6"},
    {"M20P M20J M20K M20R M20T M20M", "2 3", 1900, 2700, "4 6"},
    {"SR20 SR22 S22T", "2 3", 1900, 2700, "6"},
    {"DA40", "3", 1900, 2400, ""},  // Lycoming IO-360 (2,400) or geared diesel (2,300)
    // --- Piston twins -----------------------------------------------------------------
    {"PA44 BE95", "2", 1900, 2700, "4"},
    {"PA34 PA23 PA30 PA31 BE55 BE58 C310 C320 C340 C402 C414 BN2P", "2 3", 1900, 2800, "4 6"},
    {"DA42 DA62", "3", 1900, 2300, ""},  // geared diesels
    // --- Radials (9 cylinders) --------------------------------------------------------
    {"DHC2 BE18", "2 3", 1700, 2300, "9"},
    // --- Turboprops (free turbine / constant speed: narrow bands) ----------------------
    {"PC12", "4 5", 1500, 1750, ""},             // 1,700; NGX low-speed mode 1,550
    {"PC6", "3", 1800, 2250, ""},
    {"PC7", "3", 1800, 2250, ""},
    {"B350 BE30", "4 5", 1400, 1750, ""},       // 1,700 max; 4-blade OEM, Raisbeck/Hartzell 5-blade STC
    {"BE20", "3 4 5", 1600, 2050, ""},          // King Air 200/250: 2,000 max
    {"BE9L BE9T BE10 BE99", "3 4", 1700, 2250, ""},  // King Air 90/100, Beech 99: 2,200 max
    {"B190", "4", 1400, 1750, ""},
    {"C208", "3 4", 1600, 1950, ""},            // 1,900 max
    {"C425", "3 4", 1600, 1950, ""},
    {"TBM7 TBM8 TBM9", "4 5", 1600, 2050, ""},  // 2,000 max (TBM 960: 1,925)
    {"P46T", "4 5", 1700, 2050, ""},
    {"KODI", "4 5", 1700, 2250, ""},
    {"DHC6", "3 4", 1700, 2250, ""},            // 2,200 max; Raisbeck 4-blade STC
    {"PAY1 PAY2 PAY3 PAY4", "3 4", 1600, 2250, ""},
    {"C441 MU2 SW3 SW4 JS31 JS32 D228 AC90 AC95", "3 4 5", 1300, 1650, ""},  // Garrett TPE331: 1,591 at 100%
    {"E120", "4", 1000, 1350, ""},
    {"SF34", "4", 1100, 1420, ""},              // 1,396 max
    {"DH8A DH8B DH8C", "4", 850, 1230, ""},      // 1,212 max
    {"DH8D", "6", 800, 1050, ""},               // Q400: 1,020 take-off, 900 climb, 850 cruise
    {"AT43 AT44 AT45 AT46 AT72 AT73 AT75 AT76", "4 6", 900, 1230, ""},  // 100% Np = 1,200, cruise 82%
};

std::vector<double> numbers(const char* s) {
  std::vector<double> out;
  std::istringstream in(s);
  double x;
  while (in >> x) out.push_back(x);
  return out;
}

std::string norm(std::string_view t) {
  std::string s;
  for (char c : t)
    if (!std::isspace(static_cast<unsigned char>(c))) s += char(std::toupper(static_cast<unsigned char>(c)));
  return s;
}

}  // namespace

BladePassTable BladePassTable::builtin() {
  BladePassTable t;
  for (auto& s : SEEDS) {
    std::vector<ToneSource> src;
    for (double b : numbers(s.blades)) src.push_back({b, s.rpm_lo, s.rpm_hi});
    for (double c : numbers(s.cylinders)) src.push_back({c / 2, s.rpm_lo, s.rpm_hi});
    std::istringstream in(s.types);
    std::string type;
    while (in >> type) t.add(type, src);
  }
  return t;
}

fs::path BladePassTable::learned_path() { return adsb_dir() / "blade_pass_learned.json"; }

BladePassTable BladePassTable::load() {
  BladePassTable t = builtin();
  if (auto txt = read_file(learned_path())) {
    try {
      t.merge_learned(parse_json(*txt));
    } catch (const std::exception& e) {
      std::fprintf(stderr, "warning: ignoring %s: %s\n", learned_path().string().c_str(), e.what());
    }
  }
  return t;
}

void BladePassTable::add(const std::string& type, std::vector<ToneSource> sources) {
  auto& p = types_[norm(type)];
  p.sources.insert(p.sources.end(), sources.begin(), sources.end());
}

void BladePassTable::learn(const std::string& type, double f0) {
  if (norm(type).empty() || !(f0 > 0)) return;
  types_[norm(type)].learned.push_back(f0);
}

json::object BladePassTable::learned_json() const {
  json::object o;
  for (auto& [type, p] : types_) {
    if (p.learned.empty()) continue;
    json::array a;
    for (double f : p.learned) a.push_back(std::round(f * 100) / 100);
    o[type] = a;
  }
  return o;
}

void BladePassTable::merge_learned(const json::value& v) {
  if (!v.is_object()) return;
  for (auto& [k, arr] : v.as_object()) {
    if (!arr.is_array()) continue;
    for (auto& x : arr.as_array()) learn(std::string(k), as_number(x));
  }
}

bool BladePassTable::known(std::string_view type) const { return !ranges(type).empty(); }

std::vector<std::pair<double, double>> BladePassTable::ranges(std::string_view type, const PriorOptions& o) const {
  std::vector<std::pair<double, double>> out;
  auto it = types_.find(norm(type));
  if (it == types_.end()) return out;
  const TypePrior& p = it->second;
  // A learned-only type constrains once it has a few observations; until then it stays open.
  if (p.sources.empty() && int(p.learned.size()) < o.learned_min) return out;
  for (auto& s : p.sources) out.emplace_back(s.f_lo(), s.f_hi());
  for (double f : p.learned) out.emplace_back(f * (1 - o.learned_halfwidth), f * (1 + o.learned_halfwidth));
  return out;
}

bool BladePassTable::compatible(std::string_view type, const std::vector<double>& f0s, const PriorOptions& o) const {
  const auto rs = ranges(type, o);
  if (rs.empty() || f0s.empty()) return true;
  auto in = [&](double f) {
    for (auto& [lo, hi] : rs)
      if (f >= lo * (1 - o.margin) && f <= hi * (1 + o.margin)) return true;
    return false;
  };
  for (std::size_t i = 0; i < f0s.size(); ++i) {
    if (!(f0s[i] > 0)) continue;
    if (i == 0) {
      for (double k : o.multiples)
        if (in(f0s[i] / k)) return true;
    } else if (in(f0s[i])) {
      return true;
    }
  }
  return false;
}

bool f0_consistent(const std::vector<double>& a, const std::vector<double>& b, double tol) {
  if (a.empty() || b.empty()) return true;
  static constexpr double RATIOS[] = {1.0, 2.0, 3.0, 0.5, 1.0 / 3, 1.5, 2.0 / 3};
  for (double x : a)
    for (double y : b) {
      if (!(x > 0) || !(y > 0)) continue;
      for (double r : RATIOS)
        if (std::abs(x / (y * r) - 1) <= tol) return true;
    }
  return false;
}

}  // namespace geo::air
