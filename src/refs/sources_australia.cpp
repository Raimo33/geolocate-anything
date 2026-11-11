// AEMO (Australia): Frequency Performance Payments, Causer Pays and the yearly workbooks.
#include "refs/source_impl.hpp"

#include <algorithm>
#include <cmath>
#include <stdexcept>

#include "net/zip.hpp"
#include "store/store.hpp"
#include "util/text.hpp"

namespace geo::refs::detail {

// ---------------------------------------------------------------- AEMO (Australia)

namespace {

constexpr const char* NEMWEB = "https://nemweb.com.au";
constexpr const char* AEMO_FPP_FIRST = "2024-12";
constexpr const char *AEMO_CP_FIRST = "2011-01", *AEMO_CP_LAST = "2017-03";

json::object aemo_meta(const std::string& source, const std::string& url, std::size_t rows, bool partial) {
  json::object m{{"source", source},
                 {"url", url},
                 {"licence", "AEMO copyright permissions (free use with attribution)"},
                 {"native_resolution_s", 4.0},
                 {"timestamps", "NEM time (UTC+10, no DST), converted to UTC"},
                 {"rows", rows}};
  if (partial) m["partial"] = true;
  return m;
}

// A directory that is not there (yet) lists nothing; any other failure is an error, so
// that an outage of nemweb is not taken for "nothing published".
asio::awaitable<std::vector<std::string>> nemweb_links(Ctx& c, const std::string& path, const std::string& pattern) {
  std::string last;
  for (int attempt = 0; attempt < 3; ++attempt) {
    if (attempt) co_await net::sleep_for(double(2 << attempt));
    net::Request rq;
    rq.url = NEMWEB + path;
    rq.total_timeout_s = 120;
    auto r = co_await c.http.fetch(std::move(rq));
    if (r.ok()) co_return parse_html_links(r.body, NEMWEB, pattern);
    if (r.error.empty() && (r.status == 404 || r.status == 403)) co_return std::vector<std::string>{};
    last = r.error.empty() ? "HTTP " + std::to_string(r.status) : r.error;
  }
  throw std::runtime_error(std::string("cannot list ") + NEMWEB + path + ": " + last);
}

// Whether a partial month already holds the UTC span [a, b) of one NEM-day file: the part
// inside the month from what is being fetched again, the part before it from the store
// (the previous month's last hours come from this month's first file).
bool fpp_day_held(const Ctx& c, const std::string& grid, const std::string& month, i64 a, i64 b) {
  if (!already_have(c, grid, month, a, b)) return false;
  const i64 t0 = month_bounds(month).first;
  if (a >= t0) return true;
  auto v = store::read(grid, a, t0, {"aemo_fpp"});
  return double(std::count_if(v.begin(), v.end(), [](float x) { return std::isfinite(x); })) >= 0.99 * double(v.size());
}

// The mainland series is NSW1's, its gaps filled from QLD1's (within a second of NSW1).
// SA1 and VIC1 run 5-6 s behind NSW1 (2025-01 and 2026-09, every 10-day window), so the
// four regions are not averaged: their mean ran 2 s late, 2-3 s behind AEMO's yearly
// workbooks, which are sampled in NSW.

}  // namespace

const std::map<std::string, std::vector<Location>> AEMO_LOCATIONS = {
    {"australia_nem", {{"NEM mainland (Sydney as a point inside)", -33.87, 151.21}}},
    {"australia_tas", {{"Tasmania (Hobart as a point inside)", -42.88, 147.33}}}};

asio::awaitable<MonthData> aemo_fpp_fetch(Ctx& c, const std::string& grid, const std::string& month) {
  if (month < AEMO_FPP_FIRST) throw Unavailable(std::string("AEMO FPP frequency starts ") + AEMO_FPP_FIRST);
  auto [t0, t1] = month_bounds(month);
  if (double(t0) > now_seconds()) throw Unavailable(month + " is in the future");
  const std::string y = month.substr(0, 4), m = month.substr(5, 2);
  // NEM time: the month's files start 10 h before the UTC month (those hours go to the
  // previous month), and its last 10 h are in the next month's first file. Older
  // monthly archives also hold the next month's first NEM day: kept as spill too.
  const i64 a = t0 - SPILL_S, e = t1 + SPILL_S;
  store::RegularAccumulator nsw(a, e), qld(a, e), tas(a, e);
  FppParser p({{"NSW1", &nsw}, {"QLD1", &qld}, {"TAS1", &tas}});
  // Monthly archive (about a month after the month ends) ...
  auto files = co_await nemweb_links(c, "/Data_Archive/Wholesale_Electricity/MMSDM/" + y + "/MMSDM_" + y + "_" + m +
                                            "/MMSDM_Historical_Data_SQLLoader/DATA/",
                                     R"(FPP_REGION_FREQ_MEASURE.*\.zip$)");
  bool partial = false;
  std::string first_url;
  if (!files.empty()) {
    first_url = files[0];
    for (auto& url : files) {
      log("  downloading AEMO FPP " + month + " (" + url.substr(url.rfind('/') + 1) + ") ...");
      TempFile tmp(".zip", tmp_dir());
      co_await download_to(c, url, tmp.path);
      MappedFile mf(tmp.path);
      zip::Archive z(mf.bytes());
      for (auto& e : z.entries()) z.read(e, [&](std::string_view d) { p.feed(d); });
      p.finish();
    }
  } else {
    // ... until then, the daily FPP run archive (kept for about a year).
    auto days = co_await nemweb_links(c, "/Reports/ARCHIVE/FPPRUN/", "PUBLIC_FPP_RUN_" + y + m + R"(\d\d\.zip$)");
    if (days.empty()) throw Unavailable("AEMO has published no FPP frequency for " + month + " yet");
    first_url = days[0];
    // A refetch of a partial month skips the NEM days (UTC+10) both grids already hold.
    std::erase_if(days, [&](const std::string& url) {
      auto day = parse_iso_datetime(url.substr(url.size() - 12, 4) + "-" + url.substr(url.size() - 8, 2) + "-" + url.substr(url.size() - 6, 2));
      if (!day) return false;
      const i64 da = day->secs - NEM_UTC_OFFSET_S, db = da + 86400;
      return fpp_day_held(c, "australia_nem", month, da, db) && fpp_day_held(c, "australia_tas", month, da, db);
    });
    log("  downloading AEMO FPP " + month + " from " + std::to_string(days.size()) + " daily files ...");
    for (auto& url : days) {
      auto body = (co_await c.http.get(url)).body;
      zip::Archive outer(body);
      for (auto& e : outer.entries()) {
        std::string inner_bytes = outer.read_all(e);
        zip::Archive inner(inner_bytes);
        for (auto& ie : inner.entries()) inner.read(ie, [&](std::string_view d) { p.feed(d); });
        p.finish();
      }
    }
    partial = true;  // incomplete until AEMO's monthly archive for it appears
  }
  auto nem_all = nsw.finish();
  const std::int64_t from_qld = fill_gaps(nem_all, qld.finish());
  const std::string og = grid == "australia_tas" ? "australia_nem" : "australia_tas";
  auto own = split_spill(grid == "australia_tas" ? tas.finish() : nem_all, a, month);
  auto oth = split_spill(grid == "australia_tas" ? nem_all : tas.finish(), a, month);
  merge_have(c, grid, month, own.own);
  merge_have(c, og, month, oth.own);
  if (!any_finite(own.own)) throw Unavailable("aemo_fpp holds no " + grid + " data in " + month);
  auto meta = [&](const std::string& g) {
    auto out = aemo_meta("aemo_fpp", first_url, p.rows, partial);
    if (g == "australia_nem") {
      out["regions"] = "NSW1, gaps from QLD1 (SA1 and VIC1 run 5-6 s late: not used)";
      out["filled_from_qld1_s"] = from_qld;
    }
    return out;
  };
  MonthData md;
  md.values = std::move(own.own);
  md.spill = std::move(own.spill);
  md.meta = meta(grid);
  if (any_finite(oth.own)) md.also.push_back({og, std::move(oth.own), meta(og), std::move(oth.spill)});
  co_return md;
}

asio::awaitable<MonthData> aemo_cp_fetch(Ctx& c, const std::string& grid, const std::string& month) {
  if (!(AEMO_CP_FIRST <= month && month <= AEMO_CP_LAST))
    throw Unavailable(std::string("AEMO's Causer Pays archive covers ") + AEMO_CP_FIRST + ".." + AEMO_CP_LAST);
  const std::string y = month.substr(0, 4), m = month.substr(5, 2);
  const std::string url = std::string(NEMWEB) + "/Data_Archive/Wholesale_Electricity/FCAS_Causer_Pays/" + y + "/FCAS_Causer_Pays_" + y + "_" + m + ".zip";
  auto [t0, t1] = month_bounds(month);
  // NEM time: the archive's month starts 10 h before the UTC month: keep a margin.
  const i64 a = t0 - SPILL_S;
  store::RegularAccumulator nem(a, t1 + SPILL_S), tas(a, t1 + SPILL_S);
  std::map<std::string, store::RegularAccumulator*> accs{{"australia_nem", &nem}, {"australia_tas", &tas}};
  std::size_t rows = 0;
  log("  downloading AEMO Causer Pays " + month + " (~1.8 GB, only the frequency rows are kept) ...");
  zip::Remote z(c.http, url);
  co_await z.open();
  std::vector<zip::Entry> inner;
  for (auto& e : z.entries())
    if (ends_with_ci(e.name, ".zip")) inner.push_back(e);
  co_await z.for_each(inner, [&](const zip::Entry& e, std::span<const std::byte> data) {
    std::string bytes = zip::read_all(data, e);
    zip::Archive in(bytes);
    for (auto& ce : in.entries()) rows += parse_causer_pays_csv(in.read_all(ce), accs);
  });
  auto own = split_spill((grid == "australia_tas" ? tas : nem).finish(), a, month);
  auto oth = split_spill((grid == "australia_tas" ? nem : tas).finish(), a, month);
  if (!any_finite(own.own)) throw Unavailable("Causer Pays holds no " + grid + " frequency in " + month);
  MonthData md;
  md.values = std::move(own.own);
  md.spill = std::move(own.spill);
  md.meta = aemo_meta("aemo_causer_pays", url, rows, false);
  std::string og = grid == "australia_tas" ? "australia_nem" : "australia_tas";
  if (any_finite(oth.own)) md.also.push_back({og, std::move(oth.own), aemo_meta("aemo_causer_pays", url, rows, false), std::move(oth.spill)});
  co_return md;
}

asio::awaitable<std::vector<std::string>> aemo_fpp_months(Ctx&, const std::string&) { co_return months_between(AEMO_FPP_FIRST, now_month()); }

// AEMO's yearly frequency workbooks: aemo.com.au refuses automated downloads, so months
// arrive only through `geoloc refs import-aemo` (and the bucket); nothing to fetch.
asio::awaitable<MonthData> aemo_annual_fetch(Ctx&, const std::string&, const std::string& month) {
  throw Unavailable("aemo_annual is imported by hand (geoloc refs import-aemo FILE ...), not fetched: " + month);
  co_return MonthData{};
}
asio::awaitable<std::vector<std::string>> aemo_annual_months(Ctx&, const std::string&) { co_return std::vector<std::string>{}; }
asio::awaitable<std::vector<std::string>> aemo_cp_months(Ctx&, const std::string&) { co_return months_between(AEMO_CP_FIRST, AEMO_CP_LAST); }

}  // namespace geo::refs::detail
