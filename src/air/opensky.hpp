// OpenSky Network's historical database (Trino), as a second ADS-B source that checks
// the aircraft search's best answers.
//
// OpenSky has its own receiver network: an aircraft ADSB.lol did not see may be in it.
// The silence test only knows the aircraft in its data, so a place can look silent only
// because ADSB.lol missed a low aircraft passing it. After a search ranks its answers,
// the best few places (Options::places) are checked: OpenSky's state vectors within
// Options::radius_m of each, over the hours its matched flyovers span (plus a margin),
// are converted into the same trajectory nodes as an ADSB.lol day (adsb.cpp: altitude
// above the ellipsoid, spikes dropped, smoothed, Hermite nodes), jets dropped by the
// registry's type, and merged with ADSB.lol's segments by ICAO address (OpenSky only
// fills an aircraft's gaps, so a flight is never counted twice). The search is then
// run again at each place, with ADSB.lol alone and with the merged set; a place that
// no longer survives is dropped, one where another combination now wins is replaced.
//
// Access (opensky-network.org/data/trino): https://trino.opensky-network.org, catalog
// minio, schema osky, a bearer token from OpenSky's OpenID server (password grant,
// client trino-client; OPENSKY_USERNAME / OPENSKY_PASSWORD). Every query filters on the
// `hour` partition, one hour at a time, a box and a time window inside it, selects only
// the columns read, and runs alone (one at a time, process-wide). Results are cached on
// disk (<cache>/opensky/<key>.trk, the store's own format, keyed by the SQL) and, with
// the aircraft bucket configured, uploaded there (opensky/<key>.trk) so a server that
// cannot reach OpenSky reads the same answers. The same question is never asked twice.
//
// Data: The OpenSky Network, https://opensky-network.org (non-commercial research use).
// Schäfer, Strohmeier, Lenders, Martinovic, Wilhelm: "Bringing up OpenSky: A large-scale
// ADS-B sensor network for research", IPSN 2014.
#pragma once

#include <cstdint>
#include <functional>
#include <stdexcept>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include "air/adsb.hpp"
#include "air/search.hpp"
#include "util/json.hpp"

