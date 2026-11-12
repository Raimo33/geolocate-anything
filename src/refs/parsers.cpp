#include "refs/parsers.hpp"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstring>
#include <regex>
#include <stdexcept>

#include "dsp/filters.hpp"
#include "util/json.hpp"
#include "util/text.hpp"

namespace geo::refs {

MonthSeries split_spill(const std::vector<float>& vals, i64 a, const std::string& month) {
  auto [t0, t1] = month_bounds(month);
  MonthSeries out;
  out.own.assign(vals.begin() + (t0 - a), vals.begin() + (t1 - a));
  auto piece = [&](i64 s0, std::size_t lo, std::size_t hi) {
    std::size_t f = lo;
    while (f < hi && !std::isfinite(vals[f])) ++f;
    if (f == hi) return;
    std::size_t l = hi - 1;
    while (!std::isfinite(vals[l])) --l;
    out.spill.push_back({s0 + i64(f - lo), std::vector<float>(vals.begin() + f, vals.begin() + l + 1)});
  };
  piece(a, 0, std::size_t(t0 - a));
  piece(t1, std::size_t(t1 - a), vals.size());
  return out;
}

std::vector<float> shift(const std::vector<float>& x, int lag) {
  std::vector<float> out(x.size(), NAN);
  const std::ptrdiff_t n = std::ptrdiff_t(x.size());
  for (std::ptrdiff_t t = 0; t < n; ++t) {
    std::ptrdiff_t s = t - lag;
    if (s >= 0 && s < n) out[std::size_t(t)] = x[std::size_t(s)];
  }
  return out;
}

std::pair<double, double> detect_units(const std::vector<double>& sample) {
  double lo = INFINITY, hi = -INFINITY;
  std::vector<double> fin;
  for (double v : sample)
    if (std::isfinite(v)) {
      lo = std::min(lo, v);
      hi = std::max(hi, v);
      fin.push_back(v);
    }
  if (lo > 40 && hi < 70) return {dsp::median(fin) > 55 ? 60.0 : 50.0, 1.0};
  if (hi - lo > 2) return {0.0, 1e-3};
  return {0.0, 1.0};
}

static int find_col(const std::vector<std::string_view>& cols, std::initializer_list<const char*> names) {
  for (std::size_t i = 0; i < cols.size(); ++i) {
    std::string c = lower(trim(cols[i]));
    for (const char* n : names)
      if (c == n) return int(i);
  }
  return -1;
}

static double num(std::string_view s) {
  auto v = to_double(s);
  return v ? *v : NAN;
}

// ---------------------------------------------------------------- NESO

// Found against the London PMU (Zenodo 21461024, UTC) and at the files' seams: the
// 2025-03 file's last hour is the 2025-04 file's first (r = 1.000), NESO lacks the hour
// 2025-10-31 23:00 UTC that the PMU holds, and the PMU matches NESO 3600 s later from
// 2025-04-28 to 2025-10-31 but at lag 0 in November and December. No other year's
// March/April seam repeats an hour (2020-2024, 2026 checked).
bool neso_bst_stamped(const std::string& month) { return "2025-04" <= month && month <= "2025-10"; }

NesoParser::NesoParser(const std::string& month) : month_(month), plain_offset_s_(neso_bst_stamped(month) ? 3600 : 0) {
  auto [a, b] = month_bounds(month);
  t0_ = a;
  t1_ = b;
}

void NesoParser::feed(std::string_view chunk) {
  ls_.feed(chunk, [&](std::string_view l) { line(l); });
}

void NesoParser::line(std::string_view l) {
  if (trim(l).empty()) return;
  auto f = split(l, ',');
  if (tcol_ < 0) {
    tcol_ = find_col(f, {"dtm", "date", "datetime", "time"});
    fcol_ = find_col(f, {"f", "freq", "frequency"});
    if (tcol_ < 0 || fcol_ < 0) throw std::runtime_error("NESO file layout changed: no time/frequency column");
    return;
  }
  if (int(f.size()) <= std::max(tcol_, fcol_)) return;
  std::string_view t = trim(f[std::size_t(tcol_)]);
  double dev = num(f[std::size_t(fcol_)]) - 50.0;
  if (!(std::fabs(dev) < 3.0)) dev = NAN;  // junk (a 0 or a blank) is no data, not -3.3 Hz
  if (style_ == 0) {
    style_ = t.size() > 2 && t[2] == '/' ? 2 : 1;
    if (style_ == 1) {
      acc_.emplace(t0_ - SPILL_S, t1_ + SPILL_S);
      zone = plain_offset_s_ ? "plain times, UTC+1 (BST) throughout the file, converted to UTC"
                             : "ISO times: an explicit UTC offset, or plain times in UTC";
    } else {
      acc_.emplace(t0_ - SPILL_S, t1_ + SPILL_S);
      tz_.emplace("Europe/London");
      zone = "Europe/London wall-clock time (dd/mm/yyyy files), converted to UTC";
    }
  }
  if (style_ == 1) {
    if (auto w = parse_iso_datetime(t)) acc_->add(w->has_offset ? w->utc() : w->utc() - double(plain_offset_s_), dev);
  } else {
    auto w = parse_dmy_datetime(t);
    acc_->add((*tz_)(w ? w->naive() : NAN), dev);
  }
}

MonthSeries NesoParser::finish() {
  ls_.finish([&](std::string_view l) { line(l); });
  if (!acc_) throw std::runtime_error("NESO file holds no rows");
  return split_spill(acc_->finish(), t0_ - SPILL_S, month_);
}

// ---------------------------------------------------------------- PGF

PgfParser::PgfParser(const std::string& month, std::optional<std::string> local_tz, int shift_s)
    : month_(month), shift_(shift_s), a_(month_bounds(month).first - (local_tz ? SPILL_S : 0)),
      acc_(a_, month_bounds(month).second + (local_tz ? SPILL_S : 0)) {
  auto m = Month::parse(month).value();
  year_ = m.y;
  mon_ = m.m;
  if (local_tz) tz_.emplace(*local_tz);
}

void PgfParser::feed(std::string_view chunk) {
  ls_.feed(chunk, [&](std::string_view l) { line(l); });
}

void PgfParser::line(std::string_view l) {
  if (phase_ == Phase::Header) {
    phase_ = Phase::Sniffing;
    return;
  }
  auto c = l.find(',');
  if (c == std::string_view::npos) return;
  std::string_view ts = l.substr(0, c), rest = l.substr(c + 1);
  auto c2 = rest.find(',');
  if (c2 != std::string_view::npos) rest = rest.substr(0, c2);
  auto w = parse_iso_datetime(trim(ts));
  if (!w) return;
  pending_.emplace_back(*w, num(rest));
  if (pending_.size() >= (phase_ == Phase::Sniffing ? 2'000'000 : 65536)) flush();  // the first chunk decides the units
}

void PgfParser::flush() {
  if (phase_ != Phase::Streaming) {
    std::vector<double> v;
    v.reserve(pending_.size());
    for (auto& p : pending_) v.push_back(p.second);
    units_ = detect_units(v);
    phase_ = Phase::Streaming;
  }
  auto [off, scale] = units_;
  for (auto& [w, v] : pending_) {
    // Repair a DD/MM <-> MM/DD mix-up made upstream (France 2019): in a month-M file, a
    // row in another month of the same year whose day equals M is that error.
    i64 day = floor_div(w.secs, 86400);
    auto cv = civil_from_days(day);
    if (cv.y == year_ && int(cv.m) != mon_ && int(cv.d) == mon_) {
      i64 tod = w.secs - day * 86400;
      w.secs = days_from_civil(year_, unsigned(mon_), cv.m) * 86400 + tod;
      ++repaired_;
    }
    // A stamp with its own UTC offset is UTC already; the others are wall-clock time.
    double secs = w.has_offset ? w.utc() : tz_ ? (*tz_)(w.naive()) : w.naive();
    const double dev = (v - off) * scale;
    acc_.add(secs + shift_, std::fabs(dev) < 3.0 ? dev : NAN);
  }
  pending_.clear();
}

MonthSeries PgfParser::finish() {
  ls_.finish([&](std::string_view l) { line(l); });
  flush();
  auto vals = acc_.finish();
  if (tz_) return split_spill(vals, a_, month_);
  return {std::move(vals), {}};
}

// ---------------------------------------------------------------- Fingrid

void parse_fingrid_day(std::string_view csv, store::RegularAccumulator& acc) {
  std::vector<std::pair<double, double>> rows;
  bool header = true;
  LineSplitter ls;
  auto on = [&](std::string_view l) {
    if (header) {
      header = false;
      return;
    }
    auto c = l.find(',');
    if (c == std::string_view::npos) return;
    auto w = parse_iso_datetime(trim(l.substr(0, c)));
    std::string_view rest = l.substr(c + 1);
    if (auto c2 = rest.find(','); c2 != std::string_view::npos) rest = rest.substr(0, c2);
    rows.emplace_back(w ? w->naive() : NAN, num(rest));
  };
  ls.feed(csv, on);
  ls.finish(on);
  if (rows.empty()) return;
  std::vector<double> v;
  for (auto& r : rows)
    if (std::isfinite(r.second)) v.push_back(r.second);
  const double off = !v.empty() && dsp::median(v) > 40 ? 50.0 : 0.0;
  // Each daily file is in order, so the repeated autumn hour is found per file.
  LocalToUtc tz("Europe/Helsinki");
  for (auto& [t, val] : rows) {
    const double secs = tz(t), dev = val - off;  // the converter sees every row, in order
    if (std::fabs(dev) < 3.0) acc.add(secs, dev);
  }
}

std::map<std::string, std::string> parse_fingrid_index(std::string_view html) {
  static const std::string open = "<script id=\"__NEXT_DATA__\" type=\"application/json\">";
  auto a = html.find(open);
  if (a == std::string_view::npos) throw std::runtime_error("Fingrid dataset page layout changed: no __NEXT_DATA__");
  a += open.size();
  auto b = html.find("</script>", a);
  auto data = parse_json(html.substr(a, b - a));
  std::map<std::string, std::string> found;
  static const std::regex re(R"((\d{4})-(\d{2})\.(?:zip|7z)$)");
  std::function<void(const json::value&)> walk = [&](const json::value& o) {
    if (o.is_object()) {
      if (auto* l = o.get_object().if_contains("link"); l && l->is_string()) {
        std::string link(l->get_string());
        std::smatch m;
        if (link.starts_with("/files/339/") && std::regex_search(link, m, re)) found[m[1].str() + "-" + m[2].str()] = link;
      }
      for (auto& kv : o.get_object()) walk(kv.value());
    } else if (o.is_array()) {
      for (auto& v : o.get_array()) walk(v);
    }
  };
  walk(data);
  return found;
}

// ---------------------------------------------------------------- EDR

EdrParser::EdrParser(const std::vector<std::string>& months, std::optional<Implied> implied, std::vector<ClockKnot> clock)
    : months_(months), implied_(implied), clock_(std::move(clock)) {
  a_ = month_bounds(months.front()).first;
  b_ = month_bounds(months.back()).second;
  out_.assign(std::size_t(b_ - a_), NAN);
}

void EdrParser::feed(std::string_view chunk) {
  ls_.feed(chunk, [&](std::string_view l) { line(l); });
}

void EdrParser::line(std::string_view l) {
  if (l.empty()) return;
  if (header_) {
    header_ = false;
    sep_ = l.find(';') != std::string_view::npos ? ';' : ',';
    auto f = split(l, sep_);
    for (std::size_t i = 0; i < f.size(); ++i) {
      std::string c = lower(trim(f[i]));
      if (fcol_ < 0 && (c.starts_with("f50") || c.starts_with("f60") || c == "freq")) fcol_ = int(i);
      if (qcol_ < 0 && c.starts_with("qi")) qcol_ = int(i);
    }
    if (fcol_ < 0) throw std::runtime_error("EDR file layout changed: no f50/f60/freq column");
    return;
  }
  auto f = split(l, sep_);
  ++rows;
  if (int(f.size()) <= fcol_) return;
  double secs, v;
  if (implied_) {
    secs = double(implied_->t0) + double(rows - 1);
    v = num(f[std::size_t(fcol_)]) - implied_->nominal_hz;
  } else {
    auto w = parse_iso_datetime(trim(f[0]));
    if (!w) return;
    secs = w->utc();
    v = num(f[std::size_t(fcol_)]) * 1e-3;
  }
  if (!clock_.empty()) {
    if (secs < double(clock_.front().stamp)) return;
    secs = std::round(secs - offset_at(secs));
  }
  if (!(std::fabs(v) < 3.0 && secs >= double(a_) && secs < double(b_))) return;
  if (qcol_ >= 0 && (int(f.size()) <= qcol_ || num(f[std::size_t(qcol_)]) != 0)) return;
  out_[std::size_t(std::floor(secs - double(a_)))] = static_cast<float>(v);
  ++kept;
}

double EdrParser::offset_at(double stamp) const {
  if (stamp <= double(clock_.front().stamp)) return clock_.front().offset_s;
  for (std::size_t i = 1; i < clock_.size(); ++i)
    if (stamp <= double(clock_[i].stamp)) {
      const auto &a = clock_[i - 1], &b = clock_[i];
      return a.offset_s + (b.offset_s - a.offset_s) * (stamp - double(a.stamp)) / double(b.stamp - a.stamp);
    }
  return clock_.back().offset_s;
}

std::map<std::string, std::vector<float>> EdrParser::finish() {
  ls_.finish([&](std::string_view l) { line(l); });
  // A clock running slow or fast moves rows onto the same second or past one: those
  // single-second holes between two values are filled.
  if (!clock_.empty())
    for (std::size_t i = 1; i + 1 < out_.size(); ++i)
      if (std::isnan(out_[i]) && !std::isnan(out_[i - 1]) && !std::isnan(out_[i + 1])) out_[i] = 0.5f * (out_[i - 1] + out_[i + 1]);
  std::map<std::string, std::vector<float>> res;
  for (auto& m : months_) {
    auto [m0, m1] = month_bounds(m);
    res[m].assign(out_.begin() + (m0 - a_), out_.begin() + (m1 - a_));
  }
  out_.clear();
  out_.shrink_to_fit();
  return res;
}

// ---------------------------------------------------------------- plain time/frequency CSVs

static std::string_view unquote(std::string_view s) {
  s = trim(s);
  if (s.size() >= 2 && s.front() == '"' && s.back() == '"') s = s.substr(1, s.size() - 2);
  return s;
}

SeriesParser::SeriesParser(const std::vector<std::string>& months, SeriesFormat f) : f_(std::move(f)) {
  for (auto& m : months) open_[m];
  if (!f_.tz.empty()) tz_.emplace(f_.tz);
}

void SeriesParser::feed(std::string_view chunk) {
  ls_.feed(chunk, [&](std::string_view l) { line(l); });
}

void SeriesParser::line(std::string_view l) {
  if (trim(l).empty()) return;
  if (!sep_) sep_ = l.find(';') != std::string_view::npos ? ';' : l.find(',') != std::string_view::npos ? ',' : '\t';
  auto f = split(l, sep_);
  std::string_view first = unquote(f[0]);
  if (first.empty() || first[0] < '0' || first[0] > '9') {  // a header (or junk: no column it names)
    int t = -1, v = -1, q = -1;
    for (std::size_t i = 0; i < f.size(); ++i) {
      std::string c = lower(unquote(f[i]));
      auto is = [&](const std::vector<std::string>& names) { return std::find(names.begin(), names.end(), c) != names.end(); };
      if (t < 0 && is(f_.time_cols)) t = int(i);
      if (v < 0 && is(f_.value_cols)) v = int(i);
      if (q < 0 && is(f_.quality_cols)) q = int(i);
    }
    if (t >= 0 && v >= 0) tcol_ = t, fcol_ = v, qcol_ = q;
    return;
  }
  if (int(f.size()) <= std::max({tcol_, fcol_, qcol_})) return;
  ++rows;
  std::string_view ts = unquote(f[std::size_t(tcol_)]);
  double secs = NAN;
  auto w = parse_iso_datetime(ts);
  if (!w) w = parse_dmony_datetime(ts);
  if (w) {
    const double n = w->naive();
    secs = w->has_offset ? w->utc() : tz_ ? (*tz_)(n) : n - f_.utc_offset_s;
  } else if (double u = num(ts); u > 1e8) {
    secs = u > 1e11 ? u * 1e-3 : u;  // UNIX milliseconds or seconds
  }
  if (!std::isfinite(secs)) return;
  secs += f_.shift_s;
  if (!(secs >= f_.keep_from && secs < f_.keep_to)) return;
  double v = num(unquote(f[std::size_t(fcol_)])) * f_.scale - f_.nominal_hz;
  if (!(std::fabs(v) < 3.0)) return;
  if (qcol_ >= 0 && unquote(f[std::size_t(qcol_)]) != "0") return;
  if (secs > last_close_ + 86400) close_before(secs - 2 * 86400);
  auto it = open_.find(month_key(static_cast<i64>(std::floor(secs))));
  if (it == open_.end()) return;
  if (!it->second) {
    auto [a, b] = month_bounds(it->first);
    it->second.emplace(a, b);
  }
  it->second->add(secs, v);
  ++kept;
}

void SeriesParser::close_before(double t) {
  last_close_ = t + 2 * 86400;
  for (auto it = open_.begin(); it != open_.end();) {
    auto [a, b] = month_bounds(it->first);
    if (double(b) > t) break;  // months are ordered
    done_[it->first] = it->second ? it->second->finish() : std::vector<float>(std::size_t(b - a), NAN);
    it = open_.erase(it);
  }
}

std::map<std::string, std::vector<float>> SeriesParser::finish() {
  ls_.finish([&](std::string_view l) { line(l); });
  close_before(INFINITY);
  return std::move(done_);
}

std::size_t drop_flat_runs(std::vector<float>& v, int min_s) {
  std::size_t dropped = 0, i = 0;
  while (i < v.size()) {
    if (!std::isfinite(v[i])) {
      ++i;
      continue;
    }
    std::size_t last = i;  // last second of the run holding v[i] (at the store's 1 mHz step)
    const long q = std::lround(double(v[i]) * 1e3);
    for (std::size_t j = i + 1; j < v.size(); ++j) {
      if (!std::isfinite(v[j])) continue;
      if (std::lround(double(v[j]) * 1e3) != q) break;
      last = j;
    }
    if (last - i + 1 >= std::size_t(min_s))
      for (std::size_t j = i; j <= last; ++j)
        if (std::isfinite(v[j])) v[j] = NAN, ++dropped;
    i = last + 1;
  }
  return dropped;
}

// ---------------------------------------------------------------- LBNL micro-PMU

void UpmuAngleParser::feed(std::string_view chunk) {
  ls_.feed(chunk, [&](std::string_view l) { line(l); });
}

void UpmuAngleParser::line(std::string_view l) {
  const auto comma = l.find(',');
  if (comma == std::string_view::npos) return;
  i64 ns = 0;
  auto ts = trim(l.substr(0, comma));
  if (std::from_chars(ts.data(), ts.data() + ts.size(), ns).ec != std::errc{}) return;
  const double deg = num(trim(l.substr(comma + 1)));
  if (!std::isfinite(deg)) return;
  ++rows;
  const double dt = double(ns - prev_ns_) * 1e-9;
  if (dt > 0 && dt <= 0.05 && std::isfinite(prev_deg_)) {  // consecutive samples (120/s); a gap restarts
    double d = std::fmod(deg - prev_deg_, 360.0);
    if (d > 180) d -= 360;
    if (d <= -180) d += 360;
    const double dev = d / (360.0 * dt);
    if (std::fabs(dev) < 0.5) {  // a phase step from switching is not frequency
      acc_.add(double(prev_ns_ / 1000000000) + double(prev_ns_ % 1000000000) * 1e-9 + dt / 2, dev + 60.0 - nominal_);
      ++kept;
    }
  }
  prev_ns_ = ns;
  prev_deg_ = deg;
}

std::vector<float> UpmuAngleParser::finish() {
  ls_.finish([&](std::string_view l) { line(l); });
  return acc_.finish();
}

// ---------------------------------------------------------------- EirGrid

std::size_t parse_eirgrid_frequency(std::string_view json_text, LocalToUtc& tz, int shift_s, store::RegularAccumulator& acc,
                                    std::size_t* listed) {
  static const char* MON[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
  auto doc = parse_json(json_text);
  if (!doc.is_object() || !doc.as_object().contains("Rows")) throw std::runtime_error("EirGrid frequency response layout changed: no Rows");
  std::size_t kept = 0;
  for (auto& r : doc.as_object().at("Rows").as_array()) {
    if (!r.is_object()) continue;
    auto& o = r.as_object();
    auto* fn = o.if_contains("FieldName");
    auto* et = o.if_contains("EffectiveTime");
    auto* val = o.if_contains("Value");
    if (!fn || !fn->is_string() || fn->as_string() != "SYS_FREQUENCY" || !et || !et->is_string()) continue;
    if (listed) ++*listed;
    if (!val || !val->is_number()) continue;
    std::string_view t = et->as_string();  // "01-Nov-2026 00:00:05"
    if (t.size() < 20 || t[2] != '-' || t[6] != '-') continue;
    int mon = 0;
    while (mon < 12 && t.substr(3, 3) != MON[mon]) ++mon;
    if (mon == 12) continue;
    char iso[32];
    std::snprintf(iso, sizeof iso, "%.4s-%02d-%.2s %.8s", t.data() + 7, mon + 1, t.data(), t.data() + 12);
    auto w = parse_iso_datetime(iso);
    double v = val->to_number<double>() - 50.0;
    if (!w || !(std::fabs(v) < 3.0)) continue;
    acc.add(tz(w->naive()) + shift_s, v);
    ++kept;
  }
  return kept;
}

// ---------------------------------------------------------------- Netztransparenz

NtpParser::NtpParser(const std::string& month)
    : month_(month), a_(month_bounds(month).first - SPILL_S), acc_(a_, month_bounds(month).second + SPILL_S), tz_("Europe/Berlin") {}

void NtpParser::feed(std::string_view chunk) {
  ls_.feed(chunk, [&](std::string_view l) {
    auto f = split(l, ',');
    if (f.size() < 4) return;
    std::string ts(trim(f[0]));
    std::replace(ts.begin(), ts.end(), '/', '-');  // older files write 2015/05/01
    ts += ' ';
    ts += trim(f[1]);
    auto w = parse_iso_datetime(ts);
    double secs = tz_(w && !w->has_offset ? w->naive() : NAN) + NTP_SHIFT_S;
    double v = num(f[3]);
    acc_.add(secs, std::fabs(v - 50.0) < 1.0 ? v - 50.0 : NAN);
  });
}

MonthSeries NtpParser::finish() {
  feed("\n");
  return split_spill(acc_.finish(), a_, month_);
}

// ---------------------------------------------------------------- Irish FDR

std::size_t parse_fdr_day(std::string_view text, store::RegularAccumulator& acc) {
  std::size_t rows = 0;
  LineSplitter ls;
  auto on = [&](std::string_view l) {
    std::string_view f[8];
    std::size_t k = 0, i = 0;
    while (i < l.size() && k < 8) {
      while (i < l.size() && (l[i] == ' ' || l[i] == '\t')) ++i;
      std::size_t a = i;
      while (i < l.size() && l[i] != ' ' && l[i] != '\t') ++i;
      if (i > a) f[k++] = l.substr(a, i - a);
    }
    if (k < 6) return;
    ++rows;
    std::string d(f[1]), t(f[2]);
    if (d.size() < 6) d.insert(0, 6 - d.size(), '0');
    if (t.size() < 6) t.insert(0, 6 - t.size(), '0');
    auto two = [](const std::string& s, std::size_t p) {
      return (s[p] >= '0' && s[p] <= '9' && s[p + 1] >= '0' && s[p + 1] <= '9') ? (s[p] - '0') * 10 + (s[p + 1] - '0') : -1;
    };
    int mo = two(d, 0), dd = two(d, 2), yy = two(d, 4), hh = two(t, 0), mi = two(t, 2), ss = two(t, 4);
    if (d.size() != 6 || t.size() != 6 || mo < 1 || mo > 12 || dd < 1 || dd > 31 || yy < 0 || hh < 0 || hh > 23 || mi < 0 || mi > 59 ||
        ss < 0 || ss > 59)
      return;
    int year = yy < 69 ? 2000 + yy : 1900 + yy;
    double secs = double(days_from_civil(year, unsigned(mo), unsigned(dd)) * 86400 + hh * 3600 + mi * 60 + ss);
    secs += (num(f[3]) - 1) / 10;
    double fr = num(f[5]);
    acc.add(secs, std::fabs(fr - 50.0) < 1.0 ? fr - 50.0 : NAN);
  };
  ls.feed(text, on);
  ls.finish(on);
  return rows;
}

// ---------------------------------------------------------------- AEMO

static double nem_secs(std::string_view s) {
  auto w = parse_iso_datetime(trim(s));
  return w ? w->naive() - double(NEM_UTC_OFFSET_S) : NAN;
}

void FppParser::feed(std::string_view chunk) {
  ls_.feed(chunk, [&](std::string_view l) { line(l); });
}

void FppParser::finish() {
  ls_.finish([&](std::string_view l) { line(l); });
}

void FppParser::line(std::string_view l) {
  if (l.starts_with("I,FPP,REGION_FREQ_MEASURE")) {
    cols_.clear();
    for (auto c : split(l, ',')) cols_.emplace_back(trim(c));
    auto idx = [&](const char* n) {
      auto it = std::find(cols_.begin(), cols_.end(), n);
      return it == cols_.end() ? -1 : int(it - cols_.begin());
    };
    i_flag_ = idx("HZ_QUALITY_FLAG");
    i_region_ = idx("REGIONID");
    i_time_ = idx("MEASUREMENT_DATETIME");
    i_dev_ = idx("FREQ_DEVIATION_HZ");
    return;
  }
  if (cols_.empty() || !l.starts_with("D,FPP,REGION_FREQ_MEASURE")) return;
  auto f = split(l, ',');
  auto get = [&](int i) { return i >= 0 && std::size_t(i) < f.size() ? trim(f[std::size_t(i)]) : std::string_view(); };
  if (i_flag_ >= 0 && get(i_flag_) != "1") return;
  auto it = accs_.find(get(i_region_));
  if (it == accs_.end()) return;
  const double dev = num(get(i_dev_));
  if (!(std::fabs(dev) < 3.0)) return;
  it->second->add(nem_secs(get(i_time_)), dev);
  ++rows;
}

std::size_t parse_causer_pays_csv(std::string_view data, std::map<std::string, store::RegularAccumulator*>& accs) {
  static const std::pair<const char*, const char*> ELEMENTS[] = {{"32001", "australia_nem"}, {"42001", "australia_tas"}};
  std::size_t found = 0;
  for (auto [el, grid] : ELEMENTS) {
    auto acc_it = accs.find(grid);
    std::size_t i = data.find(el);
    while (i != std::string_view::npos) {
      std::size_t s = data.rfind('\n', i);
      s = s == std::string_view::npos ? 0 : s + 1;
      std::size_t e = data.find('\n', i);
      std::string_view line = data.substr(s, (e == std::string_view::npos ? data.size() : e) - s);
      // "2016-01-01 00:00:03, 32001 ,13,49.94000,-1"
      auto f = split(line, ',');
      if (f.size() >= 4 && f[0].size() == 19 && trim(f[1]) == el && trim(f[2]) == "13") {
        auto w = parse_iso_datetime(f[0]);
        auto vv = to_double(f[3]);
        if (w && vv) {
          ++found;
          double d = *vv - 50.0;
          if (acc_it != accs.end()) acc_it->second->add(w->naive() - double(NEM_UTC_OFFSET_S), std::fabs(d) < 1.0 ? d : NAN);
        }
      }
      if (e == std::string_view::npos) break;
      i = data.find(el, e);
    }
  }
  return found;
}

// ---------------------------------------------------------------- Energy-Charts

std::size_t parse_energy_charts(std::string_view json_text, store::RegularAccumulator& acc) {
  auto d = parse_json(json_text);
  auto* t = jget(d, "unix_seconds");
  auto* v = jget(d, "data");
  if (!t || !v || !t->is_array() || !v->is_array()) return 0;
  const auto& ta = t->get_array();
  const auto& va = v->get_array();
  if (ta.size() != va.size()) return 0;
  for (std::size_t i = 0; i < ta.size(); ++i)
    if (double d = as_number(va[i], NAN) - 50.0; std::fabs(d) < 3.0) acc.add(as_number(ta[i]), d);
  return ta.size();
}

// ---------------------------------------------------------------- index pages

std::map<std::string, std::string> parse_neso_index(std::string_view package_json) {
  auto d = parse_json(package_json);
  std::map<std::string, std::string> idx;
  static const std::regex ext(R"(\.(csv|zip)$)", std::regex::icase);
  static const std::regex ym(R"((\d{4})[-_ ](\d{1,2})\.(?:csv|zip)$)", std::regex::icase);
  static const std::regex title(R"(([A-Za-z]+)\s+(\d{4}))");
  static const char* MONTHS[] = {"january", "february", "march", "april", "may", "june", "july", "august", "september", "october", "november", "december"};
  for (auto& r : d.at("result").at("resources").get_array()) {
    std::string url = jstr(r, "url");
    if (!std::regex_search(url, ext)) continue;
    std::smatch m;
    char key[16];
    if (std::regex_search(url, m, ym)) {
      std::snprintf(key, sizeof key, "%04d-%02d", std::stoi(m[1]), std::stoi(m[2]));
    } else {
      std::string name = jstr(r, "name");
      if (!std::regex_search(name, m, title)) continue;
      std::string mon = lower(m[1].str());
      int mi = 0;
      for (int i = 0; i < 12; ++i)
        if (mon == MONTHS[i]) mi = i + 1;
      if (!mi) continue;
      std::snprintf(key, sizeof key, "%04d-%02d", std::stoi(m[2]), mi);
    }
    idx[key] = url;
  }
  return idx;
}

std::vector<std::string> parse_html_links(std::string_view html, const std::string& base, const std::string& pattern) {
  static const std::regex href(R"re(HREF="([^"]+)")re", std::regex::icase);
  std::regex pat(pattern, std::regex::icase);
  std::vector<std::string> out;
  std::string s(html);
  for (auto it = std::sregex_iterator(s.begin(), s.end(), href); it != std::sregex_iterator(); ++it) {
    std::string h = (*it)[1].str();
    if (std::regex_search(h, pat)) out.push_back(base + h);
  }
  return out;
}

}  // namespace geo::refs
