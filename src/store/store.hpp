// Reference grid-frequency store.
//
//   $GEOLOC_CACHE/refs/<grid>/<source>/<YYYY-MM>.u16z   one compressed file per UTC month
//   $GEOLOC_CACHE/refs/<grid>/<source>/manifest.json    provenance + per-month coverage
//
// Addressing is arithmetic (second t -> month file -> day chunk), reads decode only the
// days they touch from a memory map, writes fill gaps or replace a span, and several sources
// per grid are merged at read time by rank. See codec.hpp and docs/grids/architecture.md.
#pragma once

#include <functional>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "util/json.hpp"
#include "util/time.hpp"

namespace geo::store {

namespace fs = std::filesystem;

fs::path cache_dir();
fs::path refs_dir();
int source_rank(std::string_view source);

// ---------------------------------------------------------------- manifests
json::object read_manifest(const std::string& grid, const std::string& source);
void write_manifest(const std::string& grid, const std::string& source, const json::object& man);
// Changes the latest manifest on disk under an exclusive lock (safe across processes).
void update_manifest(const std::string& grid, const std::string& source, const std::function<void(json::object&)>& change);
std::vector<std::string> sources_for(const std::string& grid);  // by rank, then name
std::vector<std::string> grid_dirs();                          // grids that have a directory
bool has_month(const std::string& grid, const std::string& source, const std::string& month);
std::optional<json::object> month_info(const std::string& grid, const std::string& source, const std::string& month);

// ---------------------------------------------------------------- resampling
// Streams samples of any rate onto the 1 s grid [t0, t1) in O(t1 - t0) memory. finish():
// seconds with samples -> their mean; for sources coarser than 1 s, gaps up to
// max(30 s, 3 x spacing) are linearly interpolated; longer gaps stay NaN.
class RegularAccumulator {
 public:
  RegularAccumulator(i64 t0, i64 t1);
  void add(double t, double v) {
    if (!(v == v) || !(t >= t0d_) || !(t < t1d_)) return;  // NaN-safe
    auto i = static_cast<std::size_t>(t - t0d_);
    sums_[i] += v;
    counts_[i] += 1;
    ++n_samples_;
  }
  std::vector<float> finish() const;
  i64 t0() const { return t0_; }
  i64 t1() const { return t1_; }
  std::size_t n_samples() const { return n_samples_; }

 private:
  i64 t0_, t1_;
  double t0d_, t1d_;
  std::vector<double> sums_;
  std::vector<std::int32_t> counts_;
  std::size_t n_samples_ = 0;
};

// ---------------------------------------------------------------- writing
// Writes a 1 s series starting at UTC second t0 (any span). Only samples present are
// written (existing data in our gaps survives); values are rounded to the 1 mHz step.
void write_range(const std::string& grid, const std::string& source, i64 t0, std::span<const float> dev_hz);
// Writes [t0, t0 + size): those seconds hold exactly dev_hz afterwards (NaN = missing),
// whatever was stored there before. For a source's fresh download of a span it covers.
void replace_range(const std::string& grid, const std::string& source, i64 t0, std::span<const float> dev_hz);
// The month's manifest entry: format/url/files/partial from meta, coverage of what the
// month holds now, stored_at = now. The source's "meta" collects the other keys.
void write_entry(const std::string& grid, const std::string& source, const std::string& month, const json::object& meta);
// write_range for one whole month + its manifest entry.
void save_month(const std::string& grid, const std::string& source, const std::string& month, std::span<const float> values,
                const json::object& meta);
void mark_unavailable(const std::string& grid, const std::string& source, const std::string& month, std::string_view reason);
void refresh_coverage(const std::string& grid, const std::string& source, const std::string& month);

// ---------------------------------------------------------------- reading
// Merged deviation series (Hz, NaN = missing) for UTC seconds [t0, t1). Writes into out
// (size t1 - t0). sources empty = all, by rank.
void read(const std::string& grid, i64 t0, i64 t1, std::span<float> out, const std::vector<std::string>& sources = {});
std::vector<float> read(const std::string& grid, i64 t0, i64 t1, const std::vector<std::string>& sources = {});
// Last UTC second (exclusive) holding data at or before t1, scanning back at most max_back s.
std::optional<i64> last_data_end(const std::string& grid, i64 t1, i64 max_back);

struct Interval {
  i64 a, b;
};
std::vector<Interval> coverage_intervals(const std::string& grid, const std::vector<std::string>& sources = {});
// Stored months by start (UTC s), each with the fraction of its seconds holding data: the best
// source's recorded coverage (manifest "coverage", 1 when an old entry lacks it).
std::vector<std::pair<i64, double>> month_fill(const std::string& grid);

struct RefSeries {
  std::string grid;
  i64 start = 0;
  std::vector<float> values;
  i64 end() const { return start + static_cast<i64>(values.size()); }
  double coverage() const;
};
std::optional<RefSeries> load_grid(const std::string& grid, std::optional<i64> from = {}, std::optional<i64> to = {});

struct Inventory {
  std::string grid, source;
  std::vector<std::string> months;
};
std::vector<Inventory> inventory();
std::uint64_t disk_usage(const std::string& grid = {});

void invalidate_caches();  // after files changed behind our back (bucket pull)

}  // namespace geo::store
