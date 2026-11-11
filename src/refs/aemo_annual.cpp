#include "refs/aemo_annual.hpp"

#include <algorithm>
#include <cmath>
#include <optional>
#include <stdexcept>

#include "refs/sheets.hpp"
#include "store/store.hpp"
#include "util/text.hpp"

namespace geo::refs {

namespace {

constexpr double EXCEL_EPOCH_DAYS = 25569;  // 1970-01-01 as an Excel (1900-system) serial
constexpr i64 MARGIN_S = 16;                // a month's accumulator reaches this far past its ends

double excel_to_unix(double serial) { return (serial - EXCEL_EPOCH_DAYS) * 86400.0; }

// A day from header text: "2023-01-01", "1/01/2023" (day first, as AEMO writes), "1-Jan-2023",
// "01 Jan 2023", optionally followed by a time. UNIX seconds of its midnight.
std::optional<i64> text_day(std::string_view s) {
  s = trim(s);
  static const char* MON[] = {"jan", "feb", "mar", "apr", "may", "jun", "jul", "aug", "sep", "oct", "nov", "dec"};
  std::vector<std::string> part;
  std::string cur;
  auto digits = [&] { return !cur.empty() && std::all_of(cur.begin(), cur.end(), [](char x) { return x >= '0' && x <= '9'; }); };
  for (char c : s) {
    if (c == '-' || c == '/' || c == '.' || c == ' ' || (c == 'T' && digits())) {
      if (!cur.empty()) part.push_back(cur), cur.clear();
      if (c == ' ' || c == 'T') {
        if (part.size() >= 3) break;
      }
      continue;
    }
    cur += char(std::tolower(static_cast<unsigned char>(c)));
  }
  if (!cur.empty() && part.size() < 3) part.push_back(cur);
  if (part.size() < 3) return std::nullopt;
  auto as_int = [](const std::string& x) -> std::optional<long long> { return to_int(x); };
  auto month = [&](const std::string& x) -> std::optional<int> {
    if (auto n = as_int(x)) return int(*n);
    for (int i = 0; i < 12; ++i)
      if (x.starts_with(MON[i])) return i + 1;
    return std::nullopt;
  };
  long long y, d;
  std::optional<int> m;
  if (part[0].size() == 4 && as_int(part[0])) {  // Y-M-D
    y = *as_int(part[0]);
    m = month(part[1]);
    auto dd = as_int(part[2]);
    if (!dd) return std::nullopt;
    d = *dd;
  } else {  // D-M-Y
    auto dd = as_int(part[0]), yy = as_int(part[2]);
    if (!dd || !yy) return std::nullopt;
    d = *dd;
    y = *yy < 100 ? 2000 + *yy : *yy;
    m = month(part[1]);
  }
  if (!m || *m < 1 || *m > 12 || d < 1 || d > 31 || y < 1990 || y > 2100) return std::nullopt;
  return days_from_civil(y, unsigned(*m), unsigned(d)) * 86400;
}

// "HH:MM[:SS[.fff]]" at the start of s (after any date) -> seconds into the day.
std::optional<double> text_time_of_day(std::string_view s) {
  s = trim(s);
  auto sp = s.rfind(' ');
  if (sp != std::string_view::npos) s = s.substr(sp + 1);
  else if (auto t = s.find('T'); t != std::string_view::npos) s = s.substr(t + 1);
  auto f = split(s, ':');
  if (f.size() < 2) return std::nullopt;
  auto h = to_double(f[0]), m = to_double(f[1]);
  auto sec = f.size() > 2 ? to_double(f[2]) : std::optional<double>(0.0);
  if (!h || !m || !sec || *h < 0 || *h > 24 || *m < 0 || *m >= 60) return std::nullopt;
  return *h * 3600 + *m * 60 + *sec;
}

// A header cell naming a day: an Excel date serial or date text.
std::optional<i64> header_day(const sheets::Cell& c) {
  if (c.is_num()) {
    if (c.num >= 30000 && c.num < 80000) return i64(std::floor(excel_to_unix(std::floor(c.num)) + 0.5));
    return std::nullopt;
  }
  return text_day(c.str);
}

struct TimeCell {
  std::optional<double> abs;  // local UNIX seconds, when the cell holds a date too
  std::optional<double> tod;  // seconds into the day
  std::optional<int> dom;     // day of the month, when the cell starts with one ("17 - 00:00:04")
};

// "DD - HH:MM:SS": AEMO's yearly workbooks hold one column per month (headed by its 1st)
// and label each row with the day of the month and the time.
std::optional<int> text_day_of_month(std::string_view s) {
  s = trim(s);
  auto dash = s.find('-');
  if (dash == std::string_view::npos || dash == 0 || s.find(':') < dash) return std::nullopt;
  auto d = to_int(std::string(trim(s.substr(0, dash))));
  if (!d || *d < 1 || *d > 31) return std::nullopt;
  return int(*d);
}
TimeCell time_cell(const sheets::Cell& c) {
  TimeCell t;
  if (c.is_num()) {
    if (c.num >= 0 && c.num < 1.0 + 1e-9) {
      t.tod = std::round(c.num * 86400.0 * 1000) / 1000;
    } else if (c.num >= 30000 && c.num < 80000) {
      t.abs = std::round(excel_to_unix(c.num) * 1000) / 1000;
      t.tod = std::fmod(*t.abs, 86400.0);
    }
    return t;
  }
  t.tod = text_time_of_day(c.str);
  if (auto d = text_day(c.str); d && t.tod) t.abs = double(*d) + *t.tod;
  else if (t.tod) t.dom = text_day_of_month(c.str);
  return t;
}

bool contains(const std::string& s, std::string_view what) { return lower(s).find(what) != std::string::npos; }

}  // namespace

std::string aemo_annual_grid(const std::string& filename) {
  auto base = lower(filename.substr(filename.find_last_of("/\\") == std::string::npos ? 0 : filename.find_last_of("/\\") + 1));
  return base.find("tas") != std::string::npos ? "australia_tas" : "australia_nem";
}

AemoAnnual parse_aemo_annual(std::span<const std::byte> file, int utc_offset_s, int shift_s) {
  AemoAnnual out;
  out.format = sheets::inspect(file).format;

  // Rows arrive in order: the header is the first row naming a time column or two days.
  bool have_header = false;
  int time_col = -1, value_col = -1;
  std::map<std::uint32_t, i64> day_of_col;
  std::vector<std::string> header_text;
  std::uint32_t cur_row = UINT32_MAX;
  std::vector<std::pair<std::uint32_t, sheets::Cell>> row_cells;
  std::vector<double> ts;
  std::vector<float> vs;

  auto take_header = [&](const auto& cells) {
    int tc = -1, vc = -1, days = 0;
    std::map<std::uint32_t, i64> dc;
    for (auto& [col, c] : cells) {
      if (!c.is_num() && tc < 0 && (contains(c.str, "time") || contains(c.str, "date"))) {
        tc = int(col);
        continue;
      }
      if (auto d = header_day(c)) {
        dc[col] = *d;
        ++days;
      } else if (!c.is_num() && vc < 0 && (contains(c.str, "freq") || contains(c.str, "hz") || contains(c.str, "value"))) {
        vc = int(col);
      }
    }
    if (tc < 0 && days < 2) return false;
    if (tc < 0) tc = 0;
    dc.erase(std::uint32_t(tc));
    time_col = tc;
    day_of_col = std::move(dc);
    if (day_of_col.empty()) {
      if (vc < 0)
        for (auto& [col, c] : cells)
          if (int(col) != tc) {
            vc = int(col);
            break;
          }
      if (vc < 0) return false;
      value_col = vc;
    }
    for (auto& [col, c] : cells) header_text.push_back(c.is_num() ? fmt_num(c.num, "%.6g") : c.str);
    out.layout = day_of_col.empty() ? "date-time column" : "day columns";
    return true;
  };

  auto add = [&](double local, const sheets::Cell& c) {
    double v = c.is_num() ? c.num : to_double(c.str).value_or(NAN);
    if (!(v > 40 && v < 70)) {
      ++out.rejected;
      return;
    }
    ts.push_back(local - utc_offset_s + shift_s);
    vs.push_back(float(v - 50.0));
  };

  auto take_row = [&](const auto& cells) {
    if (cells.empty()) return;
    if (!have_header) {
      have_header = take_header(cells);
      return;
    }
    const sheets::Cell* tcell = nullptr;
    for (auto& [col, c] : cells)
      if (int(col) == time_col) tcell = &c;
    if (!tcell) return;
    TimeCell t = time_cell(*tcell);
    if (!day_of_col.empty()) {
      if (!t.tod) return;
      if (t.dom) out.layout = "month columns";
      for (auto& [col, c] : cells) {
        auto it = day_of_col.find(col);
        if (it == day_of_col.end()) continue;
        i64 day = it->second;
        if (t.dom) {  // the column is a month: its row labels say which day
          day += i64(*t.dom - 1) * 86400;
          if (month_key(day) != month_key(it->second)) {  // "31" in a 30-day month
            ++out.rejected;
            continue;
          }
        }
        add(double(day) + *t.tod, c);
      }
    } else {
      if (!t.abs) return;
      for (auto& [col, c] : cells)
        if (int(col) == value_col) add(*t.abs, c);
    }
  };

  out.sheet = sheets::read(file, {"Frequency data", "FrequencyData", "Frequency"}, [&](std::uint32_t row, std::uint32_t col, const sheets::Cell& c) {
    if (row != cur_row) {
      take_row(row_cells);
      row_cells.clear();
      cur_row = row;
    }
    row_cells.emplace_back(col, c);
  });
  take_row(row_cells);

  if (!have_header) throw std::runtime_error("no header row found (a 'Time' column and day columns, or a date-time and a value column)");
  if (ts.empty()) {
    std::string h;
    for (std::size_t i = 0; i < std::min<std::size_t>(header_text.size(), 6); ++i) h += (i ? " | " : "") + header_text[i];
    throw std::runtime_error("no frequency values read (layout '" + out.layout + "', header: " + h + ")");
  }

  const auto [lo, hi] = std::minmax_element(ts.begin(), ts.end());
  out.first = i64(std::floor(*lo));
  out.last = i64(std::floor(*hi));
  out.samples = ts.size();
  for (auto& m : months_between(month_key(out.first), month_key(out.last))) {
    auto [a, b] = month_bounds(m);
    store::RegularAccumulator acc(a - MARGIN_S, b + MARGIN_S);  // interpolation reaches across the month's ends
    for (std::size_t i = 0; i < ts.size(); ++i)
      if (ts[i] >= double(a - MARGIN_S) && ts[i] < double(b + MARGIN_S)) acc.add(ts[i], vs[i]);
    auto all = acc.finish();
    std::vector<float> own(all.begin() + MARGIN_S, all.begin() + MARGIN_S + (b - a));
    out.months[m] = std::move(own);
  }
  return out;
}

}  // namespace geo::refs
