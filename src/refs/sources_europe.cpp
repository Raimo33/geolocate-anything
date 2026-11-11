// European reference sources: NESO, the Power Grid Frequency DB, Fingrid, Energy-Charts,
// Netztransparenz, Open-ENF, the Irish FDR and EirGrid.
#include "refs/source_impl.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <regex>
#include <stdexcept>

#include "net/zip.hpp"
#include "store/store.hpp"
#include "util/text.hpp"

namespace geo::refs::detail {

// ---------------------------------------------------------------- NESO (GB)

namespace {

constexpr const char* NESO_PACKAGE = "https://api.neso.energy/api/3/action/package_show?id=system-frequency-data";

asio::awaitable<const std::map<std::string, std::string>*> neso_urls(Ctx& c) {
  if (!c.neso) c.neso = co_await cached_index(c, "neso_index.json", NESO_PACKAGE, &parse_neso_index);
  co_return &*c.neso;
}

// Months whose seams with the month before were checked by hand (see neso_bst_stamped).
constexpr const char* NESO_SEAMS_CHECKED_TO = "2026-10";

// Once in UTC, a file's first hour must not repeat the last hour stored for the month
// before. The 2025-04 file did: its plain times were BST, an hour off. A repeat in a
// later file means NESO stamped in local time again, so the file is refused (the
// refresh reports it and asks again each time) instead of being stored an hour off:
// neso_bst_stamped has to learn the new months. Past the months checked by hand the
// month before must be stored first (NESO fetches months in order, one at a time).
void neso_check_seam(const std::string& grid, const std::string& month, const std::vector<float>& own) {
  const std::string prev = Month::parse(month).value().prev().key();
  const i64 t0 = month_bounds(month).first;
  if (!store::month_info(grid, "neso", prev)) {
    if (month > NESO_SEAMS_CHECKED_TO)
      throw std::runtime_error("NESO " + month + ": " + prev + " is not stored, so its seam with " + month +
                               " cannot be checked for an hour's offset (fetched again next refresh)");
    return;
  }
  const auto before = store::read(grid, t0 - 3600, t0, {"neso"});
  std::size_t n = 0, same = 0;
  for (std::size_t i = 0; i < before.size() && i < own.size(); ++i)
    if (std::isfinite(before[i]) && std::isfinite(own[i])) ++n, same += std::fabs(before[i] - own[i]) < 0.0015f;
  if (n >= 1800 && double(same) >= 0.8 * double(n))
    throw std::runtime_error("NESO " + month + ": the file's first hour repeats " + prev + "'s last hour (" + std::to_string(same) + " of " +
                             std::to_string(n) + " s equal): its times look an hour off (BST); not stored until neso_bst_stamped covers it");
}

}  // namespace

asio::awaitable<MonthData> neso_fetch(Ctx& c, const std::string& grid, const std::string& month) {
  const auto& urls = *co_await neso_urls(c);
  auto it = urls.find(month);
  if (it == urls.end())
    throw Unavailable("NESO has no file for " + month + " (available " + (urls.empty() ? "" : urls.begin()->first + ".." + urls.rbegin()->first) + ")");
  NesoParser p(month);
  co_await stream_csv_or_zip(c, it->second, [&](std::string_view d) { p.feed(d); });
  auto ser = p.finish();
  neso_check_seam(grid, month, ser.own);
  MonthData md;
  md.values = std::move(ser.own);
  md.spill = std::move(ser.spill);
  md.meta = {{"source", "neso"}, {"url", it->second}, {"licence", "NESO Open Data Licence"}, {"native_resolution_s", 1.0},
             {"timestamps", p.zone}};
  co_return md;
}

asio::awaitable<std::vector<std::string>> neso_months(Ctx& c, const std::string&) {
  std::vector<std::string> out;
  for (auto& [k, v] : *co_await neso_urls(c)) out.push_back(k);
  co_return out;
}

// ---------------------------------------------------------------- Power Grid Frequency DB (OSF)

namespace {

constexpr const char* OSF_DATA_FOLDER = "https://api.osf.io/v2/nodes/m43tg/files/osfstorage/5eecddad145b1a010652cf46/";
struct OsfPart {
  const char *top, *country, *prefix;
  double native;
};
// Germany (the TSOs' series re-hosted, German wall-clock time, 2 s late) is the
// primary; France only fills its gaps after being time-aligned to it.
const std::map<std::string, std::vector<OsfPart>> OSF_LAYOUT = {
    {"continental_europe", {{"Continental Europe", "Germany", "germany", 1.0}, {"Continental Europe", "France", "france", 10.0}}}};
constexpr const char* OSF_TZ = "Europe/Berlin";
const std::map<std::string, int> OSF_SHIFT_S = {{"germany", -2}};
const std::map<std::string, int> DEFAULT_LAG_S = {{"france", 15}};

asio::awaitable<std::vector<json::value>> osf_list(Ctx& c, std::string url) {
  std::vector<json::value> items;
  std::string nxt = url + (url.find('?') != std::string::npos ? "&" : "?") + "page[size]=100";
  while (!nxt.empty()) {
    auto d = parse_json(co_await get_text(c, nxt));
    for (auto& it : d.at("data").get_array()) items.push_back(it);
    auto* links = jget(d, "links");
    auto* n = links ? jget(*links, "next") : nullptr;
    nxt = n && n->is_string() ? std::string(n->get_string()) : "";
  }
  co_return items;
}

asio::awaitable<std::string> osf_child(Ctx& c, const std::string& url, const std::string& name) {
  for (auto& it : co_await osf_list(c, url)) {
    const auto& a = it.at("attributes");
    if (jstr(a, "kind") == "folder" && jstr(a, "name") == name)
      co_return std::string(it.at("relationships").at("files").at("links").at("related").at("href").get_string());
  }
  throw Unavailable("OSF folder '" + name + "' not found");
}

asio::awaitable<std::string> osf_month_url(Ctx& c, const OsfPart& p, const std::string& month) {
  fs::path cache = store::cache_dir() / "osf_index.json";
  json::object idx;
  if (auto t = read_file(cache)) idx = parse_json(*t).as_object();
  std::string key = std::string(p.prefix) + ":" + month;
  if (auto* v = idx.if_contains(key)) co_return std::string(v->get_string());
  std::string y = month.substr(0, 4), m = month.substr(5, 2);
  std::string url = co_await osf_child(c, OSF_DATA_FOLDER, p.top);
  url = co_await osf_child(c, url, p.country);
  url = co_await osf_child(c, url, y);
  url = co_await osf_child(c, url, m);
  for (auto& it : co_await osf_list(c, url)) {
    std::string name = jstr(it.at("attributes"), "name");
    if (name.ends_with(".zip")) {
      // Re-read: another month may have added entries meanwhile.
      if (auto t = read_file(cache)) idx = parse_json(*t).as_object();
      idx[key] = jstr(it.at("links"), "download");
      atomic_write(cache, dump(idx));
      co_return std::string(idx[key].get_string());
    }
  }
  throw Unavailable("no data file in OSF folder " + std::string(p.top) + "/" + p.country + "/" + y + "/" + m);
}

}  // namespace

const std::map<std::string, Location> OSF_LOCATIONS = {
    {"Germany", {"Germany (TSO series; Frankfurt as a point inside)", 50.11, 8.68}},
    {"France", {"France (site not published; Paris assumed)", 48.86, 2.35}}};

asio::awaitable<MonthData> osf_fetch(Ctx& c, const std::string& grid, const std::string& month) {
  auto lay = OSF_LAYOUT.find(grid);
  if (lay == OSF_LAYOUT.end()) throw Unavailable("OSF Power Grid Frequency DB has no data for grid '" + grid + "'");
  std::vector<std::string> errors;
  std::optional<std::vector<float>> merged;
  std::vector<json::object> used;
  std::vector<Spill> spill;
  for (auto& part : lay->second) {
    std::string url;
    try {
      url = co_await osf_month_url(c, part, month);
    } catch (const Unavailable& e) {
      errors.push_back(e.what());
      continue;
    }
    log(std::string("  downloading ") + part.country + " " + month + " from OSF ...");
    auto sh = OSF_SHIFT_S.count(part.prefix) ? OSF_SHIFT_S.at(part.prefix) : 0;
    PgfParser p(month, std::string(OSF_TZ), sh);
    co_await stream_csv_or_zip(c, url, [&](std::string_view d) { p.feed(d); });
    auto ser = p.finish();
    if (p.repaired()) log("  repaired " + std::to_string(p.repaired()) + " rows with swapped day/month in " + part.country + " " + month);
    json::object info{{"country", part.country}, {"url", url}, {"native_resolution_s", part.native},
                      {"rows", p.rows()}, {"repaired_day_month_swaps", p.repaired()}};
    if (!merged) {
      merged = std::move(ser.own);
      if (int lag = DEFAULT_LAG_S.count(part.prefix) ? DEFAULT_LAG_S.at(part.prefix) : 0) {
        // No Germany this month: France stands alone, still moved onto Germany's timing.
        // Its spill is dropped (the neighbours' own files hold those hours, and the
        // shift would run them across the month's edge).
        *merged = shift(*merged, lag);
        info["aligned_lag_s"] = lag;
        info["lag_from"] = "default";
      } else {
        spill = std::move(ser.spill);  // gap-filling stays within the month
      }
    } else {
      auto vals = std::move(ser.own);
      auto est = estimate_lag(*merged, vals);
      int lag;
      if (est && est->second > 0.9) {
        lag = est->first;
        info["aligned_lag_s"] = lag;
        info["aligned_r"] = std::nearbyint(est->second * 1e4) / 1e4;
        info["lag_from"] = "measured";
      } else {
        lag = DEFAULT_LAG_S.count(part.prefix) ? DEFAULT_LAG_S.at(part.prefix) : 0;
        info["aligned_lag_s"] = lag;
        info["lag_from"] = "default";
      }
      vals = shift(vals, lag);
      std::int64_t filled = 0;
      for (std::size_t i = 0; i < vals.size(); ++i)
        if (std::isnan((*merged)[i]) && std::isfinite(vals[i])) {
          (*merged)[i] = vals[i];
          ++filled;
        }
      info["filled_s"] = filled;
    }
    used.push_back(std::move(info));
  }
  if (!merged) throw Unavailable(join(errors, "; "));
  // Germany 2012-01 is a file of timestamps without values: nothing to store.
  if (!any_finite(*merged)) throw Unavailable("the OSF files hold no values for " + month);
  MonthData md;
  md.values = std::move(*merged);
  md.spill = std::move(spill);
  md.meta = {{"source", "osf_pgf"},
             {"project", "https://osf.io/m43tg/"},
             {"licence", "see https://power-grid-frequency.org (CC BY 4.0 per project; original TSO terms may apply)"},
             {"files", to_json_array(used)},
             {"timestamps", "Europe/Berlin wall-clock time (found by cross-checking), converted to UTC; Germany shifted -2 s"}};
  co_return md;
}

asio::awaitable<std::vector<std::string>> osf_months(Ctx&, const std::string&) { co_return months_between("2011-07", "2020-09"); }

// ---------------------------------------------------------------- Fingrid (Nordic)

namespace {

constexpr const char* FINGRID_PAGE = "https://data.fingrid.fi/en/datasets/339";
constexpr const char* FINGRID_BASE = "https://data.fingrid.fi";

std::map<std::string, std::string> parse_fingrid_index_sv(std::string_view h) { return parse_fingrid_index(h); }

asio::awaitable<const std::map<std::string, std::string>*> fingrid_urls(Ctx& c) {
  if (!c.fingrid) c.fingrid = co_await cached_index(c, "fingrid_index.json", FINGRID_PAGE, &parse_fingrid_index_sv);
  co_return &*c.fingrid;
}

}  // namespace

asio::awaitable<MonthData> fingrid_fetch(Ctx& c, const std::string& grid, const std::string& month) {
  const auto& urls = *co_await fingrid_urls(c);
  auto it = urls.find(month);
  if (it == urls.end())
    throw Unavailable("Fingrid has no archive for " + month + " (available " + (urls.empty() ? "" : urls.begin()->first + ".." + urls.rbegin()->first) + ")");
  const std::string url = FINGRID_BASE + it->second;
  log("  downloading Fingrid " + month + " ...");
  TempFile tmp(".arc", tmp_dir());
  co_await download_to(c, url, tmp.path);
  auto fm = parse_fingrid_archive(tmp.path.string(), month);
  MonthData md;
  md.values = std::move(fm.own);
  for (auto& [s0, v] : fm.spill) md.spill.push_back({s0, std::move(v)});
  md.meta = {{"source", "fingrid"},
             {"url", url},
             {"licence", "CC BY 4.0 (Fingrid open data)"},
             {"native_resolution_s", 0.1},
             {"timestamps", "Europe/Helsinki wall-clock time (documented; autumn-change files hold 25 h), converted to UTC"},
             {"daily_files", fm.daily_files},
             {"rows", fm.rows}};
  co_return md;
}

asio::awaitable<std::vector<std::string>> fingrid_months(Ctx& c, const std::string&) {
  std::vector<std::string> out;
  for (auto& [k, v] : *co_await fingrid_urls(c)) out.push_back(k);
  co_return out;
}

// ---------------------------------------------------------------- Energy-Charts (CE, current)

namespace {

constexpr const char* EC_URL = "https://api.energy-charts.info/frequency";
constexpr const char* EC_FIRST = "2022-05";
constexpr i64 EC_SPAN_S = 3 * 86400;  // API limit per request

// The public API allows about 2 requests per minute (burst 4) and answers 429 with
// Retry-After: requests are serialised and wait as told.
asio::awaitable<std::string> energy_charts_get(Ctx& c, i64 start, i64 end, double max_wait_s = 900) {
  const std::string url = std::string(EC_URL) + "?region=DE-Freiburg&start=" + std::to_string(start) + "&end=" + std::to_string(end);
  co_await c.energy_charts.acquire();
  struct Release {
    net::AsyncSemaphore& s;
    ~Release() { s.release(); }
  } rel{c.energy_charts};
  double waited = 0;
  for (int attempt = 0;; ++attempt) {
    net::Request rq;
    rq.url = url;
    rq.total_timeout_s = 180;
    auto r = co_await c.http.fetch(std::move(rq));
    if (r.ok()) co_return std::move(r.body);
    if (r.error.empty() && r.status == 404) co_return std::string(R"({"unix_seconds": [], "data": []})");
    const bool retry = !r.error.empty() || r.status == 429 || r.status >= 500;
    if (!retry || waited > max_wait_s)
      throw std::runtime_error("Energy-Charts: " + (r.error.empty() ? "HTTP " + std::to_string(r.status) : r.error) + " for " + url);
    double delay = std::min(60.0, 5.0 * double(1 << std::min(attempt, 10)));
    std::string ra = r.header("retry-after");
    if (!ra.empty() && std::all_of(ra.begin(), ra.end(), ::isdigit)) delay = std::stod(ra);
    waited += delay;
    co_await net::sleep_for(delay);
  }
}

}  // namespace

asio::awaitable<MonthData> energy_charts_fetch(Ctx& c, const std::string& grid, const std::string& month) {
  if (month < EC_FIRST) throw Unavailable(std::string("Energy-Charts frequency starts ") + EC_FIRST);
  auto [t0, t1] = month_bounds(month);
  const double now = now_seconds();
  if (double(t0) > now) throw Unavailable(month + " is in the future");
  std::vector<std::pair<i64, i64>> reqs;
  for (auto [s, e] : energy_charts_requests(month, now))
    if (!already_have(c, grid, month, s, e + 1)) reqs.emplace_back(s, e);
  log("  downloading Energy-Charts " + month + " (" + std::to_string(reqs.size()) + " request(s); the API allows ~2/min) ...");
  store::RegularAccumulator acc(t0, t1);
  int n_req = 0;
  for (auto [s, e] : reqs) {
    parse_energy_charts(co_await energy_charts_get(c, s, e), acc);
    ++n_req;
  }
  MonthData md;
  md.values = acc.finish();
  merge_have(c, grid, month, md.values);
  if (std::none_of(md.values.begin(), md.values.end(), [](float v) { return std::isfinite(v); }))
    throw Unavailable("Energy-Charts returned no data for " + month);
  md.meta = {{"source", "energy_charts"},
             {"url", std::string(EC_URL) + "?region=DE-Freiburg"},
             {"licence", "CC BY 4.0, attribution: Energy-Charts.info (Fraunhofer ISE)"},
             {"native_resolution_s", 1.0},
             {"measured_at", "Fraunhofer ISE, Freiburg, Germany"},
             {"timestamps", "UTC (unix seconds from the API; each marks the start of its second)"},
             {"requests", n_req}};
  if (double(t1) > now - SETTLE_S) md.meta["partial"] = true;  // fetched again (only its holes) until settled
  co_return md;
}

asio::awaitable<std::vector<std::string>> energy_charts_months(Ctx&, const std::string&) { co_return months_between(EC_FIRST, now_month()); }

// ---------------------------------------------------------------- Netztransparenz (German TSOs, CE)

namespace {

constexpr const char* NTP_URL =
    "https://www.netztransparenz.de/xspproxy/api/staticfiles/ntp-relaunch/dokumente/regelenergie/sek%C3%BCndliche%20frequenz/";
constexpr const char *NTP_FIRST = "2012-02", *NTP_LAST = "2022-06";
// TransnetBW publishes the same series one zip per month (its 2022-02 and 2022-04 files
// are byte-identical to the yearly archive's). It stands in for months the yearly
// archive lacks (2022-03).
constexpr const char* TRANSNETBW_URL = "https://webservices.transnetbw.de/files/bis/netzfrequenz/";

// Feed a monthly member through the parser; from 2016-03 each month is zipped again
// inside the yearly zip.
void ntp_read(zip::Archive& z, const zip::Entry& e, NtpParser& p, const std::string& month) {
  std::string head;
  std::string inner;
  bool nested = false, decided = false;
  z.read(e, [&](std::string_view d) {
    if (!decided) {
      head.append(d);
      if (head.size() < 2) return;
      decided = true;
      nested = zip::is_zip(head);
      d = head;
    }
    if (nested) inner.append(d);
    else p.feed(d);
    if (!head.empty()) head.clear();
  });
  if (nested) {
    zip::Archive in(inner);
    if (in.entries().empty()) throw Unavailable("empty nested archive for " + month);
    in.read(in.entries()[0], [&](std::string_view d) { p.feed(d); });
  }
}

}  // namespace

asio::awaitable<MonthData> ntp_fetch(Ctx& c, const std::string& grid, const std::string& month) {
  if (!(NTP_FIRST <= month && month <= NTP_LAST)) throw Unavailable(std::string("Netztransparenz covers ") + NTP_FIRST + ".." + NTP_LAST);
  const std::string year = month.substr(0, 4);
  std::string url = std::string(NTP_URL) + year + "_frequenz.zip";
  log("  reading Netztransparenz " + month + " ...");
  auto path = co_await cached_download(c, url, "netztransparenz_" + year + ".zip");
  MappedFile mf(path);
  zip::Archive z(mf.bytes());
  std::string prefix = month.substr(0, 4) + month.substr(5, 2);
  auto* e = z.find_if([&](auto& e) { return e.name.starts_with(prefix); });
  NtpParser p(month);
  if (e) {
    ntp_read(z, *e, p, month);
  } else {
    url = std::string(TRANSNETBW_URL) + prefix + "_Frequenz.zip";
    log("  Netztransparenz " + year + " archive has no " + month + ": reading TransnetBW's monthly file ...");
    net::Request rq;
    rq.url = url;
    rq.total_timeout_s = 600;
    auto r = co_await c.http.fetch(std::move(rq));
    if (r.error.empty() && r.status == 404)
      throw Unavailable("Netztransparenz " + year + " archive has no file for " + month + ", nor has TransnetBW");
    if (!r.ok()) throw std::runtime_error("download failed: " + url + ": " + (r.error.empty() ? "HTTP " + std::to_string(r.status) : r.error));
    zip::Archive tz(r.body);
    if (tz.entries().empty()) throw Unavailable("TransnetBW's " + month + " archive is empty");
    ntp_read(tz, tz.entries()[0], p, month);
  }
  auto ser = p.finish();
  if (std::none_of(ser.own.begin(), ser.own.end(), [](float v) { return std::isfinite(v); }))
    throw Unavailable("Netztransparenz " + month + " holds no valid data");
  MonthData md;
  md.values = std::move(ser.own);
  md.spill = std::move(ser.spill);
  md.meta = {{"source", "netztransparenz"},
             {"url", url},
             {"licence", "netztransparenz.de (German TSOs), published data"},
             {"native_resolution_s", 1.0},
             {"timestamps", "Europe/Berlin wall-clock, converted to UTC, then -2 s (measured against GPS-timed data)"},
             {"rows", p.rows()}};
  co_return md;
}

asio::awaitable<std::vector<std::string>> ntp_months(Ctx&, const std::string&) { co_return months_between(NTP_FIRST, NTP_LAST); }

// ---------------------------------------------------------------- Open-ENF (CE before the OSF archive)

namespace {

// Chris Hodges' Open-ENF database "DE_50_2010-2021" (Zenodo 7809233, CC BY 4.0): a JSON
// header ({"gridId":"DE","startDate":1262300400,"baseFrequency":50}) padded with CRs to
// 62 bytes, then one little-endian int16 per second from startDate: the deviation in mHz,
// -32768 and -2000 marking no data. It runs to 2022-07-29. From 2011-07 it is the German
// TSOs' series (osf_pgf, netztransparenz) again, only 2 s late, so only what precedes
// the OSF archive is read:
//  - 2010-01..2010-12: 1 s at 10 mHz steps, from a recorder the release does not name.
//    It is Continental Europe (the hourly schedule-change steps match 2012's, r = 0.99),
//    in UTC (the daily pattern lines up at lag 0 in summer and winter; no gap or
//    duplicate hour at the 2010 clock changes), and about 5 s late: the hour steps sit
//    3 s later than in Open-ENF's own 2012 data (quarterly +4, +3, +2, +3 s; the same
//    method reads 0..-1 s on 2011-2016 controls), which is 2 s late. The drift points to
//    a recorder without GPS time, so these months are good to about +-2 s.
//  - 2011-01: empty.
//  - 2011-02..2011-06: one sample per 4 s, 1 mHz. The German TSOs' 4 s series: OSF's
//    germany_2011_12 is the same 4 s grid and matches Open-ENF at +2 s, r = 0.9996.
//  - 2012-01: the same 4 s series. OSF's germany_2012_01 holds timestamps without values
//    and Netztransparenz starts 2012-02, so this is the only source for the month.
constexpr const char* OENF_URL = "https://zenodo.org/records/7809233/files/DE_50_2010-2021.freq.freqdb?download=1";
constexpr i64 OENF_START = 1262300400;  // 2010-01-01 00:00 CET
constexpr std::uint64_t OENF_DATA = 62;
constexpr const char *OENF_FIRST = "2010-01", *OENF_LAST = "2012-01";
int oenf_late_s(const std::string& month) { return month < "2011-01" ? 5 : 2; }

bool oenf_used(const std::string& m) {
  return OENF_FIRST <= m && m <= OENF_LAST && m != "2011-01" && !("2011-07" <= m && m <= "2011-12");  // OSF has 2011-07..12
}
std::vector<std::string> oenf_month_list() {
  auto v = months_between(OENF_FIRST, OENF_LAST);
  std::erase_if(v, [](auto& m) { return !oenf_used(m); });
  return v;
}

}  // namespace

asio::awaitable<MonthData> oenf_fetch(Ctx& c, const std::string& grid, const std::string& month) {
  if (!oenf_used(month)) throw Unavailable("Open-ENF is used for 2010-01..2010-12, 2011-02..2011-06 and 2012-01 only");
  // The header must still be the one these offsets were worked out from.
  std::string head;
  co_await stream_url(c, OENF_URL, [&](std::string_view d) { head.append(d); }, std::pair<std::uint64_t, std::uint64_t>{0, OENF_DATA - 1});
  if (head.size() != OENF_DATA || head.find("\"startDate\":1262300400") == std::string::npos || head.find("\"gridId\":\"DE\"") == std::string::npos ||
      head.find_first_not_of('\r', head.find('}') + 1) != std::string::npos)
    throw std::runtime_error("Open-ENF file header changed: " + head.substr(0, head.find('}') + 1));
  auto [t0, t1] = month_bounds(month);
  const int late = oenf_late_s(month);
  // Stamps [t0 + late, t1 + late) hold the true seconds [t0, t1).
  const std::uint64_t a = OENF_DATA + 2 * std::uint64_t(t0 + late - OENF_START);
  const std::uint64_t b = OENF_DATA + 2 * std::uint64_t(t1 + late - OENF_START);
  log("  reading Open-ENF " + month + " (" + std::to_string((b - a) >> 20) + " MiB range) ...");
  std::string raw;
  raw.reserve(b - a);
  co_await stream_url(c, OENF_URL, [&](std::string_view d) { raw.append(d); }, std::pair{a, b - 1});
  if (raw.size() != b - a) throw std::runtime_error("Open-ENF: short read for " + month);
  store::RegularAccumulator acc(t0, t1);  // interpolates the 4 s months
  std::size_t rows = 0;
  for (std::size_t i = 0; i + 1 < raw.size(); i += 2) {
    auto v = std::int16_t(std::uint16_t(std::uint8_t(raw[i])) | std::uint16_t(std::uint8_t(raw[i + 1])) << 8);
    if (v <= -1000 || v >= 1000) continue;  // -32768 / -2000: no data
    acc.add(double(t0) + double(i / 2), v * 1e-3);
    ++rows;
  }
  MonthData md;
  md.values = acc.finish();
  if (!any_finite(md.values)) throw Unavailable("Open-ENF holds no data for " + month);
  const bool y2010 = month < "2011-01";
  md.meta = {{"source", "open_enf"},
             {"url", OENF_URL},
             {"licence", "CC BY 4.0 (Chris Hodges, Open-ENF; Zenodo 7809233)"},
             {"record", "https://doi.org/10.5281/zenodo.7809233"},
             {"native_resolution_s", y2010 ? 1.0 : 4.0},
             {"quantisation_hz", y2010 ? 0.01 : 0.001},
             {"timestamps", y2010 ? "UTC, then -5 s (hour steps 3 s later than Open-ENF's own 2012 data, which is 2 s late; recorder "
                                    "without GPS time: about +-2 s)"
                                  : "UTC, then -2 s (the German TSOs' series: matches OSF germany_2011_12 at +2 s, r = 0.9996)"},
             {"measured_at", y2010 ? "not stated (Open-ENF 'DE' series)" : "German TSOs (as netztransparenz)"},
             {"rows", rows}};
  co_return md;
}

asio::awaitable<std::vector<std::string>> oenf_months(Ctx&, const std::string&) { co_return oenf_month_list(); }

// ---------------------------------------------------------------- Ireland (FDR, OSF m43tg)

namespace {

const std::map<std::string, std::string> FDR_YEARS = {{"2014", "https://osf.io/download/y9nrh/"},
                                                      {"2015", "https://osf.io/download/38fnx/"},
                                                      {"2016", "https://osf.io/download/8dqk2/"},
                                                      {"2017", "https://osf.io/download/5rsq9/"}};
constexpr const char *FDR_FIRST = "2014-05", *FDR_LAST = "2017-11";

}  // namespace

asio::awaitable<MonthData> fdr_fetch(Ctx& c, const std::string& grid, const std::string& month) {
  if (!(FDR_FIRST <= month && month <= FDR_LAST)) throw Unavailable(std::string("the Irish FDR archive covers ") + FDR_FIRST + ".." + FDR_LAST);
  const std::string year = month.substr(0, 4);
  const int mon = std::stoi(month.substr(5, 2));
  const std::string& url = FDR_YEARS.at(year);
  auto [t0, t1] = month_bounds(month);
  store::RegularAccumulator acc(t0, t1);
  log("  reading Irish FDR " + month + " from OSF (range requests into the " + year + " archive) ...");
  zip::Remote z(c.http, url);
  co_await z.open();
  // Folders are "fdr2015/03_Mar15/"; a day can sit in its neighbour's folder, so the
  // adjacent folders are read too and rows are kept by their own dates.
  static const std::regex re(R"(/(\d{2})_[A-Za-z]{3}\d{2}/fdr-(\d{2})(\d{2}))");
  std::vector<zip::Entry> picked;
  for (auto& e : z.entries()) {
    if (!ends_with_ci(e.name, ".txt")) continue;
    std::smatch m;
    if (!std::regex_search(e.name, m, re)) continue;
    int folder = std::stoi(m[1]), fmon = std::stoi(m[2]);
    if (folder < mon - 1 || folder > mon + 1) continue;
    if (folder != mon && fmon != mon) continue;
    picked.push_back(e);
  }
  if (picked.empty()) throw Unavailable("Irish FDR " + year + " archive has no files for " + month);
  std::size_t rows = 0;
  co_await z.for_each(picked, [&](const zip::Entry& e, std::span<const std::byte> data) {
    rows += parse_fdr_day(zip::read_all(data, e), acc);
  });
  MonthData md;
  md.values = acc.finish();
  if (std::none_of(md.values.begin(), md.values.end(), [](float v) { return std::isfinite(v); }))
    throw Unavailable("Irish FDR holds no valid data in " + month);
  md.meta = {{"source", "osf_ireland"},
             {"url", url},
             {"licence", "see https://power-grid-frequency.org (Power Grid Frequency DB, independent measurements)"},
             {"native_resolution_s", 0.1},
             {"measured_at", "Dublin area, Ireland (53.20 N, 6.22 W)"},
             {"timestamps", "UTC (GPS; DST-change days hold 24 full hours)"},
             {"rows", rows},
             {"daily_files", picked.size()}};
  co_return md;
}

asio::awaitable<std::vector<std::string>> fdr_months(Ctx&, const std::string&) { co_return months_between(FDR_FIRST, FDR_LAST); }

// ---------------------------------------------------------------- EirGrid (Ireland, current)

namespace {

// Smart Grid Dashboard chart API: one request per Irish calendar day, 5 s values rounded
// to 0.01 Hz, Irish wall-clock time (the spring-change hour is empty), 2 s late against
// KIT's GPS-timed Irish recorder (2021-12, r = 0.997). Data starts 2020-07.
constexpr const char* EIRGRID_API = "https://www.smartgriddashboard.com/api/chart/";
constexpr const char* EIRGRID_FIRST = "2020-07";  // June 2020 answers with null values only
constexpr int EIRGRID_SHIFT_S = -2;

std::string eirgrid_day(i64 day_start) {
  static const char* MON[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
  auto c = civil_from_days(day_start / 86400);
  char b[16];
  std::snprintf(b, sizeof b, "%02d-%s-%04d", int(c.d), MON[c.m - 1], c.y);
  return b;
}

}  // namespace

asio::awaitable<MonthData> eirgrid_fetch(Ctx& c, const std::string& grid, const std::string& month) {
  if (month < EIRGRID_FIRST) throw Unavailable(std::string("EirGrid's dashboard frequency starts ") + EIRGRID_FIRST);
  auto [t0, t1] = month_bounds(month);
  const double now = now_seconds();
  if (double(t0) > now) throw Unavailable(month + " is in the future");
  store::RegularAccumulator acc(t0, t1);
  // Irish days overlapping the UTC month: the month's days and the next month's first
  // (Irish summer time runs an hour ahead of UTC).
  std::vector<i64> days;
  // A refetch of a partial month skips the days already stored (an Irish day is at
  // most an hour ahead of its UTC day).
  for (i64 d = t0; d <= t1 && double(d) <= now; d += 86400)
    if (!already_have(c, grid, month, d - 3600, d + 86400)) days.push_back(d);
  log("  reading EirGrid frequency " + month + " (" + std::to_string(days.size()) + " daily requests) ...");
  // The API now and then answers a day with {"Rows":[]} (HTTP 200): such a day is asked
  // again with back-off. A day without data comes back as rows of nulls: not retried.
  std::size_t rows = 0, empty_days = 0;
  co_await net::parallel_for(days, 2, [&](i64 d) -> asio::awaitable<void> {
    const std::string day = eirgrid_day(d);
    const std::string url = std::string(EIRGRID_API) + "?region=ALL&chartType=frequency&dateRange=day&dateFrom=" + day + "&dateTo=" + day +
                            "&areas=frequency";
    for (int attempt = 0; attempt < 5; ++attempt) {
      if (attempt) co_await net::sleep_for(double(2 << attempt));
      LocalToUtc tz("Europe/Dublin");  // one day's rows are chronological: the autumn fold resolves in order
      std::size_t listed = 0;
      std::size_t n = parse_eirgrid_frequency(co_await get_text(c, url), tz, EIRGRID_SHIFT_S, acc, &listed);
      rows += n;
      if (listed) co_return;
    }
    ++empty_days;
    log("  EirGrid returned no frequency for " + day);
  });
  MonthData md;
  md.values = acc.finish();
  merge_have(c, grid, month, md.values);
  if (!any_finite(md.values)) throw Unavailable("EirGrid has no frequency for " + month);
  md.meta = {{"source", "eirgrid"},
             {"url", EIRGRID_API},
             {"licence", "EirGrid Smart Grid Dashboard, published data"},
             {"native_resolution_s", 5.0},
             {"quantisation_hz", 0.01},
             {"timestamps", "Europe/Dublin wall-clock time, converted to UTC, then -2 s (measured against KIT's GPS-timed Irish recorder)"},
             {"rows", rows},
             {"empty_days", empty_days}};
  if (double(t1) > now - SETTLE_S) md.meta["partial"] = true;  // fetched again (only its holes) until settled
  co_return md;
}

asio::awaitable<std::vector<std::string>> eirgrid_months(Ctx&, const std::string&) { co_return months_between(EIRGRID_FIRST, now_month()); }

}  // namespace geo::refs::detail

namespace geo::refs {

void check_neso_seam(const std::string& grid, const std::string& month, const std::vector<float>& own) {
  detail::neso_check_seam(grid, month, own);
}

FingridMonth parse_fingrid_archive(const std::string& path, const std::string& month) {
  auto [t0, t1] = month_bounds(month);
  store::RegularAccumulator acc(t0 - SPILL_S, t1 + SPILL_S);
  int n_files = 0;
  std::string head(8, '\0');
  {
    std::ifstream f(path, std::ios::binary);
    f.read(head.data(), 8);
  }
  if (zip::is_zip(head)) {
    MappedFile mf(path);
    zip::Archive z(mf.bytes());
    std::vector<zip::Entry> csvs;
    for (auto& e : z.entries())
      if (ends_with_ci(e.name, ".csv")) csvs.push_back(e);
    std::sort(csvs.begin(), csvs.end(), [](auto& a, auto& b) { return a.name < b.name; });
    for (auto& e : csvs) {
      parse_fingrid_day(z.read_all(e), acc);
      ++n_files;
    }
  } else if (zip::is_7z(head)) {
    std::string member;  // one daily file at a time (~27 MB)
    bool csv = false;
    zip::for_each_libarchive(path, {[&](const std::string& name) { return (csv = ends_with_ci(name, ".csv")); },
                                    [&](std::string_view d) { member.append(d); },
                                    [&] {
                                      if (csv) {
                                        parse_fingrid_day(member, acc);
                                        ++n_files;
                                      }
                                      member.clear();
                                    }});
  } else {
    throw std::runtime_error("unknown archive format (expected zip or 7z)");
  }
  auto ser = split_spill(acc.finish(), t0 - SPILL_S, month);
  FingridMonth out;
  out.own = std::move(ser.own);
  for (auto& sp : ser.spill) out.spill.emplace_back(sp.start, std::move(sp.values));
  out.daily_files = n_files;
  out.rows = acc.n_samples();
  return out;
}

std::vector<std::pair<i64, i64>> energy_charts_requests(const std::string& month, double now) {
  auto [t0, t1] = month_bounds(month);
  std::vector<std::pair<i64, i64>> out;
  for (i64 s = t0; double(s) < std::min(double(t1), now);) {
    i64 e = std::min(t1, s + detail::EC_SPAN_S);
    out.emplace_back(s, e - 1);  // the API's end is inclusive
    s = e;
  }
  return out;
}

}  // namespace geo::refs
