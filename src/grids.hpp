// Synchronous-grid table: which grid covers which area when, 50 vs 60 Hz.
// data/grids.json defines every grid's extent as dated areas; data/grid_areas.json holds
// their polygons (one feature per area) for "which grid holds this point at this time".
#pragma once

#include <array>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace geo::grids {

// ISO date "YYYY-MM-DD" for epoch seconds (helpers accept dates as such strings).
std::string day_of(double t);
std::string today();
// Accepts "YYYY-MM" (first day), "YYYY-MM-DD..." (first 10 chars); empty = today.
std::string normalize_day(const std::string& s);

struct Area {
  std::vector<std::string> include, exclude;  // ISO codes only (non-code references dropped)
  std::optional<std::string> start, end;     // ISO dates, [start, end)
  std::string status = "synchronous", source;
  bool active(const std::string& day) const {
    return (!start || *start <= day) && (!end || day < *end);
  }
};

struct Grid {
  std::string id, name;
  int nominal_hz = 50;
  std::vector<Area> areas;
  std::string reference = "tracked", notes;
  bool tracked() const { return reference != "none"; }
  std::vector<std::string> countries_at(const std::string& day) const;
  std::vector<std::string> countries() const;  // today's; else those of its last period
  bool exists_at(const std::string& day) const;
  std::optional<std::string> status_at(const std::string& day) const;
};

struct GridTable {
  std::vector<Grid> grids;  // file order
  std::vector<std::string> sixty_hz_countries;
  std::map<std::string, std::string> mixed_countries;
  const Grid* find(const std::string& id) const;
  const Grid& get(const std::string& id) const;  // throws std::out_of_range with the known ids
  std::vector<const Grid*> tracked() const;
};
const GridTable& load_grids();

struct Placement {
  std::optional<std::string> grid, status;
  double distance_km = 0;
};
std::vector<Placement> grids_at(double lat, double lon, const std::string& day, double snap_km = 10.0);
// West, south, east, north of the grid's areas in effect on `day` (nullopt if none).
std::optional<std::array<double, 4>> grid_extent(const std::string& grid, const std::string& day);

// grids_at for many days: computed once per distinct set of areas in effect.
std::vector<std::vector<Placement>> grids_at_days(double lat, double lon, const std::vector<std::string>& days, double snap_km = 10.0);

}  // namespace geo::grids
