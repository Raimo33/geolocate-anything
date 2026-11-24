// End-to-end analysis: file clues -> hum detection -> edit points -> ENF traces ->
// grid/time match -> verdict. Every continuous segment and hum hypothesis is extracted
// first; then each grid is streamed once for all of them together.
#pragma once

#include <optional>
#include <string>
#include <vector>

#include "air/fusion.hpp"
#include "audio.hpp"
#include "cuts.hpp"
#include "enf/match.hpp"
#include "grids.hpp"
#include "tzclue.hpp"
#include "util/json.hpp"

namespace geo::locate {

inline constexpr double MIN_LOCATE_S = 60.0;

struct HumMatch {
  enf::HumDetection hum;
  std::optional<enf::ENFTrace> trace;
  std::vector<match::GridResult> results;
  std::optional<match::Verdict> verdict;
  json::array candidate_regions;
  std::vector<std::string> warnings;
};

struct SegmentReport {
  int index = 0;
  double start_s = 0, end_s = 0;
  HumMatch match;
  double duration_s() const { return end_s - start_s; }
};

struct Report {
  std::string path;
  double duration_s = 0;
  audio::FileClues clues;
  enf::HumDetection hum;
  std::optional<enf::ENFTrace> trace;
  std::vector<match::GridResult> results;
  std::optional<match::Verdict> verdict;
  json::array candidate_regions;
  std::vector<std::string> warnings, consistency;
  std::vector<cuts::Cut> cuts;
  std::vector<SegmentReport> segments;
  tzclue::Result tz;  // metadata clock vs. hum time: the device's UTC offset and what it leaves of the grid
  std::optional<air::AircraftStage> aircraft;  // flyovers vs. ADS-B history (air/fusion.hpp)
  // The hum re-searched around the aircraft's time, in the grid of the aircraft's place.
  struct HumCheck {
    std::string grid;
    double aircraft_start = NAN;
    double window_s = 0;
    int rank = -1;  // of the hum candidate within 3 s of the aircraft's time (0 = best), -1 none
    std::optional<match::Candidate> candidate;
    std::string note;
  };
  std::vector<HumCheck> hum_checks;
};

struct AnalyzeOptions {
  std::vector<std::string> grids;
  std::optional<std::string> from, to;
  std::optional<int> nominal;
  double frame_s = enf::DEFAULT_FRAME_S;
  int top_k = 30;
  bool split = true;
  // Aircraft: auto runs when ADS-B history is stored (`geoloc aircraft fetch`).
  enum class Aircraft { Auto, On, Off } aircraft = Aircraft::Auto;
  std::optional<std::array<double, 4>> aircraft_box;  // west, south, east, north
  std::vector<std::array<double, 2>> area;             // forced (lon, lat) polygon for the aircraft answer
  bool aircraft_fetch = false;                         // download missing ADS-B days
  bool opensky = false;                                // check the aircraft answers against OpenSky too
  int opensky_places = 0;                              // best places checked (0: GEOLOC_OPENSKY_PLACES or 5)
};

Report analyze(const std::string& path, const AnalyzeOptions& o = {});
std::string render_text(const Report& rep);
json::object to_json(const Report& rep);
// The dashboard's view: traces thinned to <= 600 points, plus the grid table.
json::object to_web_json(const Report& rep);
json::array grid_table();  // known grids + stored reference coverage

json::object region(const grids::Grid& g, const std::string& day = {});
std::optional<int> grid_nominal(const std::string& grid_id);

}  // namespace geo::locate
