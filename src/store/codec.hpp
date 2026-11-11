// Month-file codec of the reference store (format unchanged from the Python version).
//
//   <YYYY-MM>.u16z, little-endian:
//     magic "GEOZ" | version u8 (=1) | 3 reserved | n_chunks u32
//     n_chunks x (offset u32, length u32)      length 0 = no data that day
//     chunk payloads
//
// Chunk i is UTC day i of the month: zstd(byte_shuffle(zigzag(diff(codes)))) on uint16
// codes with wrap-around (lossless). code = round(dev_hz * 1e4) + 32768, 0 = missing.
#pragma once

#include <cstdint>
#include <filesystem>
#include <memory>
#include <span>
#include <string>
#include <vector>

#include "util/files.hpp"

namespace geo::codec {

inline constexpr double SCALE = 10000.0;
inline constexpr int OFFSET = 32768;
inline constexpr std::uint16_t MISSING = 0;
inline constexpr std::uint16_t CLIPPED_LO = 1, CLIPPED_HI = 65535;  // encode() clips out-of-range values to these
inline constexpr double MAX_ABS_HZ = (65535.0 - OFFSET) / SCALE;
inline constexpr int STORE_STEP_CODES = 10;  // stored step 1 mHz (docs/grids/compression.md)
inline constexpr std::int64_t CHUNK_S = 86400;
inline constexpr int ZSTD_LEVEL = 19;

std::uint16_t encode(double dev_hz);
inline float decode(std::uint16_t c) {
  return c == MISSING ? std::numeric_limits<float>::quiet_NaN() : (static_cast<float>(c) - 32768.0f) / 10000.0f;
}

// dst[k] = decode(codes[k]) where dst[k] is still NaN and the code is a value (not
// MISSING, not one of the two clipped extremes: junk rows older parsers let through).
void decode_into(std::span<const std::uint16_t> codes, std::span<float> dst);

// Compressed day chunk <-> codes. encode_chunk returns "" when every code is MISSING.
std::string encode_chunk(std::span<const std::uint16_t> codes);
// Decodes into out (size n); an empty blob means all MISSING.
void decode_chunk(std::span<const std::byte> blob, std::span<std::uint16_t> out);

// Read access to one month file through a memory map: decodes only the days asked for.
class MonthFile {
 public:
  MonthFile(const std::filesystem::path& p, std::int64_t t0, std::int64_t t1);
  std::int64_t t0() const { return t0_; }
  std::int64_t t1() const { return t1_; }
  std::size_t n_chunks() const { return index_.size(); }
  std::span<const std::byte> blob(std::size_t i) const;
  std::size_t chunk_len(std::size_t i) const;  // seconds in day i
  void chunk(std::size_t i, std::span<std::uint16_t> out) const;
  // Codes for UTC seconds [a, b) (inside this month).
  void codes(std::int64_t a, std::int64_t b, std::span<std::uint16_t> out) const;

 private:
  MappedFile map_;
  std::int64_t t0_, t1_;
  std::vector<std::pair<std::uint32_t, std::uint32_t>> index_;
};

void write_month_file(const std::filesystem::path& p, const std::vector<std::string>& blobs);

}  // namespace geo::codec
