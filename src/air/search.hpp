// Where was the listener? Matching measured flyovers against historical flight tracks.
//
// 1. Corridors. For each event, every flight segment flying within the event's speed
//    range during the time window is cut into short pieces; every 200 m cell whose
//    closest approach to the segment falls inside the piece and at a slant range in the
//    event's range gets an entry: (segment, slant range, reception time of the CPA).
// 2. One clock. Each entry implies an interval for the recording's start
//    (CPA reception time - the event's CPA position in the recording). A cell survives if
//    entries for at least `min_events` events share one start interval: two flyovers 921 s
//    apart must be heard 921 s apart.
// 3. Silence. A cell that would have heard another aircraft pass closer than the events'
//    own slant ranges between its first and last matched event, unheard, is dropped.
// 4. Exact curves. At each survivor (and a finer sub-grid) the Doppler curve of each
//    matched segment is modelled from the real trajectory (emission-time solved, not a
//    constant delay) and scored against the whitened spectrogram, sweeping each event's
//    rest frequency; the start time is swept once for all events (one clock, so a
//    second event cannot slide to fit on its own).
// 5. Ranking. Best point per combination of flights, then a fine local refinement of the
//    leaders. The fit ratio (trajectory score / straight-pass score of the same event)
//    and the margin over the next combination give the verdict.
#pragma once

#include <functional>
#include <optional>
#include <string>
#include <vector>

#include "air/adsb.hpp"
#include "air/bladepass.hpp"
#include "air/doppler.hpp"
#include "air/events.hpp"
#include "air/terrain.hpp"
#include "util/json.hpp"

