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

#include <array>
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

// What the dashboard shows of the best answer: each matched aircraft with its track
// around the closest approach, and each flyover's spectrogram with the curves drawn on it.
struct FlightView {
  int event = 0;  // index into detection.events
  std::string flight, reg, type, desc, owner, year, kind, engine;
  int engines = 0;
  double cpa_m = 0, speed = 0, score = 0, fit = 0, f0 = 0, cpa_utc = 0;
  double cpa_lat = NAN, cpa_lon = NAN, cpa_alt_m = NAN, cpa_track_deg = NAN;  // the aircraft at closest approach
  std::vector<std::array<double, 4>> track;  // epoch s, lat, lon, alt m (+-10 min, thinned)
};
struct SpectrumView {
  int event = 0;
  bool matched = false;
  double t0 = 0, t1 = 0, f_lo = 0, f_hi = 0;  // s from the recording's start, Hz
  int w = 0, h = 0;
  std::string pixels;  // base64 of w x h bytes (whitened level 0..255), row 0 at f_hi
  std::vector<std::vector<std::array<float, 2>>> model;     // per line: (t, f) along the matched aircraft's real path
  std::vector<std::vector<std::array<float, 2>>> straight;  // per line: the best straight pass (the measurement)
};

struct AircraftStage {
  Detection detection;
  std::vector<HypothesisResult> tried;
  int best = -1;  // index into tried of the best search
  std::vector<std::string> notes;
  std::vector<FlightView> flights;
  std::vector<SpectrumView> spectra;
  double start_utc = NAN;  // the best answer's start (spectrum times are relative to it)
  const SearchResult* best_result() const { return best >= 0 ? &*tried[std::size_t(best)].result : nullptr; }
};

// Detects the flyovers of the file and searches each hypothesis that has ADS-B history.
AircraftStage run_aircraft(const std::string& path, const std::vector<Hypothesis>& hyps, const AircraftOptions& o);

json::object stage_json(const AircraftStage& s);
std::string render_stage(const AircraftStage& s);

}  // namespace geo::air
