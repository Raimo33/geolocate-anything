#include "store/codec.hpp"

#include <immintrin.h>

#include <zstd.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <stdexcept>

namespace geo::codec {

namespace {
// One compression and one decompression context per thread, reused (the search decodes
// on several threads at once).
ZSTD_CCtx* cctx() {
  static thread_local std::unique_ptr<ZSTD_CCtx, decltype(&ZSTD_freeCCtx)> c(ZSTD_createCCtx(), ZSTD_freeCCtx);
  return c.get();
}
ZSTD_DCtx* dctx() {
  static thread_local std::unique_ptr<ZSTD_DCtx, decltype(&ZSTD_freeDCtx)> d(ZSTD_createDCtx(), ZSTD_freeDCtx);
  return d.get();
}
template <class T>
T rd(const std::byte* p) {
  T v;
  std::memcpy(&v, p, sizeof v);
  return v;
}
}  // namespace

std::uint16_t encode(double v) {
  if (!std::isfinite(v)) return MISSING;
  double c = std::nearbyint(std::clamp(v, -MAX_ABS_HZ, MAX_ABS_HZ) * SCALE) + OFFSET;
  return static_cast<std::uint16_t>(std::clamp(c, 1.0, 65535.0));
}

std::string encode_chunk(std::span<const std::uint16_t> c) {
  if (std::all_of(c.begin(), c.end(), [](auto x) { return x == MISSING; })) return {};
  const std::size_t n = c.size();
  std::vector<std::uint8_t> sh(2 * n);
  std::uint16_t prev = 0;
  for (std::size_t i = 0; i < n; ++i) {
    std::int32_t d = static_cast<std::int16_t>(static_cast<std::uint16_t>(c[i] - prev));
    prev = c[i];
    auto z = static_cast<std::uint16_t>((d << 1) ^ (d >> 31));
    sh[i] = static_cast<std::uint8_t>(z & 0xFF);
    sh[n + i] = static_cast<std::uint8_t>(z >> 8);
  }
  std::string out(ZSTD_compressBound(sh.size()), '\0');
  std::size_t r = ZSTD_compressCCtx(cctx(), out.data(), out.size(), sh.data(), sh.size(), ZSTD_LEVEL);
  if (ZSTD_isError(r)) throw std::runtime_error(std::string("zstd: ") + ZSTD_getErrorName(r));
  out.resize(r);
  return out;
}

void decode_chunk(std::span<const std::byte> blob, std::span<std::uint16_t> out) {
  const std::size_t n = out.size();
  if (blob.empty()) {
    std::fill(out.begin(), out.end(), MISSING);
    return;
  }
  static thread_local std::vector<std::uint8_t> raw;
  raw.resize(2 * n);
  std::size_t r = ZSTD_decompressDCtx(dctx(), raw.data(), raw.size(), blob.data(), blob.size());
  if (ZSTD_isError(r) || r != 2 * n) throw std::runtime_error("corrupt month chunk");
  // Un-shuffle, un-zigzag and the running sum (mod 2^16), 16 codes per step: an
  // in-lane log-step scan, the low lane's total carried into the high lane, and the
  // running carry advanced by the block total (one add on the loop-carried chain).
  const std::uint8_t* __restrict lo = raw.data();
  const std::uint8_t* __restrict hi = raw.data() + n;
  std::uint16_t* __restrict o = out.data();
  std::size_t i = 0;
  __m256i carry = _mm256_setzero_si256();
  const __m256i one = _mm256_set1_epi16(1);
  for (; i + 16 <= n; i += 16) {
    const __m256i l = _mm256_cvtepu8_epi16(_mm_loadu_si128(reinterpret_cast<const __m128i*>(lo + i)));
    const __m256i h = _mm256_cvtepu8_epi16(_mm_loadu_si128(reinterpret_cast<const __m128i*>(hi + i)));
    const __m256i z = _mm256_or_si256(l, _mm256_slli_epi16(h, 8));
    __m256i d = _mm256_xor_si256(_mm256_srli_epi16(z, 1), _mm256_sub_epi16(_mm256_setzero_si256(), _mm256_and_si256(z, one)));
    d = _mm256_add_epi16(d, _mm256_slli_si256(d, 2));
    d = _mm256_add_epi16(d, _mm256_slli_si256(d, 4));
    d = _mm256_add_epi16(d, _mm256_slli_si256(d, 8));
    // Each lane's last element in all its slots.
    const __m256i last = _mm256_unpackhi_epi64(_mm256_shufflehi_epi16(d, 0xFF), _mm256_shufflehi_epi16(d, 0xFF));
    d = _mm256_add_epi16(d, _mm256_permute2x128_si256(last, last, 0x08));  // low lane's total into the high lane
    const __m256i lastc = _mm256_unpackhi_epi64(_mm256_shufflehi_epi16(d, 0xFF), _mm256_shufflehi_epi16(d, 0xFF));
    const __m256i total = _mm256_permute2x128_si256(lastc, lastc, 0x11);   // block total, every slot
    _mm256_storeu_si256(reinterpret_cast<__m256i*>(o + i), _mm256_add_epi16(d, carry));
    carry = _mm256_add_epi16(carry, total);
  }
  auto acc = static_cast<std::uint16_t>(_mm256_extract_epi16(carry, 0));
  for (; i < n; ++i) {
    const auto z = static_cast<std::uint16_t>(lo[i] | (hi[i] << 8));
    o[i] = acc = static_cast<std::uint16_t>(acc + ((z >> 1) ^ (0u - (z & 1u))));
  }
}

void decode_into(std::span<const std::uint16_t> codes, std::span<float> dst) {
  const std::size_t n = std::min(codes.size(), dst.size());
  const std::uint16_t* __restrict c = codes.data();
  float* __restrict o = dst.data();
  std::size_t k = 0;
  // Eight codes per step; the same float arithmetic as decode() (exact division).
  const __m256i lo = _mm256_set1_epi32(CLIPPED_LO), hi = _mm256_set1_epi32(CLIPPED_HI), zero = _mm256_setzero_si256();
  const __m256 off = _mm256_set1_ps(32768.0f), scale = _mm256_set1_ps(10000.0f);
  for (; k + 8 <= n; k += 8) {
    const __m256i ci = _mm256_cvtepu16_epi32(_mm_loadu_si128(reinterpret_cast<const __m128i*>(c + k)));
    const __m256i junk = _mm256_or_si256(_mm256_or_si256(_mm256_cmpeq_epi32(ci, zero), _mm256_cmpeq_epi32(ci, lo)), _mm256_cmpeq_epi32(ci, hi));
    const __m256 d = _mm256_loadu_ps(o + k);
    const __m256 take = _mm256_andnot_ps(_mm256_castsi256_ps(junk), _mm256_cmp_ps(d, d, _CMP_UNORD_Q));
    const __m256 v = _mm256_div_ps(_mm256_sub_ps(_mm256_cvtepi32_ps(ci), off), scale);
    _mm256_storeu_ps(o + k, _mm256_blendv_ps(d, v, take));
  }
  for (; k < n; ++k)
    if (c[k] != MISSING && c[k] != CLIPPED_LO && c[k] != CLIPPED_HI && o[k] != o[k]) o[k] = decode(c[k]);
}

MonthFile::MonthFile(const std::filesystem::path& p, std::int64_t t0, std::int64_t t1) : map_(p), t0_(t0), t1_(t1) {
  auto b = map_.bytes();
  if (!map_.ok() || b.size() < 12 || std::memcmp(b.data(), "GEOZ", 4) != 0 || static_cast<std::uint8_t>(b[4]) != 1)
    throw std::runtime_error(p.string() + ": not a v1 month file");
  auto n = rd<std::uint32_t>(b.data() + 8);
  if (b.size() < 12 + std::size_t(n) * 8) throw std::runtime_error(p.string() + ": truncated index");
  index_.resize(n);
  for (std::uint32_t i = 0; i < n; ++i)
    index_[i] = {rd<std::uint32_t>(b.data() + 12 + 8 * i), rd<std::uint32_t>(b.data() + 16 + 8 * i)};
}

std::span<const std::byte> MonthFile::blob(std::size_t i) const {
  auto [off, ln] = index_[i];
  auto b = map_.bytes();
  if (std::size_t(off) + ln > b.size()) throw std::runtime_error("month chunk out of range");
  return b.subspan(off, ln);
}

std::size_t MonthFile::chunk_len(std::size_t i) const {
  return static_cast<std::size_t>(std::min<std::int64_t>(CHUNK_S, t1_ - t0_ - std::int64_t(i) * CHUNK_S));
}

void MonthFile::chunk(std::size_t i, std::span<std::uint16_t> out) const { decode_chunk(blob(i), out.first(chunk_len(i))); }

void MonthFile::codes(std::int64_t a, std::int64_t b, std::span<std::uint16_t> out) const {
  static thread_local std::vector<std::uint16_t> day;
  const std::int64_t i0 = (a - t0_) / CHUNK_S, i1 = (b - 1 - t0_) / CHUNK_S;
  for (std::int64_t i = i0; i <= i1; ++i) {
    const std::int64_t c0 = t0_ + i * CHUNK_S;
    const std::size_t len = chunk_len(static_cast<std::size_t>(i));
    const std::int64_t s = std::max(a, c0), e = std::min(b, c0 + std::int64_t(len));
    if (s >= e) continue;
    if (static_cast<std::size_t>(i) >= index_.size() || index_[i].second == 0) {
      std::fill(out.begin() + (s - a), out.begin() + (e - a), MISSING);
      continue;
    }
    if (s == c0 && e == c0 + std::int64_t(len)) {
      decode_chunk(blob(i), out.subspan(s - a, len));  // whole day: decode in place
    } else {
      day.resize(len);
      decode_chunk(blob(i), day);
      std::copy(day.begin() + (s - c0), day.begin() + (e - c0), out.begin() + (s - a));
    }
  }
}

void write_month_file(const std::filesystem::path& p, const std::vector<std::string>& blobs) {
  std::string head(12 + 8 * blobs.size(), '\0');
  std::memcpy(head.data(), "GEOZ", 4);
  head[4] = 1;
  auto n = static_cast<std::uint32_t>(blobs.size());
  std::memcpy(head.data() + 8, &n, 4);
  std::uint32_t off = static_cast<std::uint32_t>(head.size());
  for (std::size_t i = 0; i < blobs.size(); ++i) {
    auto ln = static_cast<std::uint32_t>(blobs[i].size());
    std::memcpy(head.data() + 12 + 8 * i, &off, 4);
    std::memcpy(head.data() + 16 + 8 * i, &ln, 4);
    off += ln;
  }
  std::vector<std::span<const std::byte>> parts;
  parts.emplace_back(reinterpret_cast<const std::byte*>(head.data()), head.size());
  for (auto& b : blobs) parts.emplace_back(reinterpret_cast<const std::byte*>(b.data()), b.size());
  atomic_write(p, parts);
}

}  // namespace geo::codec
