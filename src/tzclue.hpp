// Time-zone clue: a file's metadata clock against the time the hum gives.
//
// The hum matches a recording to true UTC. Devices write a creation time into the file,
// and many write *local* time: phones state their offset outright (QuickTime's
// com.apple.quicktime.creationdate, XMP, Sony's NRT XML), and cameras, dashcams and
// audio recorders often store wall-clock time in fields meant for UTC, or in fields
// with no zone at all (BWF origination, RIFF ICRD, ID3, Vorbis DATE, Matroska tags).
// Either way the device's UTC offset at that moment follows, and only the parts of the
// matched grid whose zone had that offset then (DST included, from the system tz
// database) remain possible: data/tz_areas.json holds every grid area cut along zone
// borders (scripts/build_tz_areas.py).
#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "audio.hpp"
#include "util/time.hpp"

namespace geo::tzclue {

// Explicit: the stamp states its offset (+02:00; a "+00:00" counts, a bare "Z" does not).
// Utc: the format defines the field as UTC (MP4/MOV mvhd, Matroska DateUTC), or the
//      stamp ends in Z. Devices that keep local time put it there anyway.
// Local: no zone stated (BWF, RIFF, ID3, Vorbis, AVI IDIT ...): usually wall-clock time.
enum class Kind { Explicit, Utc, Local };
const char* kind_name(Kind k);

struct Stamp {
  std::string key, raw;
  Kind kind = Kind::Local;
  WallTime w;
};

// Lenient date-time parsing for metadata: ISO 8601 (T or space, optional fraction and
// offset), EXIF's "YYYY:MM:DD HH:MM:SS", C's "Mon Mar 10 15:04:43 2003". A time of day is
// required: a bare date cannot tell offsets apart.
std::optional<WallTime> parse_stamp(std::string_view s);
// Every usable time stamp in the file's tags, most specific first.
std::vector<Stamp> stamps(const audio::FileClues& c);

struct Evidence {
  std::string key, raw;
  Kind kind = Kind::Local;
  std::optional<int> offset_s;  // the device clock's UTC offset this stamp shows, if any
  double residual_s = NAN;      // |stamp - hum time| once the offset is applied (NaN: not compared)
  bool used = false;            // counted towards the area
  std::string note;
};

struct Zone {
  std::string tz;
  std::vector<std::string> countries, subdivisions;
  double km2 = 0;
};

struct Result {
  std::vector<Evidence> evidence;
  std::vector<int> offsets;  // offsets the area was narrowed with (several only when stamps disagree)
  std::optional<std::string> grid;
  double at = NAN;  // the instant the offsets were evaluated at (UTC epoch seconds)
  std::vector<Zone> zones;
  double km2 = 0, grid_km2 = 0;  // remaining area; the grid's whole area at that time
  std::vector<std::string> notes;
};

// start_utc: the hum's time for the recording's first sample (NaN when nothing matched);
// grid: the matched grid (nullopt: any grid, possible only with an explicit offset).
Result infer(const audio::FileClues& c, std::optional<std::string> grid, double start_utc, double duration_s);

// A zone's UTC offset in seconds at instant t (throws std::runtime_error for unknown zones).
int offset_at(const std::string& tz, double t);
std::string fmt_offset(int s);  // "UTC+02:00", "UTC-03:30", "UTC"

// The zones in data/tz_areas.json, for tests.
std::vector<std::string> known_zones();

}  // namespace geo::tzclue
