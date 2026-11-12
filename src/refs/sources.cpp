#include "refs/sources.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <set>
#include <stdexcept>

#include "grids.hpp"
#include "refs/seismic_fetch.hpp"
#include "refs/source_impl.hpp"
#include "store/store.hpp"
#include "util/text.hpp"

namespace geo::refs {

using namespace detail;

namespace {

// Whether a month marked unavailable is still to be believed.
// "No data" answers are asked again once they are older than 90 days: fetchers learn to
// find months they used to report missing, and archives fill gaps.
bool unavailable_stands(const json::value& entry) {
  const json::object* o = entry.if_object();
  const json::value* at = o ? o->if_contains("checked_at") : nullptr;
  if (!at || !at->is_string()) return false;
  auto t = parse_iso_utc(std::string(at->get_string()));
  return t && now_seconds() - double(*t) < 90 * 86400.0;
}

// ---------------------------------------------------------------- seismic hum (US Eastern, Western, ERCOT, IPS/UPS, ...)

asio::awaitable<MonthData> seismic_fetch(Ctx& c, const seismic::Network& net, const std::string& grid, const std::string& month) {
  const std::string name = net.source;
  log("  " + name + " " + month + ": tracking the " + std::to_string(int(net.mains_hz)) + " Hz hum at the best-scoring " + net.archive + " stations ...");
  seismic::MonthResult r;
  try {
    // A partial month fetched again downloads only what it lacks: the chunks whose
    // downloads failed last time and those past where it stopped (the current month).
    // Without that record (older entries), every chunk not stored yet.
    std::set<i64> retry;
    i64 fetched_to = -1;
    if (auto info = store::month_info(grid, name, month); info && jbool(*info, "partial") && c.have.contains({grid, month}))
      if (auto* ft = info->if_contains("fetched_to"); ft && ft->is_int64()) {
        fetched_to = ft->as_int64();
        if (auto* rc = info->if_contains("retry_chunks"); rc && rc->is_array())
          for (auto& v : rc->as_array())
            if (v.is_int64()) retry.insert(v.as_int64());
      }
    r = co_await seismic::fetch_month(
        c.http, net, month,
        [&](i64 a, i64 b) {
          // (any failed chunk start inside [a, b): stored chunks may be shorter than today's)
          if (fetched_to >= 0) {
            auto it = retry.lower_bound(a);
            return (it == retry.end() || *it >= b) && b <= fetched_to;
          }
          return already_have(c, grid, month, a, b);
        });
  } catch (const seismic::NoData& e) {
    throw Unavailable(e.what());
  }
  MonthData md;
  md.values = std::move(r.values);
  merge_have(c, grid, month, md.values);
  md.meta = std::move(r.meta);
  co_return md;
}

// One fetcher and lister per seismic network (the source table holds plain functions).
template <const seismic::Network& (*NET)()>
asio::awaitable<MonthData> net_fetch(Ctx& c, const std::string& grid, const std::string& month) {
  return seismic_fetch(c, NET(), grid, month);
}
template <const seismic::Network& (*NET)()>
asio::awaitable<std::vector<std::string>> net_months(Ctx&, const std::string&) {
  co_return seismic::months(NET());
}
// ---------------------------------------------------------------- the table

std::vector<Source>& table() {
  static std::vector<Source> t = [] {
    std::vector<Source> v;
    v.push_back({{"neso", {"great_britain"}, "NESO historic system frequency, Great Britain, 1 s, 2014 to last month", "NESO Open Data Licence", true, 1},
                 &neso_fetch, &neso_months, NESO_FORMAT,
                 {{"great_britain", {{"Great Britain (national system frequency; Birmingham as a point inside)", 52.48, -1.90}}}}});
    v.push_back({{"osf_pgf", {"continental_europe"},
                  "Power Grid Frequency DB (KIT/Jülich, OSF m43tg): Continental Europe 2011-07..2020-09 (Germany 1 s, gaps from France)",
                  "CC BY 4.0 (check per folder)", false, 4},
                 &osf_fetch, &osf_months, 2,
                 {{"continental_europe", {OSF_LOCATIONS.at("Germany"), OSF_LOCATIONS.at("France")}}}});
    v.push_back({{"netztransparenz", {"continental_europe"},
                  "German TSOs' per-second frequency (netztransparenz.de; 2022-03 from TransnetBW), Continental Europe, 1 s, 2012-02..2022-06",
                  "netztransparenz.de published data", false, 4},
                 &ntp_fetch, &ntp_months, 1, {{"continental_europe", {OSF_LOCATIONS.at("Germany")}}}});
    v.push_back({{"open_enf", {"continental_europe"},
                  "Open-ENF DE database (Zenodo 7809233), Continental Europe before the OSF archive: 2010 at 1 s/10 mHz (about +-2 s), "
                  "2011-02..06 and 2012-01 (empty in the OSF archive) at 4 s",
                  "CC BY 4.0", false, 2},
                 &oenf_fetch, &oenf_months, 1, {{"continental_europe", {OSF_LOCATIONS.at("Germany")}}}});
    v.push_back({{"fingrid", {"nordic"}, "Fingrid open data 339, Nordic, 10 Hz averaged to 1 s, 2015 to last month (no key needed)", "CC BY 4.0",
                  true, 2},
                 &fingrid_fetch, &fingrid_months, 2,
                 {{"nordic", {{"Finland (Fingrid; Helsinki as a point inside)", 60.17, 24.94}}}}});
    v.push_back({{"energy_charts", {"continental_europe"},
                  "Energy-Charts (Fraunhofer ISE, Freiburg), Continental Europe, 1 s, 2022-05 to today (~5 min/month: rate-limited API)",
                  "CC BY 4.0, attribution Energy-Charts.info", true, 4},
                 &energy_charts_fetch, &energy_charts_months, 1, {{"continental_europe", {{"Freiburg, Germany", 47.99, 7.85}}}}});
    v.push_back({{"osf_ireland", {"ireland"}, "Irish frequency disturbance recorder (Power Grid Frequency DB), 10 Hz averaged to 1 s, 2014-05..2017-12",
                  "see power-grid-frequency.org", false, 2},
                 &fdr_fetch, &fdr_months, 1, {{"ireland", {{"Dublin area, Ireland", 53.20, -6.22}}}}});
    v.push_back({{"aemo_fpp", {"australia_nem", "australia_tas"},
                  "AEMO Frequency Performance Payments region frequency, mainland NEM + Tasmania, 4 s, 2024-12 to today",
                  "AEMO copyright permissions", true, 2, true},
                 &aemo_fpp_fetch, &aemo_fpp_months, AEMO_FPP_FORMAT, AEMO_LOCATIONS});
    v.push_back({{"aemo_causer_pays", {"australia_nem", "australia_tas"},
                  "AEMO Causer Pays 4 s FCAS data (frequency elements), mainland NEM + Tasmania, 2011-01..2017-03 (~1.8 GB/month)",
                  "AEMO copyright permissions", false, 2, true},
                 &aemo_cp_fetch, &aemo_cp_months, 1, AEMO_LOCATIONS});
    v.push_back({{"aemo_annual", {"australia_nem", "australia_tas"},
                  "AEMO yearly frequency workbooks (aemo.com.au), mainland NEM and Tasmania 2010+, 4 s: downloaded by hand, "
                  "then `geoloc refs import-aemo FILE ...`",
                  "AEMO copyright permissions", false, 1},
                 &aemo_annual_fetch, &aemo_annual_months, 1, AEMO_LOCATIONS});
    v.push_back({{"osf_edr", edr_grids(),
                  "KIT EDR campaigns (OSF by5hu), 1 s, days to months per site: North America, Russia/Estonia, Balearic, Canaries, Faroe, "
                  "Iceland, South Africa, Korea, Malaysia",
                  "CC BY 4.0", false, 4},
                 &edr_fetch, &edr_months, 1, {}});
    v.push_back({{"kit_zenodo", {"balearic", "iceland", "ireland"},
                  "KIT/Jülich grid frequency release (Zenodo 21997337), 1 s: Mallorca 2021-01..2025-11, Iceland 2021-11..2022-01, "
                  "Ireland 2021-12",
                  "CC BY 4.0", false, 2},
                 &zk_fetch, &zk_months, 1, ZK_LOCATIONS});
    v.push_back({{"eirgrid", {"ireland"},
                  "EirGrid Smart Grid Dashboard, all-island system frequency, 5 s at 0.01 Hz, mid-2020 to today (one request per day)",
                  "EirGrid published data", true, 2},
                 &eirgrid_fetch, &eirgrid_months, 1, {{"ireland", {{"Ireland (all-island system frequency; Dublin as a point inside)", 53.35, -6.26}}}}});
    v.push_back({{"aeso", {"us_western"}, "AESO system frequency (Alberta, Western Interconnection), 2 s, 2019-01..2025-07 (one zip per year)",
                  "AESO data request publication", false, 2},
                 series_fetcher("aeso"), series_lister("aeso"), 1,
                 {{"us_western", {{"Alberta (AESO system frequency; Calgary as a point inside)", 51.05, -114.07}}}}});
    v.push_back({{"grid_india_pmu", {"india"},
                  "Grid-India PMU at Balipara 400 kV, Assam (IIT Guwahati, Mendeley Data), 1 s, 2020-12..2023-08 (five ~2 GB CSVs)",
                  "CC BY 4.0", false, 2},
                 series_fetcher("grid_india_pmu"), series_lister("grid_india_pmu"), 1,
                 {{"india", {{"Balipara 400 kV substation, Assam", 26.82, 92.75}}}}});
    v.push_back({{"pmu_zenodo", {"southern_africa", "great_britain"},
                  "PMUs in Stellenbosch and London (Lonardi, Schäfer, Beck; Zenodo 21461024), 1 s, 2025-04-28..2025-12-31", "CC BY 4.0", false, 2},
                 series_fetcher("pmu_zenodo"), series_lister("pmu_zenodo"), 2,
                 {{"southern_africa", {{"Stellenbosch, South Africa", -33.93, 18.86}}}, {"great_britain", {{"London", 51.51, -0.13}}}}});
    v.push_back({{"osf_baltic", {"ips_ups", "baltic_island_2025"},
                  "KIT recorders in Kaunas and Tallinn (OSF m43tg), 0.1 s: IPS/UPS 2025-01-23..02-08 07:08 UTC, the Baltic island "
                  "2025-02-08 07:11..02-09 12:04 UTC",
                  "CC BY 4.0", false, 2},
                 series_fetcher("osf_baltic"), series_lister("osf_baltic"), 1,
                 {{"ips_ups", {{"Kaunas, Lithuania", 54.90, 23.89}, {"Tallinn, Estonia", 59.44, 24.75}}},
                  {"baltic_island_2025", {{"Kaunas, Lithuania", 54.90, 23.89}, {"Tallinn, Estonia", 59.44, 24.75}}}}});
    v.push_back({{"ercot_etts", {"ercot"},
                  "ERCOT EMS system frequency (ERCOT.FHZ, ETTS archive), 4 s: 2011-07/08 whole, other 2011 months days 1-7, "
                  "2012-01..06 about 3 days each",
                  "ERCOT public posting", false, 1},
                 &ercot_etts_fetch, &ercot_etts_months, 1, {{"ercot", {{"ERCOT (system frequency; Austin as a point inside)", 30.27, -97.74}}}}});
    v.push_back({{"lbnl_upmu", {"us_western"},
                  "LBNL micro-PMU reference dataset (Berkeley distribution feeder), frequency from the 120 Hz voltage angle, "
                  "2015-10-01..12 (one ~850 MB gzip)",
                  "LBNL Open uPMU dataset (cite Stewart 2016)", false, 1},
                 &lbnl_upmu_fetch, &lbnl_upmu_months, 1, {{"us_western", {{"Lawrence Berkeley National Laboratory, Berkeley, California", 37.876, -122.247}}}}});
    v.push_back({{"seismic_east", {"us_eastern"},
                  "Mains hum in EarthScope accelerometer/seismometer recordings (GPS-timed), US Eastern Interconnection, 1 s, "
                  "stations fused by agreement (one 6 h request per station; ~2 GB/month)",
                  "EarthScope / FDSN open data", true, 1},  // one month at a time: 4 connections, EarthScope allows 5
                 &net_fetch<&seismic::east>, &net_months<&seismic::east>, 2, {}});
    v.back().needs_env = "GEOLOC_EARTHSCOPE_REFRESH_TOKEN";  // the anonymous quota cannot backfill
    // 2026-11-12 rescreen (docs/grids/seismic.md#us-eastern-seismic_east): new stations and scores for these years.
    v.back().month_formats = {{"2010-01", "2013-12", 3}};
    v.push_back({{"seismic_west", {"us_western"},
                  "Mains hum in EarthScope accelerometer/seismometer recordings (GPS-timed), US Western Interconnection, 1 s, "
                  "stations fused by agreement; the months AESO's measured series covers (2019-01..2025-07) are left out "
                  "(one 6 h request per station; ~2 GB/month)",
                  "EarthScope / FDSN open data", true, 1},
                 &net_fetch<&seismic::west>, &net_months<&seismic::west>, 1, {}});
    // ~2 GB (West) and 5-6 GB (ERCOT) of download per month, ~1.3 TB to backfill: fetched
    // offline and pushed to the bucket, never refreshed by the dashboard (which has no
    // GEOLOC_US_SEISMIC_REFRESH). The fetch itself still needs GEOLOC_EARTHSCOPE_REFRESH_TOKEN.
    // One variable for both keeps them on one lane: each uses 4 of EarthScope's 5 connections.
    v.back().needs_env = "GEOLOC_US_SEISMIC_REFRESH";
    v.push_back({{"seismic_ercot", {"ercot"},
                  "Mains hum in EarthScope seismometer recordings (GPS-timed) in Texas, ERCOT, 1 s, stations fused by agreement "
                  "(one 6 h request per station; ~2 GB/month)",
                  "EarthScope / FDSN open data", true, 1},
                 &net_fetch<&seismic::ercot>, &net_months<&seismic::ercot>, 1, {}});
    v.back().needs_env = "GEOLOC_US_SEISMIC_REFRESH";  // see seismic_west
    v.push_back({{"seismic_ips", {"ips_ups"},
                  "Mains hum in EarthScope strong-motion recordings (GPS-timed) in Azerbaijan, Kazakhstan and Kyrgyzstan, IPS/UPS, 1 s, "
                  "2022-10 on; only seconds where two sites agree (one 6 h request per station; ~3 GB/month)",
                  "EarthScope / FDSN open data", true, 1},
                 &net_fetch<&seismic::ips>, &net_months<&seismic::ips>, 1, {}});
    v.back().needs_env = "GEOLOC_EARTHSCOPE_REFRESH_TOKEN";
    v.push_back({{"seismic_cn_south", {"china_southern"},
                  "Mains hum in Raspberry Shake geophones in Hong Kong and Guangdong (China Southern Power Grid), 1 s, 2019-07 on: the 50 Hz "
                  "line on the Nyquist frequency decoded to a signed deviation, stations fused by agreement (~0.3-0.55 GB per station and month)",
                  "Raspberry Shake network AM, open data", true, 1},
                 &net_fetch<&seismic::cn_south>, &net_months<&seismic::cn_south>, 1, {}});
    // Decoding costs ~25 CPU-minutes per station and month: backfilled offline and pushed to
    // the bucket, never refreshed by the dashboard (which has no GEOLOC_SHAKE_REFRESH).
    v.back().needs_env = "GEOLOC_SHAKE_REFRESH";
    v.push_back({{"seismic_cn_north", {"china_north_central"},
                  "Mains hum in Raspberry Shake geophones in Beijing, Tianjin and Ordos (North & Central China grid), 1 s, stretches of "
                  "2019-05..2024: same method as seismic_cn_south",
                  "Raspberry Shake network AM, open data", true, 1},
                 &net_fetch<&seismic::cn_north>, &net_months<&seismic::cn_north>, 1, {}});
    v.back().needs_env = "GEOLOC_SHAKE_REFRESH";
    v.push_back({{"seismic_cn_east", {"china_east"},
                  "Mains hum in Raspberry Shake geophones in Xuzhou and Xiamen (East China grid), 1 s, 2023-09 on where a station hums: "
                  "same method as seismic_cn_south",
                  "Raspberry Shake network AM, open data", true, 1},
                 &net_fetch<&seismic::cn_east>, &net_months<&seismic::cn_east>, 1, {}});
    v.back().needs_env = "GEOLOC_SHAKE_REFRESH";
    v.push_back({{"seismic_cn_southwest", {"china_southwest"},
                  "Mains hum in a Raspberry Shake geophone in Chongqing (Southwest China grid), 1 s, 2021-03 on: same method as "
                  "seismic_cn_south, one station",
                  "Raspberry Shake network AM, open data", true, 1},
                 &net_fetch<&seismic::cn_southwest>, &net_months<&seismic::cn_southwest>, 1, {}});
    v.back().needs_env = "GEOLOC_SHAKE_REFRESH";
    v.push_back({{"seismic_swis", {"australia_swis"},
                  "Mains hum in Raspberry Shake geophones in and around Perth (Western Australia's South West Interconnected System), "
                  "1 s, 2022-02 on (one station in 2020): same method as seismic_cn_south",
                  "Raspberry Shake network AM, open data", true, 1},
                 &net_fetch<&seismic::swis>, &net_months<&seismic::swis>, 1, {}});
    v.back().needs_env = "GEOLOC_SHAKE_REFRESH";
    v.push_back({{"seismic_darwin", {"darwin_katherine"},
                  "Mains hum in a Raspberry Shake geophone in Darwin (Darwin-Katherine system), 1 s, spells of 2018-10..2022-07 and "
                  "2025-05 on: same method as seismic_cn_south, one station",
                  "Raspberry Shake network AM, open data", true, 1},
                 &net_fetch<&seismic::darwin>, &net_months<&seismic::darwin>, 1, {}});
    v.back().needs_env = "GEOLOC_SHAKE_REFRESH";
    v.push_back({{"seismic_perth_basin", {"australia_swis"},
                  "Mains hum in the Perth Basin Array's 250 samples/s recordings (network 5C at AusPass, GPS-timed), Western "
                  "Australia's South West Interconnected System, 1 s, 2017-01-25..2017-06; only seconds where two sites agree "
                  "(one 6 h request per station; ~1 GB per station and month)",
                  "AusPass open FDSN data (network 5C, doi:10.7914/nx28-a367)", false, 1},
                 &net_fetch<&seismic::perth_basin>, &net_months<&seismic::perth_basin>, 1, {}});
    // A closed archive, a few GB a month: backfilled offline and pushed to the bucket, never
    // refreshed by the dashboard (which has no GEOLOC_AUSPASS_REFRESH).
    v.back().needs_env = "GEOLOC_AUSPASS_REFRESH";
    v.push_back({{"seismic_estonia", {"ips_ups"},
                  "Mains hum in the Estonian national seismic network's 250 samples/s recordings (network EE at GEOFON, GPS-timed), "
                  "IPS/UPS while the Baltic states were part of it, 1 s, 2015-05..2025-02-08; only seconds where two sites agree "
                  "(one 6 h request per station; ~1 GB per station and month)",
                  "GEOFON open FDSN data (network EE, doi:10.14470/RR425351)", false, 1},
                 &net_fetch<&seismic::estonia>, &net_months<&seismic::estonia>, 1, {}});
    // A closed period, a few GB a month: backfilled offline and pushed to the bucket, never
    // refreshed by the dashboard (which has no GEOLOC_GEOFON_REFRESH).
    v.back().needs_env = "GEOLOC_GEOFON_REFRESH";
    v.push_back({{"seismic_brazil", {"brazil_sin"},
                  "Mains hum in the University of São Paulo's short-period seismic recordings (network BL, 100 samples/s, 60 Hz folded "
                  "to 40 Hz), Brazil's SIN, 1 s, 2004-02..2008-08; one recorder, clock within ~2 s of UTC (one 6 h request per channel; "
                  "~1.1 GB/month)",
                  "IAG/USP open FDSN data (network BL)", false, 1},
                 &net_fetch<&seismic::brazil>, &net_months<&seismic::brazil>, 1, {}});
    // ~1.1 GB of download per month: backfilled offline and pushed to the bucket, never
    // refreshed by the dashboard (which has no GEOLOC_BRAZIL_REFRESH). The archive is closed.
    v.back().needs_env = "GEOLOC_BRAZIL_REFRESH";
    v.push_back({{"dtu_tokyo", {"japan_east"},
                  "DTU grid frequency recordings in Tokyo (Japan East, 50 Hz), 0.5 s: 42 days of 2017, 317 days of 2020 (CC BY-NC-ND)",
                  "CC BY-NC-ND 4.0 (non-commercial)", false, 2},
                 series_fetcher("dtu_tokyo"), series_lister("dtu_tokyo"), 1, {{"japan_east", {{"Tokyo", 35.68, 139.69}}}}});
    return v;
  }();
  return t;
}

Source* source(const std::string& name) {
  for (auto& s : table())
    if (s.info.name == name) return &s;
  return nullptr;
}

int current_format(const std::string& src, const std::string& month) {
  auto* s = source(src);
  if (!s) return 1;
  int f = s->format;
  for (auto& [from, to, mf] : s->month_formats)
    if (month >= from && month <= to) f = std::max(f, mf);
  return f;
}

// ---------------------------------------------------------------- storing

// A download is authoritative for the span its files cover: from its first to its last
// second holding data, across the month and the hours it spills into its neighbours.
// That span is replaced (so data stored there by an earlier version, say with a wrong
// time offset, is gone, gaps included); outside it, what neighbouring months' files
// put there stays. Fetching months in any order gives the same store.
void store_month(const std::string& src, const std::string& grid, const std::string& m, const std::vector<float>& vals, json::object meta,
                 const std::vector<Spill>& spill) {
  auto [t0, t1] = month_bounds(m);
  i64 lo = INT64_MAX, hi = INT64_MIN;
  auto extent = [&](i64 s0, const std::vector<float>& v) {
    auto f = std::find_if(v.begin(), v.end(), [](float x) { return std::isfinite(x); });
    if (f == v.end()) return;
    auto l = std::find_if(v.rbegin(), v.rend(), [](float x) { return std::isfinite(x); });
    lo = std::min(lo, s0 + i64(f - v.begin()));
    hi = std::max(hi, s0 + i64(v.rend() - l));  // exclusive
  };
  extent(t0, vals);
  for (auto& sp : spill) extent(sp.start, sp.values);
  std::set<std::string> touched;
  if (lo < hi) {
    std::vector<float> span(std::size_t(hi - lo), NAN);
    auto put = [&](i64 s0, const std::vector<float>& v) {
      for (std::size_t i = 0; i < v.size(); ++i) {
        const i64 t = s0 + i64(i);
        if (t >= lo && t < hi && std::isfinite(v[i])) span[std::size_t(t - lo)] = v[i];
      }
    };
    put(t0, vals);
    for (auto& sp : spill) put(sp.start, sp.values);
    store::replace_range(grid, src, lo, span);
    for (auto& nm : months_between(month_key(lo), month_key(hi - 1)))
      if (nm != m) touched.insert(nm);
  }
  meta["format"] = current_format(src, m);
  store::write_entry(grid, src, m, meta);
  for (auto& nm : touched) store::refresh_coverage(grid, src, nm);  // neighbours with a manifest entry
  char b[64];
  std::snprintf(b, sizeof b, "%.1f%%", 100.0 * jnum(*store::month_info(grid, src, m), "coverage"));
  log("[" + src + "] " + grid + " " + m + ": stored, coverage " + b + (meta.contains("retry_chunks")  ? " (partial: " + std::to_string(meta.at("retry_chunks").as_array().size()) + " chunk(s) failed, retried next fetch)"
                                    : jbool(meta, "partial") ? " (partial: current month, will refresh)"
                                                             : ""));
}

bool placed(const std::string& src, const std::string& grid, const std::string& m, const json::object& meta) {
  auto issues = placement_issues(src, grid, m, &meta);
  bool bad = false;
  for (auto& i : issues) {
    std::string sev = i.severity;
    for (auto& ch : sev) ch = static_cast<char>(std::toupper(ch));
    log("[" + src + "] " + grid + " " + m + ": placement " + sev + ": " + i.message);
    bad |= i.severity == "misplaced";
  }
  if (bad) log("[" + src + "] " + grid + " " + m + ": NOT stored (recorder outside the grid)");
  return !bad;
}

}  // namespace
// ---------------------------------------------------------------- public API

std::vector<SourceInfo> sources() {
  std::vector<SourceInfo> v;
  for (auto& s : table()) v.push_back(s.info);
  return v;
}
namespace testing {
void add_fake_source(SourceInfo info, std::function<FakeMonth(const std::string&, const std::string&)> fetch,
                     std::function<std::vector<std::string>(const std::string&)> months, int format,
                     std::vector<std::tuple<std::string, std::string, int>> month_formats) {
  Source s;
  s.info = std::move(info);
  s.format = format;
  s.month_formats = std::move(month_formats);
  s.fetch = [fetch](Ctx&, const std::string& grid, const std::string& month) -> asio::awaitable<MonthData> {
    FakeMonth f = fetch(grid, month);
    MonthData md;
    md.values = std::move(f.values);
    md.meta = std::move(f.meta);
    for (auto& [s0, v] : f.spill) md.spill.push_back({s0, std::move(v)});
    for (auto& [g, v, m] : f.also) md.also.push_back({g, std::move(v), std::move(m), {}});
    co_return md;
  };
  s.months = [months](Ctx&, const std::string& grid) -> asio::awaitable<std::vector<std::string>> { co_return months(grid); };
  s.fake = true;
  table().push_back(std::move(s));
}
void remove_fake_sources() {
  std::erase_if(table(), [](auto& s) { return s.fake; });
}
}  // namespace testing

const SourceInfo* find_source(const std::string& name) {
  auto* s = source(name);
  return s ? &s->info : nullptr;
}

std::string refresh_lane(const std::string& grid) {
  // Grids joined by joint sources, transitively; the lane is the smallest id among them.
  // Sources drawing on one account (needs_env) join their grids too: the seismic sources
  // share EarthScope's limit of 5 connections per client, and each uses 4.
  std::set<std::string> lane{grid};
  for (bool grew = true; grew;) {
    grew = false;
    for (auto& s : table()) {
      if (!std::any_of(s.info.grids.begin(), s.info.grids.end(), [&](auto& g) { return lane.contains(g); })) continue;
      if (s.info.joint)
        for (auto& g : s.info.grids) grew |= lane.insert(g).second;
      if (s.needs_env)
        for (auto& o : table())
          if (o.needs_env && std::string_view(o.needs_env) == s.needs_env)
            for (auto& g : o.info.grids) grew |= lane.insert(g).second;
    }
  }
  return *lane.begin();
}

std::vector<std::string> sources_covering(const std::string& grid) {
  std::vector<std::string> out;
  for (auto& s : table())
    if (std::find(s.info.grids.begin(), s.info.grids.end(), grid) != s.info.grids.end()) out.push_back(s.info.name);
  return out;
}

std::vector<Location> recorder_locations(const std::string& src, const std::string& grid, const json::object* info) {
  std::vector<std::string> names;
  if (info)
    if (auto* f = info->if_contains("files"); f && f->is_array())
      for (auto& x : f->get_array()) {
        if (src == "osf_edr" && jget(x, "site")) names.push_back(jstr(x, "site"));
        if (src == "osf_pgf" && OSF_LOCATIONS.count(jstr(x, "country"))) names.push_back(jstr(x, "country"));
      }
  std::vector<std::string> uniq;
  for (auto& n : names)
    if (std::find(uniq.begin(), uniq.end(), n) == uniq.end()) uniq.push_back(n);
  std::vector<Location> out;
  if (src == "osf_edr") return edr_locations(grid, std::move(uniq));
  if (const seismic::Network* net = seismic::network(src)) {  // the stations each month used, from its manifest entry
    if (info)
      if (auto* f = info->if_contains("files"); f && f->is_array())
        for (auto& x : f->get_array())
          if (auto* o = x.if_object(); o && o->contains("lat"))
            out.push_back({jstr(x, "channel"), o->at("lat").to_number<double>(), o->at("lon").to_number<double>()});
    if (out.empty())
      for (auto& st : net->stations())
        out.push_back({st.id, std::isfinite(st.audit_lat) ? st.audit_lat : st.lat, std::isfinite(st.audit_lon) ? st.audit_lon : st.lon});
    return out;
  }
  if (src == "osf_pgf" && !uniq.empty()) {
    for (auto& n : uniq) out.push_back(OSF_LOCATIONS.at(n));
    return out;
  }
  if (auto* s = source(src))
    if (auto it = s->locations.find(grid); it != s->locations.end()) return it->second;
  return out;
}

std::vector<PlacementIssue> placement_issues(const std::string& src, const std::string& grid, const std::string& month,
                                             const json::object* info) {
  auto locs = recorder_locations(src, grid, info);
  if (locs.empty()) return {{grid, src, month, "?", "unknown", "no recorder location known for " + src + " / " + grid + ": placement not checked"}};
  auto [t0, t1] = month_bounds(month);
  std::vector<std::string> days;
  for (i64 t = t0; t < t1; t += 86400) days.push_back(iso_date(t));
  std::vector<PlacementIssue> out;
  char b[512];
  for (auto& l : locs) {
    auto where = grids::grids_at_days(l.lat, l.lon, days);
    std::size_t inside = 0;
    std::set<std::string> others;
    for (auto& w : where) {
      bool in = false;
      for (auto& p : w) {
        if (p.grid == grid) in = true;
        else if (p.grid) others.insert(*p.grid);
      }
      inside += in;
    }
    if (inside == days.size()) continue;
    std::vector<std::string> ov(others.begin(), others.end());
    if (ov.empty()) ov.push_back("no known grid");
    if (inside == 0) {
      std::snprintf(b, sizeof b, "%s (%.2f, %.2f) lies in %s, not %s, throughout %s", l.label.c_str(), l.lat, l.lon, join(ov, ", ").c_str(),
                    grid.c_str(), month.c_str());
      out.push_back({grid, src, month, l.label, "misplaced", b});
    } else {
      std::snprintf(b, sizeof b, "%s (%.2f, %.2f) is in %s on %zu/%zu days of %s; otherwise in %s", l.label.c_str(), l.lat, l.lon, grid.c_str(),
                    inside, days.size(), month.c_str(), join(ov, ", ").c_str());
      out.push_back({grid, src, month, l.label, "partial", b});
    }
  }
  return out;
}

bool is_cached(const std::string& src, const std::string& grid, const std::string& month) {
  auto info = store::month_info(grid, src, month);
  if (!info || jbool(*info, "partial")) return false;
  return jint(*info, "format", 1) >= current_format(src, month);
}

std::vector<std::string> stale_months(const std::string& grid, const std::string& src) {
  std::vector<std::string> out;
  auto man = store::read_manifest(grid, src);
  if (auto* m = man.if_contains("months"); m && m->is_object())
    for (auto& kv : m->get_object())
      if (!is_cached(src, grid, std::string(kv.key()))) out.emplace_back(kv.key());
  std::sort(out.begin(), out.end());
  return out;
}

std::vector<std::pair<std::string, std::vector<std::string>>> refresh_plan(const std::string& grid, std::optional<double> now,
                                                                           std::vector<std::string>* problems) {
  return net::run_sync<std::vector<std::pair<std::string, std::vector<std::string>>>>(
      [&](asio::io_context& io) -> asio::awaitable<std::vector<std::pair<std::string, std::vector<std::string>>>> {
        Ctx c(io);
        const std::string cur = now ? month_key(static_cast<i64>(*now)) : now_month();
        Month cm = Month::parse(cur).value();
        Month recent = cm.prev().prev();  // the last three months
        std::vector<std::pair<std::string, std::vector<std::string>>> plan;
        for (auto& s : table()) {
          if (std::find(s.info.grids.begin(), s.info.grids.end(), grid) == s.info.grids.end()) continue;
          if (s.needs_env && env(s.needs_env).empty()) continue;
          auto man = store::read_manifest(grid, s.info.name);
          std::vector<std::string> available;
          try {
            for (auto& m : co_await s.months(c, grid))
              if (m <= cur) available.push_back(m);
          } catch (const std::exception& e) {  // index page down: still refresh what is stored
            log("[" + s.info.name + "] " + grid + ": cannot list months (" + e.what() + ")");
            if (problems) problems->push_back(s.info.name + ": cannot list months (" + e.what() + ")");
          }
          const json::object* gone = nullptr;
          if (auto* u = man.if_contains("unavailable"); u && u->is_object()) gone = &u->get_object();
          std::set<std::string> months;
          for (auto& m : available)
            if (!is_cached(s.info.name, grid, m) &&
                (!gone || !gone->contains(m) || !unavailable_stands(gone->at(m)) ||
                 jint(gone->at(m), "format", 1) < current_format(s.info.name, m) || (s.info.live && m >= recent.key())))
              months.insert(m);
          for (auto& m : stale_months(grid, s.info.name)) months.insert(m);
          if (!months.empty()) plan.emplace_back(s.info.name, std::vector<std::string>(months.begin(), months.end()));
        }
        co_return plan;
      });
}

std::vector<std::string> fetch(const std::string& src_name, const std::string& grid, const std::vector<std::string>& months, bool force,
                               std::map<std::string, std::string>* failed) {
  const Source* src = source(src_name);
  if (!src) {
    std::vector<std::string> names;
    for (auto& s : table()) names.push_back(s.info.name);
    throw std::out_of_range("unknown source '" + src_name + "'; known: " + join(names, ", "));
  }
  if (std::find(src->info.grids.begin(), src->info.grids.end(), grid) == src->info.grids.end())
    throw std::out_of_range("source '" + src_name + "' covers " + join(src->info.grids, ", ") + ", not '" + grid + "'");
  std::vector<std::string> done, todo;
  for (auto& m : months) {
    if (!force && is_cached(src_name, grid, m)) {
      log("[" + src_name + "] " + grid + " " + m + ": cached");
      done.push_back(m);
    } else {
      todo.push_back(m);
    }
  }
  const int n = src->info.workers;
  net::run_sync_void([&](asio::io_context& io) -> asio::awaitable<void> {
    Ctx c(io);
    co_await net::parallel_for(todo, n, [&](const std::string& m) -> asio::awaitable<void> {
      // A partial month (the current one of a live source) keeps what it holds; the
      // live fetchers download only the rest. Joint sources fill all their grids. Not
      // when it was stored in an older format: then all of it is downloaded again.
      if (!force)
        for (auto& g : src->info.joint ? src->info.grids : std::vector<std::string>{grid})
          if (auto info = store::month_info(g, src_name, m);
              info && jbool(*info, "partial") && jint(*info, "format", 1) >= current_format(src_name, m)) {
            auto [t0, t1] = month_bounds(m);
            c.have[{g, m}] = store::read(g, t0, t1, {src_name});
          }
      try {
        MonthData md = co_await src->fetch(c, grid, m);
        for (auto& a : md.also)  // one download, several grids
          if (placed(src_name, a.grid, m, a.meta)) store_month(src_name, a.grid, m, a.values, a.meta, a.spill);
        if (placed(src_name, grid, m, md.meta)) {
          store_month(src_name, grid, m, md.values, md.meta, md.spill);
          done.push_back(m);
        }
      } catch (const Unavailable& e) {
        log("[" + src_name + "] " + grid + " " + m + ": unavailable (" + e.what() + ")");
        store::mark_unavailable(grid, src_name, m, e.what(), current_format(src_name, m));
      } catch (const std::exception& e) {  // one bad month must not lose the others; retried next time
        log("[" + src_name + "] " + grid + " " + m + ": FAILED (" + e.what() + ")");
        if (failed) (*failed)[m] = e.what();
      }
      for (auto& g : src->info.grids) c.have.erase({g, m});
    });
  });
  std::sort(done.begin(), done.end());
  return done;
}

std::optional<std::pair<int, double>> estimate_lag(const std::vector<float>& primary, const std::vector<float>& secondary, int max_lag,
                                                   std::size_t min_overlap) {
  const std::size_t n = std::min(primary.size(), secondary.size());
  std::size_t cnt = 0;
  double sa = 0, sb = 0;
  for (std::size_t i = 0; i < n; ++i)
    if (std::isfinite(primary[i]) && std::isfinite(secondary[i])) {
      ++cnt;
      sa += primary[i];
      sb += secondary[i];
    }
  if (cnt < min_overlap) return std::nullopt;
  const double ma = sa / double(cnt), mb = sb / double(cnt);
  std::vector<double> a(n, 0.0), b(n, 0.0);
  double aa = 0, bb = 0;
  for (std::size_t i = 0; i < n; ++i)
    if (std::isfinite(primary[i]) && std::isfinite(secondary[i])) {
      a[i] = primary[i] - ma;
      b[i] = secondary[i] - mb;
      aa += a[i] * a[i];
      bb += b[i] * b[i];
    }
  // Exact cross-correlation at the 2 max_lag + 1 lags of interest (primary[t] ~ secondary[t - lag]).
  int best = 0;
  double best_v = -INFINITY;
  for (int lag = -max_lag; lag <= max_lag; ++lag) {
    double s = 0;
    const std::ptrdiff_t lo = std::max<std::ptrdiff_t>(0, -lag), hi = std::min<std::ptrdiff_t>(std::ptrdiff_t(n), std::ptrdiff_t(n) - lag);
    for (std::ptrdiff_t t = lo; t < hi; ++t) s += a[std::size_t(t + lag)] * b[std::size_t(t)];
    if (s > best_v) {
      best_v = s;
      best = lag;
    }
  }
  return std::pair{best, best_v / std::sqrt(aa * bb)};
}

}  // namespace geo::refs
