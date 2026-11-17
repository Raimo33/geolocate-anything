#include "tzclue.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <map>
#include <mutex>
#include <set>
#include <stdexcept>

#include "data.hpp"
#include "grids.hpp"
#include "util/json.hpp"
#include "util/text.hpp"

namespace geo::tzclue {

namespace {

// Offsets in use are at least a quarter hour apart (UTC+5:30 / +5:45), so a device clock
// that is right to within this many seconds shows its offset unambiguously.
constexpr double TOLERANCE_S = 300;

std::string trim(std::string_view s) {
  while (!s.empty() && (std::isspace(static_cast<unsigned char>(s.front())) || s.front() == '"' || s.front() == '\0')) s.remove_prefix(1);
  while (!s.empty() && (std::isspace(static_cast<unsigned char>(s.back())) || s.back() == '"' || s.back() == '\0')) s.remove_suffix(1);
  return std::string(s);
}

bool is_digit(char c) { return c >= '0' && c <= '9'; }

// "Mon Mar 10 15:04:43 2003" (asctime; AVI IDIT, some RIFF ICRD) -> "2003-03-10 15:04:43".
std::optional<std::string> from_asctime(const std::string& s) {
  static const char* MON[] = {"jan", "feb", "mar", "apr", "may", "jun", "jul", "aug", "sep", "oct", "nov", "dec"};
  char wd[8] = {}, mo[8] = {};
  int d, hh, mm, ss, y;
  if (std::sscanf(s.c_str(), "%3s %3s %d %d:%d:%d %d", wd, mo, &d, &hh, &mm, &ss, &y) != 7) return std::nullopt;
  const std::string m = lower(mo);
  for (int i = 0; i < 12; ++i)
    if (m == MON[i]) {
      char b[32];
      std::snprintf(b, sizeof b, "%04d-%02d-%02d %02d:%02d:%02d", y, i + 1, d, hh, mm, ss);
      return std::string(b);
    }
  return std::nullopt;
}

bool ends_with_z(const std::string& s) { return !s.empty() && (s.back() == 'Z' || s.back() == 'z'); }

// A stamp's kind from what it says and where it came from.
Kind kind_of(const std::string& key, const std::string& raw, const WallTime& w) {
  if (key == "creation_time" || key == "date_utc") return w.has_offset || ends_with_z(raw) ? Kind::Utc : Kind::Local;  // FFmpeg writes Z for mvhd/DateUTC
  if (!w.has_offset) return Kind::Local;
  return ends_with_z(raw) ? Kind::Utc : Kind::Explicit;
}

bool time_only(const std::string& s) {
  return s.size() >= 8 && is_digit(s[0]) && is_digit(s[1]) && !is_digit(s[2]) && is_digit(s[3]) && is_digit(s[4]) && !is_digit(s[5]) &&
         is_digit(s[6]) && is_digit(s[7]);
}

// ---------------------------------------------------------------- zone pieces

struct Piece {
  std::string grid, tz;
  std::optional<std::string> from, to;
  std::vector<std::string> countries, subdivisions;
  double km2 = 0;
  bool active(const std::string& d) const { return (!from || *from <= d) && (!to || d < *to); }
};

const std::vector<Piece>& pieces() {
  static const std::vector<Piece> out = [] {
    std::vector<Piece> v;
    auto raw = parse_json(data::tz_areas_json());
    auto strs = [](const json::value* a) {
      std::vector<std::string> r;
      if (a && a->is_array())
        for (auto& x : a->get_array())
          if (x.is_string()) r.emplace_back(x.get_string());
      return r;
    };
    for (auto& f : raw.at("features").get_array()) {
      const auto& p = f.at("properties");
      Piece pc;
      pc.grid = jstr(p, "grid");
      pc.tz = jstr(p, "tz");
      if (auto* x = jget(p, "from"); x && x->is_string()) pc.from = std::string(x->get_string());
      if (auto* x = jget(p, "to"); x && x->is_string()) pc.to = std::string(x->get_string());
      pc.countries = strs(jget(p, "countries"));
      pc.subdivisions = strs(jget(p, "subdivisions"));
      pc.km2 = as_number(p.at("km2"), 0);
      v.push_back(std::move(pc));
    }
    return v;
  }();
  return out;
}

void add_unique(std::vector<std::string>& out, const std::vector<std::string>& v) {
  for (auto& x : v)
    if (std::find(out.begin(), out.end(), x) == out.end()) out.push_back(x);
}

std::string hours(double s) {
  char b[32];
  std::snprintf(b, sizeof b, "%+.2f h", s / 3600);
  return b;
}

std::string secs(double s) {
  char b[32];
  std::snprintf(b, sizeof b, "%.0f s", s);
  return b;
}

// "UA, RO, GR, US (AZ, UT)": countries split between zones name the parts in these zones
// (merged over all of them, and only when they are few).
std::string countries_of(const std::vector<Zone>& zs, std::size_t max = 12) {
  std::vector<std::string> order;
  std::map<std::string, std::vector<std::string>> subs;
  for (auto& z : zs)
    for (auto& c : z.countries) {
      add_unique(order, {c});
      for (auto& s : z.subdivisions)
        if (s.rfind(c + "-", 0) == 0) add_unique(subs[c], {s.substr(c.size() + 1)});
    }
  std::vector<std::string> cs;
  for (auto& c : order) {
    auto& v = subs[c];
    cs.push_back(v.empty() || v.size() > 6 ? c : c + " (" + join(v, ", ") + ")");
  }
  if (cs.size() > max) {
    const auto n = cs.size() - max;
    cs.resize(max);
    cs.push_back("+" + std::to_string(n) + " more");
  }
  return cs.empty() ? std::string("(small areas only)") : join(cs, ", ");
}

}  // namespace

// Every UTC offset some zone in data/tz_areas.json had at instant t, plus 0: a wall clock
// minus true UTC is only an offset when it is one of these (no zone ever used +3:15).
static std::vector<int> offsets_in_use(double t) {
  std::vector<int> out{0};
  std::set<std::string> seen;
  for (auto& p : pieces())
    if (seen.insert(p.tz).second) try {
        const int o = offset_at(p.tz, t);
        if (std::find(out.begin(), out.end(), o) == out.end()) out.push_back(o);
      } catch (const std::runtime_error&) {
      }
  return out;
}

const char* kind_name(Kind k) {
  switch (k) {
    case Kind::Explicit: return "explicit";
    case Kind::Utc: return "utc";
    case Kind::Local: return "local";
  }
  return "?";
}

std::string fmt_offset(int s) {
  if (s == 0) return "UTC";
  char b[16];
  const int a = std::abs(s);
  std::snprintf(b, sizeof b, "UTC%c%02d:%02d", s < 0 ? '-' : '+', a / 3600, a / 60 % 60);
  return b;
}

int offset_at(const std::string& tz, double t) {
  using namespace std::chrono;
  static std::mutex mu;
  static std::map<std::string, const time_zone*> cache;
  const time_zone* z;
  {
    std::lock_guard lk(mu);
    auto it = cache.find(tz);
    if (it == cache.end()) {
      try {
        it = cache.emplace(tz, locate_zone(tz)).first;
      } catch (const std::exception&) {
        throw std::runtime_error("time zone '" + tz + "' is not in the system's tz database");
      }
    }
    z = it->second;
  }
  return static_cast<int>(z->get_info(sys_seconds{seconds{static_cast<i64>(std::floor(t))}}).offset.count());
}

std::vector<std::string> known_zones() {
  std::vector<std::string> out;
  for (auto& p : pieces()) add_unique(out, {p.tz});
  return out;
}

std::optional<WallTime> parse_stamp(std::string_view in) {
  std::string s = trim(in);
  if (s.size() < 16) return std::nullopt;  // "YYYY-MM-DD HH:MM" at least
  if (auto a = from_asctime(s)) s = *a;
  // EXIF / QuickTime-style "YYYY:MM:DD HH:MM:SS".
  if (is_digit(s[0]) && s[4] == ':' && s[7] == ':') s[4] = s[7] = '-';
  // Times written with '-' or '.' between fields ("14-22-11", seen in BWF).
  if (s.size() >= 19 && (s[10] == 'T' || s[10] == ' ') && (s[13] == '-' || s[13] == '.') && s[16] == s[13]) s[13] = s[16] = ':';
  if (s[10] != 'T' && s[10] != ' ') return std::nullopt;
  auto w = parse_iso_datetime(s);
  if (!w) return std::nullopt;
  return w;
}

std::vector<Stamp> stamps(const audio::FileClues& c) {
  // Capture-time fields, most specific first. Release / encoding / tagging dates are left
  // out: they say when a file was made from the recording, not when it was recorded.
  static const char* KEYS[] = {
      "com.apple.quicktime.creationdate",  // QuickTime keys (iPhone, iPad): always with offset
      "sony:creationdate",                 // Sony NonRealTimeMeta XML (XAVC S / XAVC HS)
      "xmp:createdate", "exif:datetimeoriginal", "photoshop:datecreated", "xmpdm:shotdate",
      "date_recorded",                     // Matroska / WebM tag
      "creation_time",                     // mvhd / tkhd (MP4, MOV, 3GP), Matroska DateUTC, ASF, MXF, AVI IDIT
      "date",                              // QuickTime ©day, ID3 TDRC/TYER+TDAT+TIME, Vorbis DATE, RIFF ICRD
      "datetimeoriginal", "creationdate", "recording_time", "date_time_original",
  };
  std::vector<Stamp> out;
  const auto& t = c.all_tags;
  auto get = [&](const char* k) -> const std::string* {
    auto it = t.find(k);
    return it == t.end() ? nullptr : &it->second;
  };
  for (const char* k : KEYS) {
    const std::string* v = get(k);
    if (!v) continue;
    std::string raw = trim(*v);
    std::string key = k;
    // BWF: FFmpeg's wav demuxer puts bext's origination date in "date" and its
    // origination *time* alone in "creation_time". Local time by the EBU spec.
    if (key == "creation_time" && time_only(raw)) {
      const std::string* d = get("date");
      if (!d || trim(*d).size() < 10) continue;
      raw = trim(*d).substr(0, 10) + " " + raw.substr(0, 8);
      key = "bwf:origination";
      if (auto w = parse_stamp(raw)) out.push_back({key, raw, Kind::Local, *w});
      continue;
    }
    if (auto w = parse_stamp(raw)) out.push_back({key, raw, kind_of(key, raw, *w), *w});
  }
  return out;
}

Result infer(const audio::FileClues& c, std::optional<std::string> grid, double start_utc, double duration_s) {
  Result r;
  r.grid = std::move(grid);
  const bool have_hum = std::isfinite(start_utc);
  const double end_utc = start_utc + (std::isfinite(duration_s) ? duration_s : 0);
  double explicit_at = NAN;
  const std::vector<int> in_use = have_hum ? offsets_in_use(start_utc) : std::vector<int>{};

  for (auto& s : stamps(c)) {
    Evidence e{s.key, s.raw, s.kind, std::nullopt, NAN, false, {}};
    if (s.kind == Kind::Explicit) {
      e.offset_s = s.w.offset_s;
      const double instant = s.w.utc();
      if (!have_hum) {
        e.used = true;
        e.note = "states the device's zone, " + fmt_offset(s.w.offset_s) + " (not checked: no hum match)";
        if (!std::isfinite(explicit_at)) explicit_at = instant;
      } else {
        e.residual_s = std::min(std::fabs(instant - start_utc), std::fabs(instant - end_utc));
        if (e.residual_s <= TOLERANCE_S) {
          e.used = true;
          e.note = "states " + fmt_offset(s.w.offset_s) + " and agrees with the hum time to within " + secs(e.residual_s);
        } else {
          e.note = "states " + fmt_offset(s.w.offset_s) + " but is " + hours(instant - start_utc) +
                   " from the hum time (wrong clock, or a later edit or export): not used";
        }
      }
    } else if (have_hum) {
      // The wall clock minus true UTC, against the first sample or (some devices stamp
      // when they finish writing) the last.
      int best_o = 0;
      double best_r = INFINITY;
      for (double d : {s.w.naive() - start_utc, s.w.naive() - end_utc})
        for (int o : in_use)
          if (const double res = std::fabs(d - o); res < best_r) best_r = res, best_o = o;
      e.residual_s = best_r;
      if (best_r > TOLERANCE_S) {
        e.note = "is " + hours(s.w.naive() - start_utc) + " from the hum time: not a time-zone offset (wrong clock, re-encoded or edited)";
      } else if (best_o == 0) {
        e.offset_s = 0;
        e.note = std::string(s.kind == Kind::Utc ? "is UTC, as the format specifies" : "equals UTC") + " (within " + secs(best_r) +
                 " of the hum time): no zone clue";
      } else {
        e.offset_s = best_o;
        e.used = true;
        e.note = std::string(s.kind == Kind::Utc ? "should be UTC but holds local time" : "holds local time") + ": the device clock was " +
                 fmt_offset(best_o) + " (within " + secs(best_r) + ")";
      }
    } else {
      e.note = "no zone stated; needs the hum's time to compare against";
    }
    r.evidence.push_back(std::move(e));
  }

  // Offsets to narrow with: stated ones outrank inferred ones; disagreement keeps all.
  for (Kind want : {Kind::Explicit, Kind::Local}) {
    for (auto& e : r.evidence)
      if (e.used && e.offset_s && (want == Kind::Explicit ? e.kind == Kind::Explicit : e.kind != Kind::Explicit) &&
          std::find(r.offsets.begin(), r.offsets.end(), *e.offset_s) == r.offsets.end())
        r.offsets.push_back(*e.offset_s);
    if (!r.offsets.empty()) break;
  }
  if (r.offsets.size() > 1) {
    std::vector<std::string> os;
    for (int o : r.offsets) os.push_back(fmt_offset(o));
    r.notes.push_back("metadata clocks disagree (" + join(os, ", ") + "): every one is kept");
  }
  if (r.offsets.empty()) return r;

  r.at = have_hum ? start_utc : explicit_at;
  if (!have_hum) r.grid.reset();  // only a match says which grid
  const std::string day = grids::day_of(r.at);
  std::map<std::string, Zone> by_tz;
  // Zones whose clock *in the other season* shows the observed offset: where a clock set by
  // hand that missed the last DST change would point instead.
  std::vector<Zone> stuck;
  constexpr double HALF_YEAR_S = 182.5 * 86400;
  for (auto& p : pieces()) {
    if ((r.grid && p.grid != *r.grid) || !p.active(day)) continue;
    int o;
    try {
      o = offset_at(p.tz, r.at);
    } catch (const std::runtime_error&) {
      continue;
    }
    r.grid_km2 += p.km2;
    Zone zp{p.tz, p.countries, p.subdivisions, p.km2};
    if (std::find(r.offsets.begin(), r.offsets.end(), o) == r.offsets.end()) {
      for (double t2 : {r.at - HALF_YEAR_S, r.at + HALF_YEAR_S})
        if (std::find(r.offsets.begin(), r.offsets.end(), offset_at(p.tz, t2)) != r.offsets.end()) {
          stuck.push_back(std::move(zp));
          break;
        }
      continue;
    }
    auto& z = by_tz[p.tz];
    z.tz = p.tz;
    add_unique(z.countries, p.countries);
    add_unique(z.subdivisions, p.subdivisions);
    z.km2 += p.km2;
    r.km2 += p.km2;
  }
  for (auto& [_, z] : by_tz) r.zones.push_back(std::move(z));
  std::sort(r.zones.begin(), r.zones.end(), [](auto& a, auto& b) { return a.km2 > b.km2; });

  const std::string where = r.grid ? grids::load_grids().get(*r.grid).name : std::string("any grid");
  if (r.zones.empty()) {
    r.notes.push_back("no part of " + where + " was on " + fmt_offset(r.offsets[0]) + " on " + day +
                      ": the device clock was set for another zone (a traveller's home zone) or by hand");
    return r;
  }
  char b[256];
  std::snprintf(b, sizeof b, "%s leaves %.0f%% of %s (%.0f of %.0f thousand km²): %s", fmt_offset(r.offsets[0]).c_str(),
                r.grid_km2 > 0 ? 100 * r.km2 / r.grid_km2 : 0.0, where.c_str(), r.km2 / 1000, r.grid_km2 / 1000, countries_of(r.zones).c_str());
  r.notes.emplace_back(b);
  const bool inferred = std::none_of(r.evidence.begin(), r.evidence.end(), [](auto& e) { return e.used && e.kind == Kind::Explicit; });
  if (inferred)
    r.notes.push_back("assumes the device clock showed the local time where it recorded (not a traveller's home time)");
  if (inferred && !stuck.empty())
    r.notes.push_back("or, if the clock was set by hand and missed the last DST change: " + countries_of(stuck, 8));
  return r;
}

}  // namespace geo::tzclue
