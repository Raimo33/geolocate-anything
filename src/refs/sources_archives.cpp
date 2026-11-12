// One-off releases and measurement campaigns: KIT's EDR campaigns and Zenodo release,
// plain time/frequency CSV releases (AESO, Grid-India, ...), ERCOT's ETTS archive and
// LBNL's micro-PMU dataset.
#include "refs/source_impl.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <stdexcept>

#include "net/zip.hpp"
#include "refs/sheets.hpp"
#include "store/store.hpp"
#include "util/text.hpp"

namespace geo::refs::detail {

// ---------------------------------------------------------------- KIT EDR campaigns (OSF by5hu)

namespace {

struct EdrDef {
  const char *site, *grid, *url, *m0, *m1;
  // Files without a time column: the UTC start of row 1 (KIT's metadata), 1 s per row,
  // values in Hz around this nominal.
  const char* implied_start = nullptr;
  double nominal_hz = 0;
};
const EdrDef EDR_SITES[] = {
    {"US_CO01", "us_western", "https://osf.io/download/kbxh3/", "2024-01", "2024-11"},
    {"US_UT01", "us_western", "https://osf.io/download/8rp4v/", "2019-05", "2019-05"},
    {"US_FL01", "us_eastern", "https://osf.io/download/w8x9b/", "2023-06", "2023-07"},
    {"US_TX01", "ercot", "https://osf.io/download/t5wxz/", "2019-05", "2019-05"},
    {"US_TX02", "ercot", "https://osf.io/download/zngy8/", "2019-05", "2019-05"},
    {"RU01", "ips_ups", "https://osf.io/download/tvsyc/", "2019-04", "2019-05"},
    {"EE01", "ips_ups", "https://osf.io/download/t5ske/", "2019-03", "2019-04"},
    {"ES_PM01", "balearic", "https://osf.io/download/2qn9k/", "2019-09", "2019-12"},
    {"ES_PM02", "balearic", "https://osf.io/download/vrc6a/", "2020-01", "2020-12"},
    {"ES_PM03", "balearic", "https://osf.io/download/7xe5r/", "2021-01", "2021-03"},
    {"ES_GC01", "gran_canaria", "https://osf.io/download/wz42b/", "2018-02", "2018-02"},
    {"ES_GC02", "gran_canaria", "https://osf.io/download/rukat/", "2018-11", "2018-11"},
    {"FO01", "faroe", "https://osf.io/download/a7h5b/", "2019-11", "2019-11"},
    {"IS01", "iceland", "https://osf.io/download/sxph8/", "2017-10", "2017-10"},
    {"ZA01", "southern_africa", "https://osf.io/download/gzk7d/", "2017-11", "2017-11"},
    {"ZA02", "southern_africa", "https://osf.io/download/xtgqh/", "2025-05", "2025-08"},
    {"KR01", "south_korea", "https://osf.io/download/6qazn/", "2024-08", "2024-12"},
    {"MY01", "malaysia_peninsular", "https://osf.io/download/msc5k/", "2022-12", "2022-12"},
    // The 2026-08-12 solar eclipse over Spain: three days, rows only ("freq,QI" in Hz).
    {"ES_PM_solar_eclipse_2026", "balearic", "https://osf.io/download/n6s32/", "2026-08", "2026-08", "2026-08-11 00:00:00", 50.0},
};
// The recorders' towns, as KIT's database page lists them
// (power-grid-frequency.org/assets/files/frequency_measurements_statistics.json). Florida
// names no town: its point is Orlando, marked "assumed".
const std::map<std::string, Location> EDR_LOCATIONS = {
    {"US_CO01", {"Boulder, Colorado", 40.01, -105.27}},
    {"US_UT01", {"Salt Lake City, Utah", 40.76, -111.89}},
    {"US_FL01", {"Florida (town not published; Orlando assumed)", 28.54, -81.38}},
    {"US_TX01", {"College Station, Texas", 30.63, -96.33}},
    {"US_TX02", {"College Station, Texas", 30.63, -96.33}},
    {"RU01", {"St. Petersburg, Russia", 59.94, 30.31}},
    {"EE01", {"Tallinn, Estonia", 59.44, 24.75}},
    {"ES_PM01", {"Palma de Mallorca", 39.57, 2.65}},
    {"ES_PM02", {"Palma de Mallorca", 39.57, 2.65}},
    {"ES_PM03", {"Palma de Mallorca", 39.57, 2.65}},
    {"ES_PM_solar_eclipse_2026", {"Palma de Mallorca", 39.57, 2.65}},
    {"ES_GC01", {"Las Palmas de Gran Canaria", 28.12, -15.43}},
    {"ES_GC02", {"Las Palmas de Gran Canaria", 28.12, -15.43}},
    {"FO01", {"Vestmanna, Faroe Islands", 62.16, -7.17}},
    {"IS01", {"Reykjavík, Iceland", 64.15, -21.94}},
    {"ZA01", {"Cape Town, South Africa", -33.92, 18.42}},
    {"ZA02", {"Stellenbosch, South Africa", -33.93, 18.86}},
    {"KR01", {"Naju, South Korea", 35.03, 126.71}},
    {"MY01", {"Kuala Lumpur, Malaysia", 3.14, 101.69}},
};

// Recorders whose clock was not on UTC, as the GPS-timed Estonian seismometers show
// (seismic_estonia; docs/grids/seismic_estonia.md). KIT's files put every recorder on one
// time axis, and the Karlsruhe column beside Tallinn's follows Continental Europe's
// measured series at lag 0, but Tallinn's own column (2019-03-25..04-17) runs late by an
// offset that grows by 1.2-2.3 s a day, from 19 s on 2019-03-29 to 55 s on 04-17. The
// knots are its lag behind the seismic reconstruction in 1-6 h windows (r 0.97-0.99),
// less the 1 s by which that reconstruction leads KIT's recorders whose clocks held
// (St. Petersburg, RU01, 2019-04-30..05-11: +1 s throughout; Kaunas and Tallinn in 2025,
// osf_baltic: +0.9 s), so Tallinn lands on KIT's own time base. Before 03-29 23:00 the
// lag is not determined (too few humming seconds; a broad peak at 33 s on 03-26 would
// mean a reset), so those four days are dropped.
using Knot = EdrParser::ClockKnot;
std::vector<Knot> edr_clock(const std::string& site) {
  if (site != "EE01") return {};
  auto at = [](const char* iso) { return static_cast<i64>(parse_iso_datetime(iso)->utc()); };
  return {{at("2019-03-29 23:00:00"), 19.0}, {at("2019-04-02 02:00:00"), 21.0}, {at("2019-04-03 19:30:00"), 25.0},
          {at("2019-04-04 19:00:00"), 28.0}, {at("2019-04-05 21:30:00"), 30.0}, {at("2019-04-06 20:30:00"), 32.0},
          {at("2019-04-08 03:00:00"), 35.0}, {at("2019-04-11 22:30:00"), 42.0}, {at("2019-04-12 21:00:00"), 43.5},
          {at("2019-04-13 22:00:00"), 47.5}, {at("2019-04-15 03:00:00"), 50.0}, {at("2019-04-15 21:30:00"), 50.0},
          {at("2019-04-16 12:00:00"), 53.5}, {at("2019-04-17 00:30:00"), 55.0}};
}

const EdrDef& edr_def(const std::string& site) {
  for (auto& d : EDR_SITES)
    if (site == d.site) return d;
  throw std::out_of_range("unknown EDR site " + site);
}

// A campaign CSV (up to 1.1 GB) is streamed once per process.
Loader edr_loader(const EdrDef& d) {
  return [&d](Ctx& c, SharedFile& s) -> asio::awaitable<void> {
    log(std::string("  downloading EDR campaign ") + d.site + " (" + d.m0 + ".." + d.m1 + ") from OSF ...");
    std::optional<EdrParser::Implied> implied;
    if (d.implied_start) implied = EdrParser::Implied{static_cast<i64>(parse_iso_datetime(d.implied_start)->utc()), d.nominal_hz};
    EdrParser p(months_between(d.m0, d.m1), implied, edr_clock(d.site));
    co_await stream_url(c, d.url, [&](std::string_view b) { p.feed(b); });
    s.months = p.finish();
    s.rows = p.rows;
    s.kept = p.kept;
  };
}

}  // namespace

asio::awaitable<MonthData> edr_fetch(Ctx& c, const std::string& grid, const std::string& month) {
  std::vector<std::string> sites, spans;
  for (auto& d : EDR_SITES)
    if (grid == d.grid) {
      spans.push_back(std::string(d.site) + " " + d.m0 + ".." + d.m1);
      if (d.m0 <= month && month <= d.m1) sites.emplace_back(d.site);
    }
  if (sites.empty())
    throw Unavailable("no EDR campaign for " + grid + " in " + month + " (campaigns: " + (spans.empty() ? std::string("none") : join(spans, ", ")) + ")");
  auto [t0, t1] = month_bounds(month);
  std::vector<float> merged(std::size_t(t1 - t0), NAN);
  std::vector<json::object> used;
  for (auto& site : sites) {
    const auto& d = edr_def(site);
    auto load = edr_loader(d);
    auto v = co_await take_month(c, "edr:" + site, month, load);
    auto s = c.files.contains("edr:" + site) ? c.files["edr:" + site] : nullptr;
    std::int64_t filled = v ? fill_gaps(merged, *v) : 0;
    used.push_back({{"site", site}, {"url", d.url}, {"filled_s", filled}, {"rows", s ? s->rows : 0}, {"rows_kept_qi0", s ? s->kept : 0}});
    if (!edr_clock(site).empty())
      used.back()["clock"] = "corrected: the recorder ran 19-55 s late, measured against seismic_estonia; rows before 2019-03-29 23:00 dropped";
  }
  if (!any_finite(merged))
    throw Unavailable("EDR campaigns " + join(sites, ", ") + " hold no valid data in " + month);
  MonthData md;
  md.values = std::move(merged);
  md.meta = {{"source", "osf_edr"},
             {"project", "https://osf.io/by5hu/"},
             {"licence", "CC BY 4.0 (Power grid frequency data base, KIT)"},
             {"native_resolution_s", 1.0},
             {"timestamps", "UTC (GPS-disciplined recorder)"},
             {"files", to_json_array(used)}};
  co_return md;
}

asio::awaitable<std::vector<std::string>> edr_months(Ctx&, const std::string& grid) {
  std::set<std::string> out;
  for (auto& d : EDR_SITES)
    if (grid == d.grid)
      for (auto& m : months_between(d.m0, d.m1)) out.insert(m);
  co_return std::vector<std::string>(out.begin(), out.end());
}

std::vector<std::string> edr_grids() {
  std::vector<std::string> g;
  for (auto& d : EDR_SITES)
    if (std::find(g.begin(), g.end(), d.grid) == g.end()) g.emplace_back(d.grid);
  return g;
}

std::vector<Location> edr_locations(const std::string& grid, std::vector<std::string> sites) {
  if (sites.empty())
    for (auto& d : EDR_SITES)
      if (grid == d.grid && std::find(sites.begin(), sites.end(), d.site) == sites.end()) sites.emplace_back(d.site);
  std::vector<Location> out;
  for (auto& s : sites) out.push_back(EDR_LOCATIONS.at(s));
  return out;
}

// ---------------------------------------------------------------- KIT/Jülich Zenodo release (Nat. Energy 2026)

namespace {

// "Grid Frequency Data.zip" of Zenodo record 21997337: 1 s CSVs, mHz, UTC, stored
// uncompressed in an 8.7 GB zip, so one file is streamed out of it with a range request.
// Only what OSF by5hu lacks is listed: Mallorca after 2021-03 (its 2019-10..2021-03 is
// the same recorder as osf_edr's ES_PM campaigns), Iceland 2021-22 and Ireland 2021-12.
// The Mallorca/Ireland files keep only complete hours (no QI column); the others carry QI.
constexpr const char* ZK_URL = "https://zenodo.org/records/21997337/files/Grid%20Frequency%20Data.zip?download=1";
struct ZkFile {
  const char *grid, *path, *m0, *m1;
};
const ZkFile ZK_FILES[] = {
    {"balearic", "Grid Frequency Data/Mallorca/mallorca_full_hours_2021.csv", "2021-01", "2021-12"},
    {"balearic", "Grid Frequency Data/Mallorca/mallorca_full_hours_2022.csv", "2022-01", "2022-12"},
    {"balearic", "Grid Frequency Data/Mallorca/mallorca_full_hours_2023.csv", "2023-01", "2023-12"},
    {"balearic", "Grid Frequency Data/Mallorca/mallorca_full_hours_2024.csv", "2024-01", "2024-12"},
    {"balearic", "Grid Frequency Data/Mallorca/mallorca_full_hours_2025.csv", "2025-01", "2025-11"},
    {"iceland", "Grid Frequency Data/Iceland/iceland.csv", "2021-11", "2022-01"},
    {"ireland", "Grid Frequency Data/Ireland/ireland_2021_december.csv", "2021-12", "2021-12"},
};

// One CSV streamed out of the remote zip with a range request.
Loader zk_loader(const ZkFile& d) {
  return [&d](Ctx& c, SharedFile& s) -> asio::awaitable<void> {
    zip::Remote z(c.http, ZK_URL);
    co_await z.open();
    auto e = std::find_if(z.entries().begin(), z.entries().end(), [&](auto& e) { return e.name == d.path; });
    if (e == z.entries().end()) throw std::runtime_error(std::string("Zenodo archive has no ") + d.path);
    auto [a, b] = co_await z.stored_span(*e);
    log(std::string("  reading ") + d.path + " (" + d.m0 + ".." + d.m1 + ", " + std::to_string((b - a) >> 20) + " MiB) from Zenodo ...");
    EdrParser p(months_between(d.m0, d.m1));
    co_await stream_url(c, z.url(), [&](std::string_view chunk) { p.feed(chunk); }, std::pair{a, b - 1});
    s.months = p.finish();
    s.rows = p.rows;
    s.kept = p.kept;
  };
}

}  // namespace

const std::map<std::string, std::vector<Location>> ZK_LOCATIONS = {
    {"balearic", {{"Palma de Mallorca", 39.57, 2.65}}},
    {"iceland", {{"Iceland (city not published; Reykjavík assumed)", 64.15, -21.94}}},
    {"ireland", {{"Ireland (city not published; Dublin assumed)", 53.35, -6.26}}},
};

asio::awaitable<MonthData> zk_fetch(Ctx& c, const std::string& grid, const std::string& month) {
  const ZkFile* d = nullptr;
  for (auto& f : ZK_FILES)
    if (grid == f.grid && f.m0 <= month && month <= f.m1) d = &f;
  if (!d) throw Unavailable("the KIT Zenodo release has no " + grid + " data for " + month);
  const std::string key = std::string("zenodo:") + d->path;
  auto load = zk_loader(*d);
  auto v = co_await take_month(c, key, month, load);
  if (!v) throw Unavailable(std::string(d->path) + " has no rows in " + month);
  auto s = c.files[key];
  MonthData md;
  md.values = std::move(*v);
  if (!any_finite(md.values))
    throw Unavailable(std::string(d->path) + " holds no valid data in " + month);
  md.meta = {{"source", "kit_zenodo"},
             {"record", "https://doi.org/10.5281/zenodo.21997337"},
             {"file", d->path},
             {"licence", "CC BY 4.0 (Hartmann et al., Nat. Energy 2026, doi:10.1038/s41560-026-02144-0)"},
             {"native_resolution_s", 1.0},
             {"timestamps", "UTC (GPS-disciplined recorder)"},
             {"rows", s->rows},
             {"rows_kept", s->kept}};
  co_return md;
}

asio::awaitable<std::vector<std::string>> zk_months(Ctx&, const std::string& grid) {
  std::set<std::string> out;
  for (auto& f : ZK_FILES)
    if (grid == f.grid)
      for (auto& m : months_between(f.m0, f.m1)) out.insert(m);
  co_return std::vector<std::string>(out.begin(), out.end());
}

// ---------------------------------------------------------------- one-off releases of plain time/frequency CSVs

namespace {

// A file of a release: the months it covers and how to read it. Several files of one
// source may cover a month (the Grid-India files overlap by days): they fill each
// other's gaps in the order listed.
struct SeriesFile {
  std::string grid, url, m0, m1;
  std::string note;  // what the file is, for the log and manifest
};
struct SeriesRelease {
  const char* source;
  std::vector<SeriesFile> files;
  SeriesFormat format;
  json::object meta;  // licence, resolution, timestamps, location ...
};

// AESO (Alberta, part of the Western Interconnection): yearly zips of 2 s averages,
// released once on request ("no plans to provide further updates").
const SeriesRelease AESO = {
    "aeso",
    [] {
      std::vector<SeriesFile> v;
      for (int y = 2019; y <= 2025; ++y) {
        const std::string ys = std::to_string(y);
        v.push_back({"us_western", "https://www.aeso.ca/assets/Uploads/data-requests/AESO-System-2-Second-Frequency-" + ys + ".zip", ys + "-01",
                     y == 2025 ? "2025-07" : ys + "-12", "yearly zip " + ys});
      }
      return v;
    }(),
    // 2019..2023 head the value column "Frequency", 2024..2025 "Avg" (with Max and Min).
    {{"timestamp (utc)"}, {"avg", "frequency"}, {}, 60.0, 0, "", -3},
    {{"licence", "AESO data request publication (aeso.ca, no licence stated)"},
     {"native_resolution_s", 2.0},
     {"timestamps", "'Timestamp (UTC)' column, then -3 s (measured against KIT's GPS-timed Colorado recorder, 2024-04, r = 0.993)"},
     {"measured_at", "Alberta Interconnected Electric System (AESO system frequency)"}}};

// Grid-India PMU at Balipara 400 kV (NERLDC, Assam), released on Mendeley Data by IIT
// Guwahati. Five exports of the PDC, three layouts; three were cut short by the export
// limit (a Java stack trace ends them). Times are IST (the hourly schedule changes show
// at minute 0 of the stamped hour).
const SeriesRelease GRID_INDIA = {
    "grid_india_pmu",
    {{"india", "https://data.mendeley.com/public-files/datasets/bvwh9d6227/files/199247fd-d6a3-49ba-bc0a-ac2744e592fd/file_downloaded",
      "2020-12", "2021-08", "Frequency Data I.csv (2020-12-20..2021-08-08)"},
     {"india", "https://data.mendeley.com/public-files/datasets/bvwh9d6227/files/f0fa6671-eed9-43b7-85a5-c0446c13b3ad/file_downloaded",
      "2021-08", "2022-01", "Frequency Data II.csv (2021-08-01..2022-01-27)"},
     {"india", "https://data.mendeley.com/public-files/datasets/bvwh9d6227/files/01ad3056-5fab-4d28-b85f-d72771c39f25/file_downloaded",
      "2022-01", "2022-09", "Frequency Data III.csv (2022-01-19..2022-09-07)"},
     {"india", "https://data.mendeley.com/public-files/datasets/bvwh9d6227/files/e83b2fe3-fb0c-4784-a82f-b025d53dc52c/file_downloaded",
      "2022-09", "2023-02", "Frequency Data IV.csv (2022-09-01..2023-02-05)"},
     {"india", "https://data.mendeley.com/public-files/datasets/bvwh9d6227/files/002c997a-84d7-4356-9d4d-d8722869ac14/file_downloaded",
      "2023-01", "2023-08", "Frequency Data V.csv (2023-01-01..2023-08-20)"}},
    {{"startdate"},
     {"measurementdata1hz", "complexreportgroup:1hz"},
     {"measurementdata1hz:quality", "complexreportgroup:1hz:quality"},
     50.0,
     19800,
     "",
     0},
    {{"licence", "CC BY 4.0 (Mukherjee, Kalita, Kumar, IIT Guwahati; Mendeley Data, doi:10.17632/bvwh9d6227.4)"},
     {"native_resolution_s", 1.0},
     {"timestamps", "IST (UTC+5:30), converted to UTC; inferred from the hourly schedule-change steps at minute 0"},
     {"measured_at", "Balipara 400 kV substation (BALIP_PG), Assam, India"}}};

// Two PMUs of Lonardi, Schäfer & Beck (QMUL/KIT), Zenodo 21461024, 1 s, both UTC.
// Stellenbosch: lag 0 against KIT's ZA02 recorder, r = 0.999. London: its stamps run on
// without a jump or repeat through both 2025 clock changes and 1 November, and its hour
// 2025-10-31 23:00 UTC is missing from NESO's files (r < 0.1 against every NESO hour
// near it): NESO's plain-time files are the ones off by the UK offset (NesoParser), not
// London. Format 2: London was read as "UTC minus the UK offset" before (an hour early
// all summer).
const SeriesRelease PMU_ZENODO = {
    "pmu_zenodo",
    {{"southern_africa", "https://zenodo.org/records/21461024/files/frequency_stellenbosch.csv?download=1", "2025-04", "2025-12",
      "frequency_stellenbosch.csv"},
     {"great_britain", "https://zenodo.org/records/21461024/files/frequency_london.csv?download=1", "2025-04", "2025-12",
      "frequency_london.csv"}},
    {{"time_stamp"}, {"frequency"}, {}, 50.0, 0, "", 0},
    {{"licence", "CC BY 4.0 (Lonardi, Schäfer, Beck; Zenodo 21461024)"},
     {"record", "https://doi.org/10.5281/zenodo.21461024"},
     {"native_resolution_s", 1.0},
     {"timestamps", "UTC (Stellenbosch against KIT's GPS-timed ZA02 recorder: lag 0 s, r = 0.998; London against NESO, its UK "
                    "offset removed: lag 0 s)"}}};

// DTU (Technical University of Denmark), Tokyo, 50 Hz: 2017 (42 days, 3 samples/s, 7z)
// and 2020 (317 days, 2 samples/s, zip). UNIX milliseconds, 10 mHz accuracy (DEIF MTR-3).
const SeriesRelease DTU_TOKYO = {
    "dtu_tokyo",
    {{"japan_east", "https://ndownloader.figshare.com/files/25264913", "2017-01", "2017-12", "freq_Japan_2017.7z (42 days)"},
     {"japan_east", "https://ndownloader.figshare.com/files/26485760", "2020-01", "2020-12", "Tokyo_2020.csv.zip (317 days)"}},
    {{"timestamp", "time", "unix", "t"}, {"frequency", "freq", "f"}, {}, 50.0, 0, "", 0},
    {{"licence", "CC BY-NC-ND 4.0 (DTU data.dtu.dk, doi:10.11583/DTU.14038910 and doi:10.11583/DTU.13142858): non-commercial use"},
     {"native_resolution_s", 0.5},
     {"timestamps", "UNIX milliseconds (UTC)"},
     {"accuracy", "10 mHz (DEIF MTR-3)"},
     {"measured_at", "Tokyo, Japan"}}};

// KIT's GPS-synchronised recorders in Kaunas and Tallinn around the Baltic states'
// move from IPS/UPS to Continental Europe (OSF m43tg, "Baltic synchronization"), 0.1 s,
// mHz deviations, 2025-01-23..02-25. One zip per site; Kaunas first, Tallinn fills its
// gaps. The series splits at the two switch-overs, visible in the data: Kaunas changes
// character at 07:10 UTC on 2025-02-08 (leaving BRELL, announced for ~07:09) and matches
// KIT's Berlin recorder from 12:05 UTC on 2025-02-09 (|Kaunas - Berlin| drops from ~20
// to 1.2 mHz; second-to-second steps correlate r = 0.58 after, 0.0 before). A minute
// either side of each switch is left out. After it, Kaunas and Tallinn are Continental Europe, which
// Energy-Charts already covers. All four recorders run 1.3 s early against Energy-Charts
// (Berlin, 2025-02-10..11: r = 0.9976 at +1.3 s, 0.9958 unshifted; Kaunas the same):
// shifted +1 s.
constexpr double BALTIC_LEFT_IPS = 1738998480;     // 2025-02-08 07:08:00 UTC (kept before)
constexpr double BALTIC_ISLANDED = 1738998660;     // 2025-02-08 07:11:00 UTC
constexpr double BALTIC_JOINED_CE = 1739102640;    // 2025-02-09 12:04:00 UTC (island kept before)
std::vector<SeriesFile> baltic_files(const char* grid, const char* m0) {
  return {{grid, "https://osf.io/download/py9xg/", m0, "2025-02", "Kaunas.zip (0.1 s, 2025-01-23..02-25)"},
          {grid, "https://osf.io/download/n6pc8/", m0, "2025-02", "Tallinn.zip (0.1 s, 2025-01-23..02-25)"}};
}
const json::object BALTIC_META = {{"licence", "CC BY 4.0 (Power Grid Frequency DB, KIT; power-grid-frequency.org)"},
                                  {"project", "https://osf.io/m43tg/"},
                                  {"native_resolution_s", 0.1},
                                  {"timestamps", "UTC (GPS-synchronised), then +1 s (1.3 s early against Energy-Charts, 2025-02)"},
                                  {"measured_at", "Kaunas, Lithuania; Tallinn, Estonia"}};
const SeriesRelease BALTIC_IPS = {
    "osf_baltic", baltic_files("ips_ups", "2025-01"),
    {{""}, {"kaunas", "tallinn"}, {}, 0.0, 0, "", 1, 1e-3, -INFINITY, BALTIC_LEFT_IPS}, BALTIC_META};
const SeriesRelease BALTIC_ISLAND = {
    "osf_baltic", baltic_files("baltic_island_2025", "2025-02"),
    {{""}, {"kaunas", "tallinn"}, {}, 0.0, 0, "", 1, 1e-3, BALTIC_ISLANDED, BALTIC_JOINED_CE}, BALTIC_META};

const SeriesRelease* SERIES_RELEASES[] = {&AESO, &GRID_INDIA, &PMU_ZENODO, &DTU_TOKYO, &BALTIC_IPS, &BALTIC_ISLAND};

Loader series_loader(const SeriesRelease& r, const SeriesFile& f) {
  return [&r, &f](Ctx& c, SharedFile& s) -> asio::awaitable<void> {
    log(std::string("  reading ") + r.source + " " + f.note + " (" + f.m0 + ".." + f.m1 + ") ...");
    SeriesParser p(months_between(f.m0, f.m1), r.format);
    co_await stream_csv_or_zip(c, f.url, [&](std::string_view d) { p.feed(d); });
    s.months = p.finish();
    s.rows = p.rows;
    s.kept = p.kept;
  };
}

std::vector<const SeriesRelease*> releases_of(const std::string& source, const std::string& grid) {
  std::vector<const SeriesRelease*> out;
  for (auto* r : SERIES_RELEASES)
    if (source == r->source)
      for (auto& f : r->files)
        if (grid == f.grid) {
          out.push_back(r);
          break;
        }
  return out;
}

// By value: the coroutine outlives the wrappers' temporaries.
asio::awaitable<MonthData> series_fetch(std::string source, Ctx& c, std::string grid, std::string month) {
  auto [t0, t1] = month_bounds(month);
  std::vector<float> merged(std::size_t(t1 - t0), NAN);
  std::vector<json::object> used;
  std::vector<std::string> spans;
  const SeriesRelease* rel = nullptr;
  for (auto* r : releases_of(source, grid))
    for (auto& f : r->files) {
      if (grid != f.grid) continue;
      spans.push_back(f.m0 + ".." + f.m1);
      if (!(f.m0 <= month && month <= f.m1)) continue;
      rel = r;
      auto load = series_loader(*r, f);
      const std::string key = source + ":" + grid + ":" + f.url;  // a file may feed several grids, each its own window
      auto v = co_await take_month(c, key, month, load);
      auto s = c.files.contains(key) ? c.files[key] : nullptr;
      used.push_back({{"file", f.note}, {"url", f.url}, {"filled_s", v ? fill_gaps(merged, *v) : 0}, {"rows", s ? s->rows : 0},
                      {"rows_kept", s ? s->kept : 0}});
    }
  if (!rel) throw Unavailable(source + " has no " + grid + " data for " + month + " (files: " + join(spans, ", ") + ")");
  if (!any_finite(merged)) throw Unavailable(source + " holds no valid " + grid + " data in " + month);
  MonthData md;
  md.values = std::move(merged);
  md.meta = rel->meta;
  md.meta["source"] = source;
  md.meta["files"] = to_json_array(used);
  co_return md;
}

asio::awaitable<std::vector<std::string>> series_months(std::string source, std::string grid) {
  std::set<std::string> out;
  for (auto* r : releases_of(source, grid))
    for (auto& f : r->files)
      if (grid == f.grid)
        for (auto& m : months_between(f.m0, f.m1)) out.insert(m);
  co_return std::vector<std::string>(out.begin(), out.end());
}

}  // namespace

Fetch series_fetcher(const char* source) {
  return [source](Ctx& c, const std::string& grid, const std::string& month) { return series_fetch(source, c, grid, month); };
}
Months series_lister(const char* source) {
  return [source](Ctx&, const std::string& grid) { return series_months(source, grid); };
}

// ---------------------------------------------------------------- ERCOT 2011-12 (ETTS archive)

namespace {

// ERCOT's Emerging Technology Tracking System page kept two zips of its EMS historian
// (OSIsoft PI) at 4 s: ERCOT.FHZ is the system frequency. 2011: monthly CSVs, all of July
// and August but only the 1st-7th of the other months. 2012-01..06: one .xls per month,
// cut at Excel 97's 65,536 rows (about 3 days). Times are Central prevailing time (the
// 01:00 hour repeats on 2011-11-06).
constexpr const char* ERCOT_2011_URL = "https://www.ercot.com/files/docs/2012/08/21/2011_regulation_data.zip";
constexpr const char* ERCOT_2012_URL = "https://www.ercot.com/files/docs/2012/07/06/2012_regulation_and_frequency_data.zip";
// The historian repeats its last value while the feed is down: identical runs fall off
// steeply up to ~20 s (1,760 runs of >= 16 s, 88 of >= 30 s, most of those > 1 min).
constexpr int ERCOT_FLAT_S = 30;
const SeriesFormat ERCOT_FORMAT = {{"", "time"}, {"ercot.fhz"}, {}, 60.0, 0, "America/Chicago", 0};

// One 2012 workbook as "time,ERCOT.FHZ" lines (Excel serial dates -> ISO wall-clock time).
void ercot_xls_lines(std::string_view xls, const std::function<void(std::string_view)>& feed) {
  std::map<std::uint32_t, std::pair<double, double>> rows;  // row -> (serial, Hz)
  std::uint32_t tcol = 1, fcol = UINT32_MAX;
  sheets::read(std::as_bytes(std::span(xls.data(), xls.size())), {}, [&](std::uint32_t r, std::uint32_t c, const sheets::Cell& cell) {
    if (!cell.str.empty() && iequals(trim(cell.str), "ERCOT.FHZ")) fcol = c;
    if (!cell.is_num()) return;
    auto& row = rows.try_emplace(r, NAN, NAN).first->second;
    if (c == tcol) row.first = cell.num;
    else if (c == fcol) row.second = cell.num;
  });
  if (fcol == UINT32_MAX) throw std::runtime_error("ERCOT 2012 workbook has no ERCOT.FHZ column");
  std::string out = "time,ERCOT.FHZ\n";
  for (auto& [r, tv] : rows) {
    auto [serial, hz] = tv;
    if (!std::isfinite(serial) || !std::isfinite(hz)) continue;
    const i64 s = std::llround((serial - 25569.0) * 86400.0);  // Excel day 25569 = 1970-01-01
    const i64 d = floor_div(s, 86400), sec = s - d * 86400;
    auto cd = civil_from_days(d);
    char b[64];
    std::snprintf(b, sizeof b, "%04d-%02u-%02u %02d:%02d:%02d,%.6f\n", cd.y, cd.m, cd.d, int(sec / 3600), int(sec / 60 % 60), int(sec % 60), hz);
    out += b;
  }
  feed(out);
}

Loader ercot_loader(int year) {
  return [year](Ctx& c, SharedFile& s) -> asio::awaitable<void> {
    const bool y11 = year == 2011;
    log(std::string("  reading ERCOT ") + (y11 ? "2011 frequency CSVs" : "2012 frequency workbooks") + " ...");
    fs::path p = co_await cached_download(c, y11 ? ERCOT_2011_URL : ERCOT_2012_URL, y11 ? "ercot_2011_regulation_data.zip" : "ercot_2012_regulation_and_frequency_data.zip");
    MappedFile mf(p);
    zip::Archive z(mf.bytes());
    std::vector<const zip::Entry*> members;
    for (auto& e : z.entries())
      if (ends_with_ci(e.name, y11 ? ".csv" : ".xls")) members.push_back(&e);
    std::sort(members.begin(), members.end(), [](auto* a, auto* b) { return a->name < b->name; });  // "11_01 ..": chronological
    if (members.empty()) throw std::runtime_error(std::string("ERCOT ") + (y11 ? "2011" : "2012") + " zip holds no data files");
    SeriesParser parser(months_between(y11 ? "2011-01" : "2012-01", y11 ? "2011-12" : "2012-06"), ERCOT_FORMAT);
    auto feed = [&](std::string_view d) { parser.feed(d); };
    for (auto* e : members) {
      if (y11) z.read(*e, feed);
      else ercot_xls_lines(z.read_all(*e), feed);
      parser.feed("\n");
    }
    s.months = parser.finish();
    s.rows = parser.rows;
    s.kept = parser.kept;
  };
}

}  // namespace

asio::awaitable<MonthData> ercot_etts_fetch(Ctx& c, const std::string& grid, const std::string& month) {
  const int year = std::stoi(month.substr(0, 4));
  if (grid != "ercot" || !(month >= "2011-01" && month <= "2012-06")) throw Unavailable("ercot_etts covers ercot 2011-01..2012-06 only");
  const std::string key = "ercot_etts:" + std::to_string(year);
  auto v = co_await take_month(c, key, month, ercot_loader(year));
  if (!v || !any_finite(*v)) throw Unavailable("ERCOT's " + std::to_string(year) + " file holds no frequency for " + month);
  auto s = c.files.contains(key) ? c.files[key] : nullptr;
  MonthData md;
  md.values = std::move(*v);
  const std::size_t stale = drop_flat_runs(md.values, ERCOT_FLAT_S);
  if (!any_finite(md.values)) throw Unavailable("ERCOT's " + std::to_string(year) + " file holds only stale frequency for " + month);
  if (stale) log("  ERCOT " + month + ": " + std::to_string(stale) + " s of stale (repeated) values dropped");
  md.meta = {{"source", "ercot_etts"},
             {"url", year == 2011 ? ERCOT_2011_URL : ERCOT_2012_URL},
             {"licence", "ERCOT public posting (ercot.com, Emerging Technology Tracking System archive)"},
             {"native_resolution_s", 4.0},
             {"point", "ERCOT.FHZ (EMS system frequency)"},
             {"timestamps", "Central prevailing time (America/Chicago), converted to UTC; no independent timing check (no other ERCOT data that year)"},
             {"rows", s ? s->rows : 0},
             {"rows_kept", s ? s->kept : 0}};
  co_return md;
}

asio::awaitable<std::vector<std::string>> ercot_etts_months(Ctx&, const std::string& grid) {
  co_return grid == "ercot" ? months_between("2011-01", "2012-06") : std::vector<std::string>{};
}

// ---------------------------------------------------------------- LBNL micro-PMU (Berkeley, 2015-10-01..12)

namespace {

// The ARPA-E micro-PMU reference dataset (Stewart, LBNL 2016): GPS-timed PQube-based
// recorders on LBNL's campus distribution feeders, 120 samples/s, one gzip per channel
// with "<UTC ns>,<value>" lines. The dataset is described as 2015-10..12, but every
// public file stops at 120,000,000 rows: 2015-10-01 00:00 .. 10-12 13:46 UTC. No
// frequency channel: it comes from the phase-1 voltage angle (L1ANG) of bus A6. The other
// recorders' files cover the same 11.6 days (Grizzly's filled 3 s of A6's gaps), so only
// A6 is read.
constexpr const char* LBNL_BASE = "https://powerdata-download.lbl.gov/data/";
const std::vector<std::pair<std::string, std::string>> LBNL_FILES = {{"a6_bus1", "_LBNL_a6_bus1_L1ANG.gz"}};

Loader lbnl_loader(std::string file) {
  return [file = std::move(file)](Ctx& c, SharedFile& s) -> asio::awaitable<void> {
    log("  reading LBNL micro-PMU " + file + " (~850 MB gzip, 120 samples/s) ...");
    const i64 t0 = month_bounds("2015-10").first, t1 = month_bounds("2015-10").second;
    UpmuAngleParser p(t0, t1);
    zip::Gunzip gz([&](std::string_view d) { p.feed(d); });
    co_await stream_url(c, std::string(LBNL_BASE) + file, [&](std::string_view d) { gz.feed(d); });
    gz.finish();
    auto all = p.finish();
    s.months["2015-10"] = std::move(all);
    s.rows = p.rows;
    s.kept = p.kept;
  };
}

}  // namespace

asio::awaitable<MonthData> lbnl_upmu_fetch(Ctx& c, const std::string& grid, const std::string& month) {
  if (grid != "us_western" || month != "2015-10") throw Unavailable("lbnl_upmu covers us_western 2015-10 only");
  auto [t0, t1] = month_bounds(month);
  std::vector<float> merged(std::size_t(t1 - t0), NAN);
  std::vector<json::object> used;
  for (auto& [site, file] : LBNL_FILES) {
    const std::string key = "lbnl_upmu:" + file;
    auto v = co_await take_month(c, key, month, lbnl_loader(file));
    auto s = c.files.contains(key) ? c.files[key] : nullptr;
    used.push_back({{"recorder", site}, {"url", std::string(LBNL_BASE) + file}, {"filled_s", v ? fill_gaps(merged, *v) : 0},
                    {"rows", s ? s->rows : 0}, {"rows_kept", s ? s->kept : 0}});
    if (std::none_of(merged.begin(), merged.end(), [](float x) { return std::isnan(x); })) break;
  }
  if (!any_finite(merged)) throw Unavailable("LBNL micro-PMU files hold no data for " + month);
  MonthData md;
  md.values = std::move(merged);
  md.meta = {{"source", "lbnl_upmu"},
             {"licence", "LBNL Open uPMU reference dataset (Stewart 2016, doi:10.21990/C21599); cite on use"},
             {"native_resolution_s", 1.0 / 120},
             {"derived_from", "phase-1 voltage angle (L1ANG): frequency = 60 Hz + d(angle)/dt / 360, averaged per second"},
             {"timestamps", "UTC nanoseconds, GPS-synchronised"},
             {"measured_at", "Lawrence Berkeley National Laboratory distribution feeder (bus A6), Berkeley, California"},
             {"files", to_json_array(used)}};
  co_return md;
}

asio::awaitable<std::vector<std::string>> lbnl_upmu_months(Ctx&, const std::string& grid) {
  co_return grid == "us_western" ? std::vector<std::string>{"2015-10"} : std::vector<std::string>{};
}

}  // namespace geo::refs::detail