namespace geo::air {

struct SearchOptions {
  double start_min = NAN, start_max = NAN;  // window for the recording's first sample (epoch s)
  Bbox box;                                 // search envelope
  std::function<bool(double lat, double lon)> inside;  // optional finer area test (a grid's polygon)
  double cell_m = 200.0;                    // corridor grid
  double fine_m = 100.0;                    // scoring sub-grid
  double refine_m = 20.0;                   // final refinement around the leaders
  double c = SOUND_SPEED;
  double tc_tol_s = 4.0;                    // CPA timing tolerance beyond the event's own range
  double v_margin = 12.0;                   // m/s, on the event's speed range (wind, ADS-B)
  double d_factor = 1.25;                   // on the event's slant range bounds
  int min_events = 2;                       // events a location must explain
  int max_events = 6;                       // strongest events considered
  bool silence = true;
  bool silence_whole = false;               // over the whole recording (else between matched flyovers)
  double silence_factor = 1.0;              // unheard passes closer than this x max event slant range reject
  bool unknown_types = true;                // include aircraft of unknown type
  // Rest-frequency prior (bladepass.hpp): a segment is a candidate for an event only if
  // its type can make the event's tone; types outside the table always can.
  bool type_prior = true;
  PriorOptions prior;
  const BladePassTable* blade_pass = nullptr;  // nullptr: the built-in table + learned observations
  double same_aircraft_f0_tol = 0.05;          // one aircraft explaining two events: tones agree (or a simple ratio)
  double mic_height = 2.0;
  int top_k = 20;
  int max_scored = 1500;                    // survivors fully fitted at the cell centre (best by a quick fit)
  int max_subgrid = 300;                    // of those, scored on the sub-grid
  int max_refine = 5;                       // best flight combinations refined on the fine grid
  double score_floor = -INFINITY;           // survivors whose full fit scores below this are dropped
  double window_factor = 1.0;               // scoring window: the event's half window times this,
  double max_window_s = 60.0;               //   at most this
  bool pair_first = true;                   // >= 2 flyovers: rasterize only where pieces of enough events can meet (same result)
  bool segment_bounds = true;               // skip segments whose speed or extent bounds rule them out (same result)
};

struct Match {
  int event = 0;
  std::string flight;  // icao hex
  std::string type, reg;
  double cpa_m = 0, speed = 0;
  double score = 0;      // curve score of this event at this point
  double fit = 0;        // score / the event's straight-pass score
  double f0 = 0;         // fitted rest frequency of the strongest line
  double cpa_utc = 0;    // reception time of the CPA
};

struct Candidate {
  double lat = 0, lon = 0, ground_m = 0;
  double start_utc = 0;  // fitted time of the recording's first sample
  double score = 0;      // sum of the matched events' scores
  std::vector<Match> matches;
  std::string combo;     // flights per event, the grouping key
};

enum class AirLevel { None, Weak, Moderate, Strong };
// One flyover alone reaches moderate only with a near-straight-pass fit: speech and
// room noise make single poor-fit "flyovers" that some flight in a busy window explains
// (a council meeting matched to a helicopter 664 km away at fit 0.61).
inline constexpr double SINGLE_FLYOVER_MODERATE_FIT = 0.8;
const char* air_level_name(AirLevel l);

struct SearchResult {
  std::vector<Candidate> candidates;  // best per flight combination, best first
  AirLevel level = AirLevel::None;
  std::vector<std::string> reasons;
  std::vector<int> events_used;
  // Funnel counts.
  std::size_t segments = 0, corridor_cells = 0, consistent_cells = 0, silent_cells = 0, scored = 0;
  // Rest-frequency prior: (segment, event) pairs passing the speed-independent checks
  // (eligible type), those whose type cannot make the event's tone, and segments
  // dropped for every event.
  std::size_t type_pairs = 0, type_pruned_pairs = 0, type_pruned_segments = 0;
  bool terrain_complete = true;
  double seconds = 0;
  std::size_t pair_pieces = 0, pair_kept = 0;  // pair-first: pieces within reach of a tile, and those rasterized (summed over tiles)
  double cpu_corridors = 0, cpu_centre = 0, cpu_pairs = 0;  // thread-seconds in the tile pass (pairs: part of corridors)
  double wall_tiles = 0, wall_subgrid = 0, wall_refine = 0;
  double cpu_silence = 0;                // part of cpu_corridors
  double wall_load = 0, wall_pieces = 0;  // loading tracks (search_regions), cutting them into pieces
};

SearchResult search(const Spectrogram& spec, const std::vector<Event>& events, const std::vector<Track>& segments, Terrain& terrain,
                    const SearchOptions& o);

// The same over a large box and a long start window, one region and one time slice at a
// time (bounded memory): tracks_for(box, t0, t1) returns the gap-free segments with fixes
// inside the box between t0 and t1; candidates are merged and judged together. Start
// windows longer than SEARCH_SLICE_S are cut into slices of that length, and a slice whose
// tracks exceed SEARCH_MAX_SEGMENTS is halved (down to SEARCH_MIN_SLICE_S), so memory
// follows the traffic of a slice, not the window (a whole AirVenture day in Wisconsin
// needed >3.2 GB at once). Tracks are loaded only around the flyovers' times.
inline constexpr double SEARCH_SLICE_S = 4 * 3600.0;
inline constexpr double SEARCH_MIN_SLICE_S = 600.0;
inline constexpr std::size_t SEARCH_MAX_SEGMENTS = 400;
using TrackSource = std::function<std::vector<Track>(const Bbox&, double t0, double t1)>;
SearchResult search_regions(const Spectrogram& spec, const std::vector<Event>& events, const TrackSource& tracks_for, Terrain& terrain,
                            const SearchOptions& o, double region_deg = 3.0, const std::function<void(const std::string&)>& progress = {});
void judge(SearchResult& r);

// A track's fixes as an ECEF Hermite path (empty with fewer than two usable fixes).
Path path_of(const Track& tr);

// Bounds holding every point of the path's curve and its speed, from its nodes alone
// (the search skips segments they rule out).
struct PathBounds {
  Bbox box;
  double vlo = 0, vhi = INFINITY;
};
PathBounds path_bounds(const Path& path);

// Modelled curve score of one event for a listener at `o` (ECEF) and recording start
// `start`: sweeps the rest frequency by +-sweep; returns {score, f0 of the strongest line}.
// `ridge`: scored on ridges (as the search's final pass), else on brightness.
std::pair<double, double> event_score(const Spectrogram& spec, const Event& e, const Path& path, const Vec3& o, double start, double c,
                                      double sweep = 0.02, bool ridge = false);

json::object result_json(const SearchResult& r, const std::vector<Event>& events);
std::string render_result(const SearchResult& r, const std::vector<Event>& events);

}  // namespace geo::air
