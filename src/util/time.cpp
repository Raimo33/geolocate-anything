#include "util/time.hpp"

#include <charconv>
#include <cstdio>

namespace geo {

namespace {

bool digits(std::string_view s, std::size_t pos, std::size_t n, int& out) {
  if (pos + n > s.size()) return false;
  int v = 0;
  for (std::size_t i = pos; i < pos + n; ++i) {
    char c = s[i];
    if (c < '0' || c > '9') return false;
    v = v * 10 + (c - '0');
  }
  out = v;
  return true;
}

bool valid_date(int y, int m, int d) {
  if (m < 1 || m > 12 || d < 1) return false;
  static constexpr int mdays[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
  int lim = mdays[m - 1] + (m == 2 && ((y % 4 == 0 && y % 100 != 0) || y % 400 == 0));
  return d <= lim;
}

// Parses "[T ]HH:MM[:SS[.fff]][ ][Z|+HH[:]MM]" starting at pos. Missing time = midnight.
bool parse_time_tail(std::string_view s, std::size_t pos, WallTime& w, bool require_seconds) {
  int hh = 0, mm = 0, ss = 0;
  if (pos < s.size() && (s[pos] == 'T' || s[pos] == ' ')) {
    ++pos;
    if (!digits(s, pos, 2, hh) || pos + 2 >= s.size() || s[pos + 2] != ':' || !digits(s, pos + 3, 2, mm)) return false;
    pos += 5;
    if (pos < s.size() && s[pos] == ':') {
      if (!digits(s, pos + 1, 2, ss)) return false;
      pos += 3;
      if (pos < s.size() && (s[pos] == '.' || s[pos] == ',')) {
        ++pos;
        double f = 0, scale = 0.1;
        std::size_t start = pos;
        while (pos < s.size() && s[pos] >= '0' && s[pos] <= '9') {
          f += (s[pos] - '0') * scale;
          scale *= 0.1;
          ++pos;
        }
        if (pos == start) return false;
        w.frac = f;
      }
    } else if (require_seconds) {
      return false;
    }
  }
  if (hh > 24 || mm > 59 || ss > 60) return false;
  w.secs += hh * 3600 + mm * 60 + ss;
  if (pos < s.size() && s[pos] == ' ' && pos + 1 < s.size() && (s[pos + 1] == '+' || s[pos + 1] == '-' || s[pos + 1] == 'Z')) ++pos;
  if (pos < s.size()) {
    char c = s[pos];
    if (c == 'Z' || c == 'z') {
      w.has_offset = true;
      w.offset_s = 0;
      ++pos;
    } else if (c == '+' || c == '-') {
      int oh = 0, om = 0;
      if (!digits(s, pos + 1, 2, oh)) return false;
      std::size_t p = pos + 3;
      if (p < s.size() && s[p] == ':') ++p;
      if (p < s.size()) {
        if (!digits(s, p, 2, om)) return false;
        p += 2;
      }
      w.has_offset = true;
      w.offset_s = (c == '-' ? -1 : 1) * (oh * 3600 + om * 60);
      pos = p;
    }
  }
  while (pos < s.size() && (s[pos] == ' ' || s[pos] == '\r' || s[pos] == '"')) ++pos;
  return pos == s.size();
}

}  // namespace

std::optional<Month> Month::parse(std::string_view key) {
  int y, m;
  if (key.size() < 7 || !digits(key, 0, 4, y) || key[4] != '-' || !digits(key, 5, 2, m) || m < 1 || m > 12) return std::nullopt;
  return Month{y, m};
}

std::string Month::key() const {
  char b[16];
  std::snprintf(b, sizeof b, "%04d-%02d", y, m);
  return b;
}

std::vector<std::string> months_between(std::string_view a, std::string_view b) {
  std::vector<std::string> out;
  auto x = Month::parse(a), y = Month::parse(b);
  if (!x || !y) return out;
  for (Month m = *x; m <= *y; m = m.next()) out.push_back(m.key());
  return out;
}

std::string iso(double t) {
  i64 us = static_cast<i64>(std::nearbyint(t * 1e6));
  i64 s = floor_div(us, 1000000);
  i64 d = floor_div(s, 86400), r = s - d * 86400;
  auto c = civil_from_days(d);
  char b[40];
  std::snprintf(b, sizeof b, "%04d-%02u-%02u %02d:%02d:%02d UTC", c.y, c.m, c.d, int(r / 3600), int(r / 60 % 60), int(r % 60));
  return b;
}

std::string iso_date(i64 t) {
  auto c = civil_from_days(floor_div(t, 86400));
  char b[16];
  std::snprintf(b, sizeof b, "%04d-%02u-%02u", c.y, c.m, c.d);
  return b;
}

double now_seconds() {
  using namespace std::chrono;
  return duration<double>(system_clock::now().time_since_epoch()).count();
}

std::string now_isoformat() {
  i64 s = static_cast<i64>(std::floor(now_seconds()));
  i64 d = floor_div(s, 86400), r = s - d * 86400;
  auto c = civil_from_days(d);
  char b[40];
  std::snprintf(b, sizeof b, "%04d-%02u-%02uT%02d:%02d:%02d+00:00", c.y, c.m, c.d, int(r / 3600), int(r / 60 % 60), int(r % 60));
  return b;
}

std::optional<WallTime> parse_iso_datetime(std::string_view s) {
  while (!s.empty() && (s.front() == ' ' || s.front() == '"')) s.remove_prefix(1);
  int y, m, d;
  if (!digits(s, 0, 4, y) || s.size() < 10 || (s[4] != '-' && s[4] != '/') || s[7] != s[4] || !digits(s, 5, 2, m) || !digits(s, 8, 2, d))
    return std::nullopt;
  if (!valid_date(y, m, d)) return std::nullopt;
  WallTime w;
  w.secs = days_from_civil(y, m, d) * 86400;
  if (!parse_time_tail(s, 10, w, false)) return std::nullopt;
  return w;
}

std::optional<WallTime> parse_dmy_datetime(std::string_view s) {
  int y, m, d;
  if (s.size() < 10 || !digits(s, 0, 2, d) || s[2] != '/' || !digits(s, 3, 2, m) || s[5] != '/' || !digits(s, 6, 4, y)) return std::nullopt;
  if (!valid_date(y, m, d)) return std::nullopt;
  WallTime w;
  w.secs = days_from_civil(y, m, d) * 86400;
  if (!parse_time_tail(s, 10, w, true)) return std::nullopt;
  return w;
}

std::optional<WallTime> parse_dmony_datetime(std::string_view s) {
  static constexpr std::string_view MON[] = {"jan", "feb", "mar", "apr", "may", "jun", "jul", "aug", "sep", "oct", "nov", "dec"};
  int d, y;
  if (s.size() < 9 || !digits(s, 0, 2, d) || s[2] != '-' || s[6] != '-' || !digits(s, 7, 2, y)) return std::nullopt;
  int m = 0;
  while (m < 12 && !(std::tolower(static_cast<unsigned char>(s[3])) == MON[m][0] &&
                     std::tolower(static_cast<unsigned char>(s[4])) == MON[m][1] &&
                     std::tolower(static_cast<unsigned char>(s[5])) == MON[m][2]))
    ++m;
  if (m == 12 || !valid_date(2000 + y, m + 1, d)) return std::nullopt;
  WallTime w;
  w.secs = days_from_civil(2000 + y, unsigned(m + 1), unsigned(d)) * 86400;
  if (!parse_time_tail(s, 9, w, true)) return std::nullopt;
  return w;
}

std::optional<i64> parse_iso_utc(std::string_view s) {
  auto w = parse_iso_datetime(s);
  if (!w) return std::nullopt;
  double t = w->utc();
  return static_cast<i64>(std::trunc(t));
}

LocalToUtc::LocalToUtc(std::string_view tz) : zone_(std::chrono::locate_zone(tz)) {}

double LocalToUtc::operator()(double secs) {
  using namespace std::chrono;
  if (std::isnan(secs)) return NaN;
  const double pm = max_;
  if (secs < pm - 600) fold_ = pm;  // jump back: the repeated hour starts
  const bool second_pass = secs <= fold_;
  if (secs > max_) max_ = secs;

  const double whole_d = std::floor(secs);
  const i64 whole = static_cast<i64>(whole_d);
  const double frac = secs - whole_d;
  if (whole >= lo_ && whole < hi_) return static_cast<double>(whole - off_) + frac;

  auto info = zone_->get_info(local_seconds{seconds{whole}});
  if (info.result == local_info::nonexistent) return NaN;
  if (info.result == local_info::ambiguous) {
    const auto& pick = second_pass ? info.second : info.first;
    return static_cast<double>(whole - pick.offset.count()) + frac;
  }
  // Unique: cache the local interval in which this offset applies unambiguously.
  const auto& si = info.first;
  constexpr i64 BIG = i64(1) << 50;
  const i64 off = si.offset.count();
  const i64 b = si.begin.time_since_epoch().count(), e = si.end.time_since_epoch().count();
  i64 lo = -BIG, hi = BIG;
  if (b > -BIG) {
    i64 prev_off = zone_->get_info(sys_seconds{seconds{b - 1}}).offset.count();
    lo = b + std::max(off, prev_off);
  }
  if (e < BIG) {
    i64 next_off = zone_->get_info(sys_seconds{seconds{e}}).offset.count();
    hi = e + std::min(off, next_off);
  }
  lo_ = lo;
  hi_ = hi;
  off_ = off;
  return static_cast<double>(whole - off) + frac;
}

}  // namespace geo
