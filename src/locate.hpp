// End-to-end analysis: file clues -> hum detection -> edit points -> ENF traces ->
// grid/time match -> verdict. Every continuous segment and hum hypothesis is extracted
// first; then each grid is streamed once for all of them together.
#pragma once

#include <optional>
#include <string>
#include <vector>

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
};

struct AnalyzeOptions {
  std::vector<std::string> grids;
  std::optional<std::string> from, to;
  std::optional<int> nominal;
  double frame_s = enf::DEFAULT_FRAME_S;
  int top_k = 30;
  bool split = true;
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
