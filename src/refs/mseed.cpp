#include "refs/mseed.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <map>

#include "util/time.hpp"

namespace geo::mseed {

namespace {

struct Reader {
  std::span<const std::uint8_t> b;
  bool be;
  std::uint16_t u16(std::size_t o) const {
    return be ? std::uint16_t(b[o] << 8 | b[o + 1]) : std::uint16_t(b[o + 1] << 8 | b[o]);
  }
  std::int16_t i16(std::size_t o) const { return std::int16_t(u16(o)); }
  std::uint32_t u32(std::size_t o) const {
    return be ? std::uint32_t(b[o]) << 24 | std::uint32_t(b[o + 1]) << 16 | std::uint32_t(b[o + 2]) << 8 | b[o + 3]
              : std::uint32_t(b[o + 3]) << 24 | std::uint32_t(b[o + 2]) << 16 | std::uint32_t(b[o + 1]) << 8 | b[o];
  }
  std::int32_t i32(std::size_t o) const { return std::int32_t(u32(o)); }
};

std::uint32_t be32(const std::uint8_t* p) { return std::uint32_t(p[0]) << 24 | std::uint32_t(p[1]) << 16 | std::uint32_t(p[2]) << 8 | p[3]; }

// Sign-extend the low `bits` of v.
std::int32_t sx(std::uint32_t v, int bits) {
  const std::uint32_t m = 1u << (bits - 1);
  v &= (bits == 32 ? 0xFFFFFFFFu : (1u << bits) - 1);
  return std::int32_t((v ^ m) - m);
}

std::string trim(const std::uint8_t* p, std::size_t n) {
  std::string s(reinterpret_cast<const char*>(p), n);
  while (!s.empty() && (s.back() == ' ' || s.back() == '\0')) s.pop_back();
  std::size_t i = 0;
  while (i < s.size() && s[i] == ' ') ++i;
  return s.substr(i);
}

double rate_of(std::int16_t f, std::int16_t m) {
  if (f == 0 || m == 0) return 0;
  double F = f, M = m;
  if (f > 0 && m > 0) return F * M;
  if (f > 0 && m < 0) return -F / M;
  if (f < 0 && m > 0) return -M / F;
  return 1.0 / (F * M);
}

}  // namespace

std::vector<std::int32_t> steim(std::span<const std::uint8_t> frames, int level, std::size_t n) {
  std::vector<std::int32_t> d;
  d.reserve(n + 8);
  const std::size_t nf = frames.size() / 64;
  std::int32_t x0 = 0, xn = 0;
  for (std::size_t f = 0; f < nf && d.size() < n + 1; ++f) {
    const std::uint8_t* fr = frames.data() + f * 64;
    const std::uint32_t nib = be32(fr);
    for (int w = 1; w < 16; ++w) {
      const std::uint32_t v = be32(fr + 4 * w);
      const int c = int(nib >> (30 - 2 * w)) & 3;
      if (f == 0 && w == 1) { x0 = std::int32_t(v); continue; }
      if (f == 0 && w == 2) { xn = std::int32_t(v); continue; }
      if (c == 0) continue;
      if (c == 1) {
        for (int k = 0; k < 4; ++k) d.push_back(sx(v >> (24 - 8 * k), 8));
      } else if (level == 1) {
        if (c == 2) {
          d.push_back(sx(v >> 16, 16));
          d.push_back(sx(v, 16));
        } else {
          d.push_back(std::int32_t(v));
        }
      } else {
        const int dn = int(v >> 30);
        if (c == 2) {
          if (dn == 1) d.push_back(sx(v, 30));
          else if (dn == 2) for (int k = 0; k < 2; ++k) d.push_back(sx(v >> (15 - 15 * k), 15));
          else if (dn == 3) for (int k = 0; k < 3; ++k) d.push_back(sx(v >> (20 - 10 * k), 10));
          else throw Error("Steim2: bad dnib 0 for nibble 2");
        } else {
          if (dn == 0) for (int k = 0; k < 5; ++k) d.push_back(sx(v >> (24 - 6 * k), 6));
          else if (dn == 1) for (int k = 0; k < 6; ++k) d.push_back(sx(v >> (25 - 5 * k), 5));
          else if (dn == 2) for (int k = 0; k < 7; ++k) d.push_back(sx(v >> (24 - 4 * k), 4));
          else throw Error("Steim2: bad dnib 3 for nibble 3");
        }
      }
    }
  }
  if (d.size() < n) throw Error("Steim: " + std::to_string(d.size()) + " differences for " + std::to_string(n) + " samples");
  std::vector<std::int32_t> x(n);
  if (n == 0) return x;
  x[0] = x0;
  for (std::size_t i = 1; i < n; ++i) x[i] = std::int32_t(std::uint32_t(x[i - 1]) + std::uint32_t(d[i]));  // wraps like the encoder
  if (x[n - 1] != xn) throw Error("Steim: last sample " + std::to_string(x[n - 1]) + " != reverse constant " + std::to_string(xn));
  return x;
}

std::vector<Segment> decode(std::span<const std::uint8_t> buf) {
  struct Rec {
    double t0, rate;
    std::vector<float> x;
  };
  std::map<std::string, std::vector<Rec>> by;
  std::size_t off = 0, corrupt = 0;
  while (off + 48 <= buf.size()) {
    const std::size_t corrupt_before = corrupt;
    auto b = buf.subspan(off);
    // Header sanity: sequence digits/spaces and a quality letter; otherwise skip 512 (noise
    // between records is not expected from FDSN services, so throw if nothing parses).
    const char q = char(b[6]);
    if (!(q == 'D' || q == 'R' || q == 'Q' || q == 'M')) throw Error("miniSEED: no record header at byte " + std::to_string(off));
    // Byte order from the year (1900..2100 reads right one way only).
    Reader r{b, true};
    if (r.u16(20) < 1900 || r.u16(20) > 2100) r.be = false;
    const int year = r.u16(20), doy = r.u16(22), hh = b[24], mi = b[25], ss = b[26];
    const int frac = r.u16(28);  // 0.0001 s
    const std::size_t ns = r.u16(30);
    const double rate = rate_of(r.i16(32), r.i16(34));
    const std::uint8_t act = b[36];
    const int nblk = b[39];
    const std::int32_t tcorr = r.i32(40);
    const std::size_t doff = r.u16(44);
    std::size_t boff = r.u16(46);
    int enc = -1, reclen_exp = 0, usec = 0;
    bool data_be = r.be;
    for (int k = 0; k < nblk && boff >= 48 && boff + 4 <= b.size(); ++k) {
      const int type = r.u16(boff);
      const std::size_t next = r.u16(boff + 2);
      if (type == 1000 && boff + 8 <= b.size()) {
        enc = b[boff + 4];
        data_be = b[boff + 5] == 1;
        reclen_exp = b[boff + 6];
      } else if (type == 1001 && boff + 8 <= b.size()) {
        usec = std::int8_t(b[boff + 5]);
      }
      if (next == 0) break;
      boff = next;
    }
    const std::size_t reclen = reclen_exp ? std::size_t(1) << reclen_exp : 512;
    if (reclen < 128 || off + reclen > buf.size()) throw Error("miniSEED: record at byte " + std::to_string(off) + " runs past the data");
    if (enc < 0) throw Error("miniSEED: record without blockette 1000");
    const std::string id = trim(b.data() + 18, 2) + "." + trim(b.data() + 8, 5) + "." + trim(b.data() + 13, 2) + "." + trim(b.data() + 15, 3);
    if (ns > 0 && rate > 0) {
      if (doff < 48 || doff >= reclen) throw Error("miniSEED: bad data offset in " + id);
      auto data = b.subspan(doff, reclen - doff);
      std::vector<float> x(ns);
      Reader dr{data, data_be};
      switch (enc) {
        case 1:
          if (ns * 2 > data.size()) throw Error("miniSEED: short int16 record");
          for (std::size_t i = 0; i < ns; ++i) x[i] = float(dr.i16(2 * i));
          break;
        case 3:
          if (ns * 4 > data.size()) throw Error("miniSEED: short int32 record");
          for (std::size_t i = 0; i < ns; ++i) x[i] = float(dr.i32(4 * i));
          break;
        case 4:
          if (ns * 4 > data.size()) throw Error("miniSEED: short float32 record");
          for (std::size_t i = 0; i < ns; ++i) {
            std::uint32_t u = dr.u32(4 * i);
            float f;
            std::memcpy(&f, &u, 4);
            x[i] = f;
          }
          break;
        case 5:
          if (ns * 8 > data.size()) throw Error("miniSEED: short float64 record");
          for (std::size_t i = 0; i < ns; ++i) {
            std::uint64_t u = (std::uint64_t(dr.u32(8 * i)) << 32) | dr.u32(8 * i + 4);
            if (!data_be) u = (std::uint64_t(dr.u32(8 * i + 4)) << 32) | dr.u32(8 * i);
            double f;
            std::memcpy(&f, &u, 8);
            x[i] = float(f);
          }
          break;
        case 10:
        case 11: {
          // A record that fails Steim's own integrity check (its last sample is not the
          // reverse integration constant) has corrupt differences: it is dropped, leaving
          // a gap of a few seconds, rather than failing the whole request. Raspberry Shake
          // archives hold an occasional one (Tianjin 2019: 1 record in ~9,000).
          std::vector<std::int32_t> v;
          try {
            v = steim(data, enc == 10 ? 1 : 2, ns);
          } catch (const Error&) {
            ++corrupt;
            break;
          }
          for (std::size_t i = 0; i < ns; ++i) x[i] = float(v[i]);
          break;
        }
        default: throw Error("miniSEED: unsupported encoding " + std::to_string(enc) + " in " + id);
      }
      if (corrupt > corrupt_before) {
        off += reclen;
        continue;
      }
      double t = double(days_from_civil(year, 1, 1) + doy - 1) * 86400.0 + hh * 3600.0 + mi * 60.0 + ss + frac * 1e-4 + usec * 1e-6;
      if (!(act & 0x02)) t += tcorr * 1e-4;  // correction not yet applied
      by[id].push_back({t, rate, std::move(x)});
    }
    off += reclen;
  }
  std::vector<Segment> out;
  for (auto& [id, recs] : by) {
    std::stable_sort(recs.begin(), recs.end(), [](const Rec& a, const Rec& b) { return a.t0 < b.t0; });
    for (auto& rc : recs) {
      if (!out.empty() && out.back().id == id && out.back().rate == rc.rate &&
          std::fabs(rc.t0 - out.back().end()) < 0.5 / rc.rate) {
        auto& s = out.back();
        s.x.insert(s.x.end(), rc.x.begin(), rc.x.end());
      } else {
        out.push_back({id, rc.t0, rc.rate, std::move(rc.x)});
      }
    }
  }
  return out;
}

}  // namespace geo::mseed
