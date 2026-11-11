// Downloaders for reference grid-frequency data. Each source turns its own format into
// the store's 1 s series. Downloads are asynchronous (libcurl multi on the Asio event
// loop): several months stream at once, parsing happens as bytes arrive or right after
// a download lands, and writes to the store are serialised on the same loop.
#pragma once

#include <functional>
#include <map>
#include <tuple>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

#include "util/json.hpp"
#include "util/time.hpp"

namespace geo::refs {

struct Location {
  std::string label;
  double lat, lon;
};

// A source has no data for this month (remembered, so refreshes don't ask again).
struct Unavailable : std::runtime_error {
  using std::runtime_error::runtime_error;
};

struct SourceInfo {
  std::string name;
  std::vector<std::string> grids;
  std::string description, licence;
  bool live = false;
  int workers = 4;
  bool joint = false;  // one download fills all its grids: refresh those one after another
};

std::vector<SourceInfo> sources();
const SourceInfo* find_source(const std::string& name);
std::vector<std::string> sources_covering(const std::string& grid);
// Grids sharing a joint source (one download stores several grids) must not refresh at
// the same time: they get the same lane, other grids get lanes of their own.
std::string refresh_lane(const std::string& grid);

// Placement: every recorder behind a stored month must lie inside its grid that month.
struct PlacementIssue {
  std::string grid, source, month, label, severity, message;  // misplaced | partial | unknown
};
std::vector<Location> recorder_locations(const std::string& source, const std::string& grid, const json::object* info);
std::vector<PlacementIssue> placement_issues(const std::string& source, const std::string& grid, const std::string& month,
                                             const json::object* info);

bool is_cached(const std::string& source, const std::string& grid, const std::string& month);
std::vector<std::string> stale_months(const std::string& grid, const std::string& source);

// What bringing a grid up to date takes: (source, months) pairs. Needs the network for
// live sources' indexes (runs its own event loop). A source whose months cannot be
// listed (its index page down or changed) still gets its stored months refreshed;
// "source: why" goes to `problems` when given, so the refresh does not pass for done.
std::vector<std::pair<std::string, std::vector<std::string>>> refresh_plan(const std::string& grid, std::optional<double> now = {},
                                                                           std::vector<std::string>* problems = nullptr);

// Download and store months; returns the months now available. Runs an event loop.
// A month ends in one of three ways: stored (returned), unavailable (recorded in the
// manifest, not retried unless the source is live), or failed (a transient error:
// nothing is recorded and the next refresh plans it again). Failed months and their
// errors go to `failed` when given.
std::vector<std::string> fetch(const std::string& source, const std::string& grid, const std::vector<std::string>& months,
                               bool force = false, std::map<std::string, std::string>* failed = nullptr);

// Shift (s) to add to secondary's timestamps so it lines up with primary, and r; nullopt
// when the overlap is under min_overlap seconds.
std::optional<std::pair<int, double>> estimate_lag(const std::vector<float>& primary, const std::vector<float>& secondary,
                                                   int max_lag = 60, std::size_t min_overlap = 6 * 3600);

// Fingrid month archive (zip or 7z of daily 10 Hz CSVs in Finnish time) -> the UTC month
// and the spill into its neighbours.
struct FingridMonth {
  std::vector<float> own;
  std::vector<std::pair<i64, std::vector<float>>> spill;
  int daily_files = 0;
  std::size_t rows = 0;
};
FingridMonth parse_fingrid_archive(const std::string& path, const std::string& month);
// Throws when a NESO month (already in UTC) repeats the stored month before's last hour,
// the mark of times an hour off, or (past the months checked by hand) when that month
// is not stored to check against.
void check_neso_seam(const std::string& grid, const std::string& month, const std::vector<float>& own);
// The 3-day requests one Energy-Charts month takes (inclusive ends), up to `now`.
std::vector<std::pair<i64, i64>> energy_charts_requests(const std::string& month, double now);

namespace testing {
struct FakeMonth {
  std::vector<float> values;
  json::object meta;
  std::vector<std::pair<i64, std::vector<float>>> spill;
  std::vector<std::tuple<std::string, std::vector<float>, json::object>> also;
};
// Registers a source whose months come from fetch (throw Unavailable for "no data").
void add_fake_source(SourceInfo info, std::function<FakeMonth(const std::string& grid, const std::string& month)> fetch,
                     std::function<std::vector<std::string>(const std::string& grid)> months, int format = 1);
void remove_fake_sources();
}  // namespace testing

}  // namespace geo::refs
