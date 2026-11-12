// Internals shared by the reference-data downloaders (sources*.cpp): the per-loop
// context, what a fetcher returns, download helpers, and each provider's fetcher and
// month lister for the source table in sources.cpp. Not part of the public API.
#pragma once

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

#include "net/http.hpp"
#include "refs/parsers.hpp"
#include "refs/sources.hpp"
#include "util/files.hpp"

namespace geo::refs::detail {

namespace asio = boost::asio;
using net::Http;

void log(const std::string& msg);

struct Also {
  std::string grid;
  std::vector<float> values;
  json::object meta;
  std::vector<Spill> spill;
};
struct MonthData {
  std::vector<float> values;
  json::object meta;
  std::vector<Spill> spill;
  std::vector<Also> also;  // one download, several grids (AEMO)
};

struct SharedFile;

// Per-event-loop state: HTTP client, indexes, shared downloads.
struct Ctx {
  asio::io_context& io;
  Http http;
  std::optional<std::map<std::string, std::string>> neso, fingrid;
  std::map<std::string, std::shared_ptr<SharedFile>> files;  // shared multi-month files, by key
  std::map<std::string, std::shared_ptr<net::AsyncEvent>> in_flight;  // cached downloads
  net::AsyncSemaphore energy_charts;                                  // one request at a time
  // What partial months being fetched again already hold, by (grid, month): the live
  // sources skip what is stored and fetch only the rest.
  std::map<std::pair<std::string, std::string>, std::vector<float>> have;
  explicit Ctx(asio::io_context& i) : io(i), http(i), energy_charts(i.get_executor(), 1) {}
};

using Fetch = std::function<asio::awaitable<MonthData>(Ctx&, const std::string&, const std::string&)>;
using Months = std::function<asio::awaitable<std::vector<std::string>>(Ctx&, const std::string&)>;

struct Source {
  SourceInfo info;
  Fetch fetch;
  Months months;
  // Version of what the fetcher stores: bumped when a fix changes stored data, so months
  // stored by an older build are fetched again.
  int format = 1;
  std::map<std::string, std::vector<Location>> locations;
  bool fake = false;  // registered by a test
  // The environment variable (an account's token) the source needs to be refreshed at
  // all; without it refresh plans leave the source alone (explicit fetches still run).
  const char* needs_env = nullptr;
  // Months to fetch again although their format is current: {from, to, format} raises the
  // format those months need (a station table rescreened for a span of years). Months
  // stored, or marked unavailable, before that are fetched again.
  std::vector<std::tuple<std::string, std::string, int>> month_formats;
};

// A file several months share (a campaign CSV, a yearly archive): read once per process
// by a loader that fills `months`; each month is handed out once and then freed (asking
// for it again reads the file again).
struct SharedFile {
  explicit SharedFile(asio::any_io_executor ex) : ready(ex) {}
  net::AsyncEvent ready;
  std::map<std::string, std::vector<float>> months;
  std::set<std::string> handed_out;
  std::size_t rows = 0, kept = 0;
  std::exception_ptr error;
};
using Loader = std::function<asio::awaitable<void>(Ctx&, SharedFile&)>;

// A month of a live API (Energy-Charts, EirGrid) stays partial until two weeks after it
// ends: each refresh in that time asks again only for what is still missing (the hours
// the API had not published yet at the month's end, a request answered with a 404 or
// an empty day), then the month is final with whatever the source had.
constexpr double SETTLE_S = 14 * 86400.0;

// ---------------------------------------------------------------- download helpers (source_util.cpp)

std::string now_month();
json::array to_json_array(const std::vector<json::object>& v);
fs::path tmp_dir();
// Stream a URL into a file (atomic), with retries.
asio::awaitable<void> download_to(Ctx& c, const std::string& url, const fs::path& dst);
// A file several months share (yearly archives): downloaded once into the cache.
asio::awaitable<fs::path> cached_download(Ctx& c, const std::string& url, const std::string& name);
// A CSV, a ZIP holding CSVs or a 7z, parsed through `feed` as it streams in.
asio::awaitable<void> stream_csv_or_zip(Ctx& c, const std::string& url, const std::function<void(std::string_view)>& feed);
asio::awaitable<std::string> get_text(Ctx& c, const std::string& url, int retries = 3);
// Index cached on disk for a day.
asio::awaitable<std::map<std::string, std::string>> cached_index(Ctx& c, const char* file, const std::string& url,
                                                                 std::map<std::string, std::string> (*parse)(std::string_view));
// Loads (3 attempts; Unavailable is final) or waits for the load already running.
asio::awaitable<std::shared_ptr<SharedFile>> shared_file(Ctx& c, const std::string& key, const Loader& load);
// One month of a shared file; nullopt when the file does not cover it.
asio::awaitable<std::optional<std::vector<float>>> take_month(Ctx& c, const std::string& key, const std::string& month,
                                                              const Loader& load);
// Stream a URL through `feed`, failing on any HTTP error.
asio::awaitable<void> stream_url(Ctx& c, const std::string& url, const std::function<void(std::string_view)>& feed,
                                 std::optional<std::pair<std::uint64_t, std::uint64_t>> range = {});
bool any_finite(const std::vector<float>& v);
// Fill `into`'s gaps from `from`; returns the seconds filled.
std::int64_t fill_gaps(std::vector<float>& into, const std::vector<float>& from);
// Whether [a, b) (clipped to the month) is already stored in a partial month being
// fetched again: 99% of its seconds hold data.
bool already_have(const Ctx& c, const std::string& grid, const std::string& month, i64 a, i64 b);
// Add what was already stored to a freshly fetched (partly skipped) month.
void merge_have(const Ctx& c, const std::string& grid, const std::string& month, std::vector<float>& v);

// ---------------------------------------------------------------- Europe (sources_europe.cpp)

// 2: UK wall-clock time in the dd/mm/yyyy files. 3: the BST-stamped 2025-04..10 files.
constexpr int NESO_FORMAT = 3;
asio::awaitable<MonthData> neso_fetch(Ctx& c, const std::string& grid, const std::string& month);
asio::awaitable<std::vector<std::string>> neso_months(Ctx& c, const std::string&);
extern const std::map<std::string, Location> OSF_LOCATIONS;  // by country
asio::awaitable<MonthData> osf_fetch(Ctx& c, const std::string& grid, const std::string& month);
asio::awaitable<std::vector<std::string>> osf_months(Ctx&, const std::string&);
asio::awaitable<MonthData> fingrid_fetch(Ctx& c, const std::string& grid, const std::string& month);
asio::awaitable<std::vector<std::string>> fingrid_months(Ctx& c, const std::string&);
asio::awaitable<MonthData> energy_charts_fetch(Ctx& c, const std::string& grid, const std::string& month);
asio::awaitable<std::vector<std::string>> energy_charts_months(Ctx&, const std::string&);
asio::awaitable<MonthData> ntp_fetch(Ctx& c, const std::string& grid, const std::string& month);
asio::awaitable<std::vector<std::string>> ntp_months(Ctx&, const std::string&);
asio::awaitable<MonthData> oenf_fetch(Ctx& c, const std::string& grid, const std::string& month);
asio::awaitable<std::vector<std::string>> oenf_months(Ctx&, const std::string&);
asio::awaitable<MonthData> fdr_fetch(Ctx& c, const std::string& grid, const std::string& month);
asio::awaitable<std::vector<std::string>> fdr_months(Ctx&, const std::string&);
asio::awaitable<MonthData> eirgrid_fetch(Ctx& c, const std::string& grid, const std::string& month);
asio::awaitable<std::vector<std::string>> eirgrid_months(Ctx&, const std::string&);

// ---------------------------------------------------------------- Australia (sources_australia.cpp)

// 2: NSW1 instead of the four mainland regions' mean; the month's edges from the
// neighbouring months' files (before, the last 10 h of each month were lost).
constexpr int AEMO_FPP_FORMAT = 2;
extern const std::map<std::string, std::vector<Location>> AEMO_LOCATIONS;
asio::awaitable<MonthData> aemo_fpp_fetch(Ctx& c, const std::string& grid, const std::string& month);
asio::awaitable<std::vector<std::string>> aemo_fpp_months(Ctx&, const std::string&);
asio::awaitable<MonthData> aemo_cp_fetch(Ctx& c, const std::string& grid, const std::string& month);
asio::awaitable<std::vector<std::string>> aemo_cp_months(Ctx&, const std::string&);
asio::awaitable<MonthData> aemo_annual_fetch(Ctx&, const std::string&, const std::string& month);
asio::awaitable<std::vector<std::string>> aemo_annual_months(Ctx&, const std::string&);

// ---------------------------------------------------------------- releases and campaigns (sources_archives.cpp)

asio::awaitable<MonthData> edr_fetch(Ctx& c, const std::string& grid, const std::string& month);
asio::awaitable<std::vector<std::string>> edr_months(Ctx&, const std::string& grid);
std::vector<std::string> edr_grids();
// The recorders of these EDR sites (none: all the grid's sites).
std::vector<Location> edr_locations(const std::string& grid, std::vector<std::string> sites);
extern const std::map<std::string, std::vector<Location>> ZK_LOCATIONS;
asio::awaitable<MonthData> zk_fetch(Ctx& c, const std::string& grid, const std::string& month);
asio::awaitable<std::vector<std::string>> zk_months(Ctx&, const std::string& grid);
// Fetcher and lister of a one-off release of time/frequency CSVs (AESO, Grid-India, ...).
Fetch series_fetcher(const char* source);
Months series_lister(const char* source);
asio::awaitable<MonthData> ercot_etts_fetch(Ctx& c, const std::string& grid, const std::string& month);
asio::awaitable<std::vector<std::string>> ercot_etts_months(Ctx&, const std::string& grid);
asio::awaitable<MonthData> lbnl_upmu_fetch(Ctx& c, const std::string& grid, const std::string& month);
asio::awaitable<std::vector<std::string>> lbnl_upmu_months(Ctx&, const std::string& grid);

}  // namespace geo::refs::detail