namespace geo::air::opensky {

inline constexpr double SAMPLE_S = 5.0;       // one state vector per aircraft every 5 s (time % 5 = 0)
inline constexpr double MAX_ALT_M = 12000.0;  // above: never heard
inline constexpr double MAX_SPEED = 200.0;    // m/s; faster is a jet
inline constexpr double SNAP_DEG = 0.1;       // boxes snapped outwards (more cache hits)
inline constexpr double SNAP_S = 600.0;       // windows snapped outwards, within the hour

// One query: one `hour` partition, a window inside it, a box.
struct Window {
  std::int64_t hour = 0;  // epoch s, a multiple of 3600
  std::int64_t t0 = 0, t1 = 0;
  Bbox box;
};
// [t0, t1] over `box`, one window per hour it touches, snapped outwards.
std::vector<Window> windows(double t0, double t1, const Bbox& box);
std::string sql(const Window& w, std::int64_t limit = 0);  // limit 0: a 2M-row safety net
std::string cache_key(const Window& w);  // hex: SHA-256 of the format version and the SQL

// Trino's rows (columns by name: time, icao24, lat, lon, velocity, heading, vertrate,
// baroaltitude, geoaltitude) -> one track per aircraft, raw fixes in time order.
// Altitude above the ellipsoid as an ADSB.lol day: GNSS altitude where reported; else
// barometric + the aircraft's own median GNSS-minus-barometric offset; else + the median
// of every aircraft in its 2 x 2 degree cell (the local pressure deviation); else raw.
// Types from the registry (AircraftDb); jets dropped unless keep_jets.
std::vector<Track> tracks_from_rows(const std::vector<std::string>& columns, const json::array& rows, bool keep_jets = false);
// The same in two steps, for results too large to hold as JSON: rows appended page by
// page as compact records, then grouped into tracks. The barometric fallback is the
// median offset of the aircraft's 2 x 2 degree cell.
struct Row {
  std::uint32_t icao = 0;
  double t = 0, lat = 0, lon = 0;
  float v = NAN, h = NAN, vr = NAN, baro = NAN, geo = NAN;
};
void append_rows(const std::vector<std::string>& columns, const json::array& rows, std::vector<Row>& out);
std::vector<Track> tracks_of_rows(std::vector<Row> rows, bool keep_jets = false);
// Runs one statement (one at a time, process-wide), streaming its rows. Needs credentials.
std::vector<Row> query_rows(const std::string& sql);

// The trajectory nodes an ADSB.lol day would store for these tracks (adsb.cpp).
std::vector<Track> to_nodes(const std::vector<Track>& tracks);

// The JWT's preferred_username (the Trino user must be the token's, not the login typed).
std::string token_user(std::string_view jwt);

// ADSB.lol's gap-free segments plus OpenSky's (nodes, any number per aircraft): an
// aircraft ADSB.lol has keeps its segments; OpenSky adds only where ADSB.lol has no
// segment within GAP_S of it (its own segments cut there).
inline constexpr double GAP_S = 60.0;
struct Merged {
  std::vector<Track> segments;           // ADSB.lol's, then OpenSky's additions
  std::size_t from_adsb = 0;             // segments[0, from_adsb) are ADSB.lol's
  std::set<std::uint32_t> only_opensky;  // aircraft ADSB.lol does not have here
  std::set<std::uint32_t> gap_filled;    // aircraft both have; OpenSky filled gaps
};
Merged merge(const std::vector<Track>& adsb_segments, const std::vector<Track>& opensky_tracks);

struct Options {
  bool enabled = false;
  int places = 5;            // best answers checked
  double radius_m = 30000;   // around each
  double margin_s = 600;     // before the first and after the last matched flyover's window
  bool push = true;          // upload new answers to the aircraft bucket (when configured)
  // Where a window's tracks come from (default: fetch). Tests set it: no network.
  std::function<std::vector<Track>(const Window&)> source;
};
// GEOLOC_OPENSKY (1 = on by default), GEOLOC_OPENSKY_PLACES, GEOLOC_OPENSKY_RADIUS_KM,
// GEOLOC_OPENSKY_PUSH (0 = never upload).
Options options_from_env();
bool has_credentials();  // OPENSKY_USERNAME and OPENSKY_PASSWORD set

// A window's non-jet tracks as nodes: the local cache, else the aircraft bucket, else
// Trino (needs credentials). `source` says which. Throws NotConfigured when the answer
// is not cached and there are no credentials, std::runtime_error when OpenSky fails.
struct NotConfigured : std::runtime_error {
  using std::runtime_error::runtime_error;
};
std::vector<Track> fetch(const Window& w, std::string* source = nullptr, bool push = true);

// What OpenSky changed at one checked place.
struct Aircraft {
  std::string flight, type, reg;
  bool only_opensky = false;  // else ADSB.lol has it, with gaps OpenSky filled
  double closest_m = NAN, closest_utc = NAN;  // closest slant range (reception time) during the matched flyovers' span
  bool unheard = false;  // within the silence range there, at a time no flyover was detected
};
struct PlaceCheck {
  int rank = 0;  // in the search's ranking, from 0
  double lat = 0, lon = 0, start_utc = 0, score = 0;
  std::string combo;
  std::string outcome;  // unchanged | dropped | changed (other flights win) | moved (same flights, elsewhere nearby) | not reproduced
  std::string detail;
  std::optional<Candidate> replacement;  // changed / moved: the answer that takes its place
  std::size_t opensky_segments = 0, adsb_segments = 0;
  std::vector<Aircraft> aircraft;  // OpenSky's additions within the silence range of the place
};
struct Report {
  std::string status;  // off | checked | not configured | unreachable
  std::string note;
  std::vector<PlaceCheck> places;
  std::vector<std::string> queries;  // per window: key and where it came from
  int dropped = 0, changed = 0;  // changed counts moved too
};

// Checks r's best places (see the header comment) and applies the outcome to r
// (dropped places removed, changed ones replaced, judged again); r.opensky holds the
// report. Without credentials or a cached answer, or when OpenSky cannot be reached,
// r is left exactly as it was and the report says why.
void check(SearchResult& r, const Spectrogram& spec, const std::vector<Event>& events, const SearchOptions& so, const TrackSource& adsb, Terrain& terrain,
           const Options& o);

// ---------------------------------------------------------------- whole days
//
// A day ADSB.lol does not have, built from OpenSky as an ADSB.lol day would be (the same
// store, adsb/<day>.trk, its coverage and index), so every search, the server and the
// history map read it unchanged; adsb/<day>.source says "opensky". Queried one hour at a
// time in latitude bands around the globe (one query at a time, a pause between them);
// a band that reaches the row limit is split and asked again, never stored truncated.
// Each band-hour is cached (<cache>/opensky/days/<day>/), so an interrupted day resumes.
struct DayOptions {
  bool force = false;        // build even when ADSB.lol (here or in the bucket) has the day
  bool push = false;         // upload the store, its coverage and its source to the aircraft bucket
  double band_deg = 15.0;    // latitude band per query
  std::int64_t limit = 1000000;  // rows per query; more: the band is split
  double pause_s = 2.0;      // between queries
};
struct DayStats {
  int queries = 0, cached = 0, splits = 0;
  std::size_t rows = 0, aircraft = 0, nodes = 0;
  std::uint64_t bytes = 0;
  double seconds = 0;
};
// The day's first queries: per hour, the bands from -90 to 90 (all longitudes).
std::vector<Window> day_windows(const std::string& day, double band_deg = 15.0);
fs::path source_path(const std::string& day);  // <cache>/adsb/<day>.source
std::string chunk_name(const Window& w);       // a band-hour's file under <cache>/opensky/days/<day>/
DayStats build_day(const std::string& day, const DayOptions& o = {}, const std::function<void(const std::string&)>& progress = {});

json::object report_json(const Report& rep);
std::string render_report(const Report& rep);

}  // namespace geo::air::opensky
