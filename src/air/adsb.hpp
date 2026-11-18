// Historical ADS-B tracks: ADSB.lol's daily archives, reduced to a compact store.
//
// ADSB.lol publishes every UTC day as a GitHub release of adsblol/globe_history_<year>
// (ODbL): one tar, split in ~2 GB parts, holding a gzipped readsb "trace_full" JSON per
// aircraft. A day is ~4 GB. `ingest_day` streams it (download or local parts) through a
// tar reader; each member's header is inflated first, and jets (by ICAO type
// designator, tar1090-db) are dropped before their bulk is parsed. What is kept is every
// airborne fix of every other aircraft (propeller, turboprop, helicopter, unknown type):
// time, position, altitude above the WGS84 ellipsoid, ground speed, track, vertical
// rate, reduced to the nodes of a smooth curve through them (adsb.cpp: a steady leg is a
// node every 100 s). Stored as one zstd file per day under <cache>/adsb/.
//
// Altitude: readsb's geometric altitude is GNSS height above the ellipsoid. Where a fix
// has only barometric (pressure) altitude, it is corrected by the aircraft's own median
// geometric-minus-barometric offset that day; failing that, by the median offset of all
// aircraft within the same 2 deg x 2 deg cell and 3 h (the local pressure deviation);
// failing that it stays raw, and is flagged.
#pragma once

#include <array>
#include <map>
#include <set>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "util/files.hpp"

namespace geo::air {

// ICAO Doc 8643 description code ("L2P"): kind of aircraft, engine count, engine type.
enum class EngineType : std::uint8_t { Unknown = 0, Piston, Turboprop, Jet, Electric, Rocket };
enum class AltSource : std::uint8_t { Geometric = 0, BaroTrack, BaroRegion, BaroRaw };

const char* engine_name(EngineType e);

struct TypeInfo {
  char kind = '?';  // L landplane, S seaplane, A amphibian, H helicopter, G gyrocopter, T tiltrotor
  int engines = 0;
  EngineType engine = EngineType::Unknown;
};

// The designator table, from tar1090-db (downloaded once, cached).
class TypeDb {
 public:
  static const TypeDb& get();               // loads or downloads; throws if neither works
  static TypeDb parse(std::string_view json);  // {"C172": {"desc": "L1P", ...}, ...}
  std::optional<TypeInfo> find(std::string_view designator) const;
  std::size_t size() const { return codes_.size(); }

 private:
  std::vector<std::pair<std::array<char, 4>, TypeInfo>> codes_;  // sorted
};

// Per-aircraft registry (tar1090-db's aircraft.csv: ICAO address -> registration, type
// designator), for traces whose own record lacks the type: about 40% of the non-jets
// kept would otherwise be "unknown", jets among them.
class AircraftDb {
 public:
  static const AircraftDb& get();  // loads or downloads; empty (with a warning) if neither works
  static AircraftDb parse(std::string_view csv);
  std::string type(std::uint32_t icao) const;  // "" when unknown
  std::size_t size() const { return rows_.size(); }

