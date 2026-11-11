// miniSEED 2 reader: the records FDSN dataselect services return (EarthScope, ...).
//
// Each record is a 48-byte fixed header, blockettes (1000 gives the encoding and the
// record length, 1001 the microseconds), then the data: Steim1/Steim2 compressed
// integers (always big-endian) or plain int16/int32/float32/float64. Records of one
// channel that follow each other within half a sample are joined into a segment; a gap
// or overlap starts a new one.
#pragma once

#include <cstdint>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

namespace geo::mseed {

struct Segment {
  std::string id;       // NET.STA.LOC.CHA
  double t0 = 0;        // UTC seconds of the first sample
  double rate = 0;      // samples per second
  std::vector<float> x; // samples (counts)
  double end() const { return t0 + double(x.size()) / rate; }
};

struct Error : std::runtime_error {
  using std::runtime_error::runtime_error;
};

// Decodes every record in buf (concatenated records, any record lengths) and returns
// the segments, ordered by channel then time. Records of other lengths than blockette
// 1000 says, or without it, are read as 512 bytes when they parse; anything else throws.
std::vector<Segment> decode(std::span<const std::uint8_t> buf);

// Steim1 (level 1) or Steim2 (level 2) frames to n integers; x0 checked against the
// reverse integration constant (throws on a mismatch: a corrupt record).
std::vector<std::int32_t> steim(std::span<const std::uint8_t> frames, int level, std::size_t n);

}  // namespace geo::mseed
