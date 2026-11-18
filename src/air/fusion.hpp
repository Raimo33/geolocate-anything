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
#include <functional>
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
  Area area;                        // forced area: candidates outside it go
};

struct HypothesisResult {
  Hypothesis h;
  std::optional<SearchResult> result;
  std::string note;  // why it was not searched
};

struct AircraftOptions {
  std::optional<Bbox> box;  // area for hypotheses without a grid
  Area area;                // forced area: every search stays inside it
  bool fetch = false;       // download missing ADS-B days (~4 GB each)
  bool online = true;       // DEM tiles
  int max_days = 3;         // at most this many days fetched per run
};

// What the dashboard shows of the best answer: each matched aircraft with its track
// around the closest approach, and each flyover's spectrogram with the curves drawn on it.
struct FlightView {
  int event = 0;  // index into detection.events
  int clip = -1;  // edited file: the clip (hum segment) it was heard in
  std::string flight, reg, type, desc, owner, year, kind, engine;
  int engines = 0;
  double cpa_m = 0, speed = 0, score = 0, fit = 0, f0 = 0, cpa_utc = 0;
  double cpa_lat = NAN, cpa_lon = NAN, cpa_alt_m = NAN, cpa_track_deg = NAN;  // the aircraft at closest approach
  std::vector<std::array<double, 4>> track;  // epoch s, lat, lon, alt m (+-10 min, thinned)
};
struct SpectrumView {
  int event = 0;
  int clip = -1;
  bool matched = false;
  double t0 = 0, t1 = 0, f_lo = 0, f_hi = 0;  // s from the recording's start, Hz
  double utc0 = NAN;                          // UTC at t = 0 on this view's clock (its clip's, when edited)
  int w = 0, h = 0;
  std::string pixels;  // base64 of w x h bytes (whitened level 0..255), row 0 at f_hi
  std::vector<std::vector<std::array<float, 2>>> model;     // per line: (t, f) along the matched aircraft's real path
  std::vector<std::vector<std::array<float, 2>>> straight;  // per line: the best straight pass (the measurement)
};

// An edited file (hum edit points): each clip has its own flyovers and its own clock,
// in an unknown order; the place is shared (the camera did not move).
struct ClipAir {
  int index = 0;                  // the hum segment
  double start_s = 0, end_s = 0;  // span in the file
  int event0 = 0, n_events = 0;   // its flyovers in the stage's detection (tc in file time)
  int hyp0 = 0, n_hyps = 0;       // its hypotheses in the stage's `tried` (each searched alone)
  int best = -1;                  // index into `tried` of its best search alone
  std::optional<SearchResult> at_place;  // searched again around the shared place
  int at_hyp = -1;                // the hypothesis that gave at_place
  double start_utc = NAN;         // the clip's first sample, heard at the shared place
  double alone_m = NAN;           // its own best place (searched alone) from the shared place
  std::string note;
};
struct SharedPlace {
  double lat = NAN, lon = NAN, ground_m = 0;
  AirLevel level = AirLevel::None;
  int anchor = -1;  // the clip whose answer chose the place
  int clips_matched = 0, clips_agreeing = 0;
  bool disagree = false;  // a clip located elsewhere on its own
  std::vector<std::string> reasons;
};

struct AircraftStage {
  Detection detection;
  std::vector<HypothesisResult> tried;
  int best = -1;  // index into tried of the best search
  std::vector<std::string> notes;
  std::vector<FlightView> flights;
  std::vector<SpectrumView> spectra;
  double start_utc = NAN;  // the best answer's start (spectrum times are relative to it)
  std::vector<ClipAir> clips;         // edited file only
  std::optional<SharedPlace> shared;  // edited file: the one place all clips are heard from
  const SearchResult* best_result() const { return best >= 0 ? &*tried[std::size_t(best)].result : nullptr; }
};

// Detects the flyovers of the file and searches each hypothesis that has ADS-B history.
AircraftStage run_aircraft(const std::string& path, const std::vector<Hypothesis>& hyps, const AircraftOptions& o);

// The same for an edited file: flyovers detected per clip (no template straddles a cut),
// each clip searched on its own clock (its hypotheses), then one place for all clips.
struct ClipSpan {
  int index = 0;
  double start_s = 0, end_s = 0;
  std::vector<Hypothesis> hyps;  // windows for the clip's first sample
};
AircraftStage run_aircraft_clips(const std::string& path, const std::vector<ClipSpan>& clips, const AircraftOptions& o);

// The search behind run_aircraft_clips, on spectrograms and tracks in hand (tests):
// each clip's hypotheses searched alone (where a box is known), the shared place chosen
// by the evidence of every clip near it, then each clip searched again around that place.
// Hypotheses carry their box; `unavailable(h, duration)` returns why one cannot be
// searched (e.g. ADS-B days missing), empty when it can.
struct ClipInput {
  const Spectrogram* spec = nullptr;
  std::vector<Event> events;  // tc from the clip's start
  std::vector<Hypothesis> hyps;
};
struct ClipOutcome {
  std::vector<HypothesisResult> tried;  // alone
  int best = -1;
  std::optional<SearchResult> at_place;
  int at_hyp = -1;
  double start_utc = NAN, alone_m = NAN;
  std::string note;
};
struct JointClips {
  std::vector<ClipOutcome> clips;
  std::optional<SharedPlace> shared;
};
using SpanTrackSource = std::function<std::vector<Track>(const Bbox&, double t0, double t1)>;
JointClips search_clips(const std::vector<ClipInput>& clips, const SpanTrackSource& tracks, Terrain& terrain,
                        const std::function<std::string(const Hypothesis&, double duration_s)>& unavailable = {},
                        const std::function<void(const std::string&)>& progress = {});
// The clips' results as one stage, in file time (clip spans from `spans`).
AircraftStage clips_stage(const std::vector<ClipSpan>& spans, const std::vector<ClipInput>& in, JointClips j);
inline constexpr double CLIP_AGREE_M = 1000.0;  // clips' places this close are one place
inline constexpr int CLIP_PLACES = 5;  // each clip's best answers tried as the shared place
inline constexpr double MIN_CLIP_AIR_S = 60.0;     // shorter clips are not searched for flyovers

json::object stage_json(const AircraftStage& s);
std::string render_stage(const AircraftStage& s);

}  // namespace geo::air