 private:
  std::vector<std::pair<std::uint32_t, std::array<char, 4>>> rows_;  // sorted
};

// Registry details for a few aircraft (one pass over the cached aircraft.csv.gz).
struct RegistryEntry {
  std::string reg, type, desc, year, owner;
};
std::map<std::uint32_t, RegistryEntry> registry_lookup(const std::set<std::uint32_t>& icaos);

struct Fix {
  double t = 0;           // epoch seconds
  double lat = 0, lon = 0;
  float alt_m = NAN;      // above the WGS84 ellipsoid
  float gs = NAN;         // ground speed, m/s
  float track = NAN;      // degrees true
  float vrate = NAN;      // m/s
  AltSource alt_src = AltSource::Geometric;
  bool leg_start = false;  // readsb: a new flight leg starts here
};

struct Track {
  std::uint32_t icao = 0;
  std::string type;  // ICAO designator, may be empty
  std::string reg;
  TypeInfo info;
  std::vector<Fix> fixes;  // airborne fixes, time order (from a store: curve nodes)
  std::string id() const;  // "a1dfeb"
};

struct Bbox {
  double west = -180, south = -90, east = 180, north = 90;
  bool contains(double lat, double lon) const { return lat >= south && lat <= north && lon >= west && lon <= east; }
  Bbox grown(double m) const;
};

// An area drawn on the map: a closed polygon of (lon, lat) vertices. Answers outside it go.
using Area = std::vector<std::array<double, 2>>;
bool area_contains(const Area& a, double lat, double lon);  // even-odd rule
Bbox area_box(const Area& a);
Area parse_area(const std::string& s);  // "LON,LAT;LON,LAT;..." (>= 3 vertices), throws otherwise

fs::path adsb_dir();
fs::path day_path(const std::string& day);  // <cache>/adsb/<YYYY-MM-DD>.trk
bool has_day(const std::string& day);
std::vector<std::string> stored_days();

// How much history covers each place: aircraft-minutes (distinct minutes an aircraft
// had a fix in the cell) per 0.5 deg cell. One sidecar per day (<day>.cov), written at
// ingest or computed on first use; `coverage_json` merges every stored day:
// {cell_deg, days: [...], cells: [[south, west, aircraft_minutes], ...],
//  regions: [{id, name, bbox?: [w, s, e, n], minutes: [per listed day]}, ...]}.
// Days whose store left the volume keep their sidecar, so the map shows every day the
// bucket holds.
inline constexpr double COVERAGE_DEG = 0.5;
std::string coverage_json();
fs::path coverage_path(const std::string& day);  // <cache>/adsb/<day>.cov
void ensure_coverage(const std::string& day);    // writes the sidecar if missing or stale

// A bounded store (a server's volume): GEOLOC_AIRPLANE_KEEP_DAYS > 0 keeps only that many
// days' stores, the ones used last (sidecars stay).
void mark_used(const std::string& day);
void evict_days();  // never a day pinned by a live DayPinScope (any thread)
// The day is stored here, or was just downloaded from the ADS-B bucket. Inside a
// DayPinScope the day is pinned first.
bool ensure_day(const std::string& day);

// While one lives on this thread, every day ensure_day() is asked for stays stored until
// it ends: a search must not evict a day it reads. A whole-day window needs three days
// (the window's, and its neighbours for the padding), more than a small
// GEOLOC_AIRPLANE_KEEP_DAYS, and evicting by last use dropped the window's own day,
// leaving only the tracks after midnight.
class DayPinScope {
 public:
  DayPinScope();
  ~DayPinScope();
  DayPinScope(const DayPinScope&) = delete;
  DayPinScope& operator=(const DayPinScope&) = delete;
  void pin(const std::string& day);

 private:
  DayPinScope* prev_;
  std::vector<std::string> days_;
};

struct IngestOptions {
  std::vector<std::string> parts;  // local tar parts in order; empty = download the release
  std::optional<Bbox> bbox;        // keep only fixes inside (default: the world)
  bool keep_jets = false;
  int threads = 0;
};
struct IngestStats {
  std::size_t members = 0, kept = 0, jets = 0, ground_only = 0, unknown_type = 0;
  std::size_t fixes = 0, baro_track = 0, baro_region = 0, baro_raw = 0;
  std::uint64_t bytes_in = 0, bytes_out = 0;
  std::string release;
};
// Builds the day's store file; returns what was read and kept.
IngestStats ingest_day(const std::string& day, const IngestOptions& o = {});

// Tracks with fixes in [t0, t1] (and the fix either side of it), from the stored days
// that overlap; only fixes inside `bbox` when given. Each aircraft's fixes are split
// into gap-free segments by `split_tracks`.
// With `speed` [lo, hi] (m/s), only aircraft with such a node there and then (node
// speeds: the curve between nodes can differ by a few m/s; leave a margin).
// Aircraft are picked with the day's chunk index (<day>.idx, built on first use), so
// only those with a node near the window inside the box are decompressed.
std::vector<Track> load_tracks(double t0, double t1, const std::optional<Bbox>& bbox = {}, const std::optional<std::pair<double, double>>& speed = {});

// The chunk index of a stored day: every aircraft's nodes in 10-minute slots, each with
// its box and speed range. A sidecar (<day>.idx) built from the store alone, so it is
// added to existing stores without fetching them again; rebuilt when the store changes.
fs::path index_path(const std::string& day);
struct IndexStats {
  std::size_t aircraft = 0, chunks = 0;
  std::uint64_t bytes = 0, store_bytes = 0;
  double seconds = 0;  // reading it, or building and writing it
};
IndexStats ensure_index(const std::string& day, bool force = false);

// Gap-free pieces: a new piece starts at a readsb leg start or after more than
// `max_gap_s` without a fix.
std::vector<Track> split_tracks(const std::vector<Track>& tracks, double max_gap_s = 120.0);

// Binary (de)serialisation of a day's tracks (zstd-compressed), exposed for tests.
std::string encode_tracks(const std::vector<Track>& tracks);
std::vector<Track> decode_tracks(std::string_view bytes);
// One readsb trace_full JSON -> track (nullopt when it holds no airborne fix or is a
// jet and jets are not kept). Altitudes are not yet corrected beyond the geometric ones.
std::optional<Track> parse_trace(std::string_view json, const TypeDb& types, bool keep_jets, const std::optional<Bbox>& bbox);

}  // namespace geo::air
