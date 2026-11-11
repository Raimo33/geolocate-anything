// Calendar arithmetic on UTC epoch seconds, month keys, ISO formatting/parsing and
// local wall-clock -> UTC conversion (std::chrono time-zone database).
#pragma once

#include <chrono>
#include <cmath>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace geo {

using i64 = std::int64_t;
inline constexpr double NaN = std::numeric_limits<double>::quiet_NaN();

constexpr i64 days_from_civil(i64 y, unsigned m, unsigned d) {
  y -= m <= 2;
  const i64 era = (y >= 0 ? y : y - 399) / 400;
  const unsigned yoe = static_cast<unsigned>(y - era * 400);
  const unsigned doy = (153 * (m > 2 ? m - 3 : m + 9) + 2) / 5 + d - 1;
  const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  return era * 146097 + static_cast<i64>(doe) - 719468;
}

struct Civil {
  int y;
  unsigned m, d;
};
constexpr Civil civil_from_days(i64 z) {
  z += 719468;
  const i64 era = (z >= 0 ? z : z - 146096) / 146097;
  const unsigned doe = static_cast<unsigned>(z - era * 146097);
  const unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
  const i64 y = static_cast<i64>(yoe) + era * 400;
  const unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
  const unsigned mp = (5 * doy + 2) / 153;
  const unsigned d = doy - (153 * mp + 2) / 5 + 1;
  const unsigned m = mp < 10 ? mp + 3 : mp - 9;
  return {static_cast<int>(y + (m <= 2)), m, d};
}

constexpr i64 floor_div(i64 a, i64 b) { return a / b - ((a % b != 0) && ((a < 0) != (b < 0))); }

// A UTC calendar month. Ordered; key() is "YYYY-MM".
struct Month {
  int y = 1970, m = 1;
  static Month of(i64 t) {
    auto c = civil_from_days(floor_div(t, 86400));
    return {c.y, static_cast<int>(c.m)};
  }
  static std::optional<Month> parse(std::string_view key);
  i64 t0() const { return days_from_civil(y, m, 1) * 86400; }
  i64 t1() const { return next().t0(); }
  Month next() const { return m == 12 ? Month{y + 1, 1} : Month{y, m + 1}; }
  Month prev() const { return m == 1 ? Month{y - 1, 12} : Month{y, m - 1}; }
  std::string key() const;
  auto operator<=>(const Month&) const = default;
};

std::vector<std::string> months_between(std::string_view a, std::string_view b);
inline std::pair<i64, i64> month_bounds(std::string_view key) {
  auto m = Month::parse(key).value();
  return {m.t0(), m.t1()};
}
inline std::string month_key(i64 t) { return Month::of(t).key(); }

// "2019-04-17 09:41:07 UTC" (seconds truncated, like datetime.strftime).
std::string iso(double t);
// "2019-04-17" / "2019-04-17T09:41:07+00:00" (now, seconds precision).
std::string iso_date(i64 t);
std::string now_isoformat();
double now_seconds();

// ISO 8601 date/time as used on the command line and in the API: "2019-04-17",
// "2019-04-17T09:41", "2019-04-17 09:41:07.5Z", "...+02:00". Naive = UTC.
// Returns whole epoch seconds (truncated toward zero, like int(dt.timestamp())).
std::optional<i64> parse_iso_utc(std::string_view s);

// A wall-clock timestamp as found in CSV files.
struct WallTime {
  i64 secs = 0;        // seconds since 1970-01-01 of the wall clock (offset not applied)
  double frac = 0.0;   // fractional second
  bool has_offset = false;
  int offset_s = 0;    // UTC offset written in the timestamp (local = utc + offset)
  double utc() const { return static_cast<double>(secs - (has_offset ? offset_s : 0)) + frac; }
  double naive() const { return static_cast<double>(secs) + frac; }
};
// "YYYY-MM-DD[T ]HH:MM[:SS[.ffffff]][ ][Z|+HH[:]MM]"; '/' also accepted between date parts.
std::optional<WallTime> parse_iso_datetime(std::string_view s);
// "DD/MM/YYYY HH:MM:SS"
std::optional<WallTime> parse_dmy_datetime(std::string_view s);
// "DD-Mon-YY HH:MM:SS" (English month abbreviation, two-digit year = 20YY), as OSIsoft PI
// exports write it (ERCOT's 2011 frequency files).
std::optional<WallTime> parse_dmony_datetime(std::string_view s);

// Naive local wall-clock seconds -> UTC seconds for chronologically ordered logs.
// At the autumn change the wall clock repeats an hour; a row is in the repeated
// (standard-time) pass when an earlier row already showed a later wall time, which a
// running maximum detects without the whole file. Spring-gap times return NaN.
class LocalToUtc {
 public:
  explicit LocalToUtc(std::string_view tz);
  double operator()(double naive_secs);  // NaN in, NaN out
 private:
  const std::chrono::time_zone* zone_;
  double max_ = -INFINITY, fold_ = -INFINITY;
  // Cache of the current unique-offset interval in local seconds [lo, hi).
  i64 lo_ = 1, hi_ = 0, off_ = 0;
};

}  // namespace geo
