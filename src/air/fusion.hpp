// Aircraft and grid hum together.
//
// The two signals fix complementary things. The hum gives the grid (continent scale) and
// the UTC second, but often too weakly to be sure; flyovers give a place to within a few
// hundred metres, but only once the time is known to a few seconds or two flyovers pin
// it. So:
//   - a located hum match hands its second (+-3 s) and its grid's extent to the aircraft
//     search, where a single flyover can suffice;
//   - a weak hum hands its best few candidate seconds over; the one an aircraft match
//     (two flyovers at least) confirms is the answer for both;
//   - without hum, a window (from --from/--to) and an area: the aircraft result gives
//     place and time, and the hum is then re-searched only around that time in the grid
//     of that place, where far fewer wrong alignments compete (locate.cpp).
#pragma once

#include <optional>
#include <string>
#include <vector>

#include "air/events.hpp"
#include "air/search.hpp"
#include "util/json.hpp"

namespace geo::air {

struct Hypothesis {
  std::string label;                // where the time came from
  std::optional<std::string> grid;  // restrict to this grid's area
  double start_min = NAN, start_max = NAN;
  int min_events = 2;
  std::optional<Bbox> box;          // explicit area (else the grid's extent)
};

struct HypothesisResult {
  Hypothesis h;
  std::optional<SearchResult> result;
  std::string note;  // why it was not searched
};

struct AircraftOptions {
  std::optional<Bbox> box;  // area for hypotheses without a grid
  bool fetch = false;       // download missing ADS-B days (~4 GB each)
  bool online = true;       // DEM tiles
  int max_days = 3;         // at most this many days fetched per run
};

struct AircraftStage {
  Detection detection;
  std::vector<HypothesisResult> tried;
  int best = -1;  // index into tried of the best search
  std::vector<std::string> notes;
  const SearchResult* best_result() const { return best >= 0 ? &*tried[std::size_t(best)].result : nullptr; }
};

// Detects the flyovers of the file and searches each hypothesis that has ADS-B history.
AircraftStage run_aircraft(const std::string& path, const std::vector<Hypothesis>& hyps, const AircraftOptions& o);

json::object stage_json(const AircraftStage& s);
std::string render_stage(const AircraftStage& s);

}  // namespace geo::air
