#include "air/adsb.hpp"

#include <zlib.h>
#include <zstd.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <fstream>
#include <iostream>
#include <map>
#include <mutex>
#include <sstream>
#include <thread>
#include <tuple>
#include <unordered_map>

#include "air/bucket.hpp"
#include "air/geodesy.hpp"
#include "net/http.hpp"
#include "store/store.hpp"
#include "util/json.hpp"
#include "util/parallel.hpp"
#include "util/time.hpp"

namespace geo::air {

namespace {

// v3: trajectory nodes (older stores are not read: rebuild them).
constexpr char MAGIC[8] = {'G', 'L', 'A', 'D', 'S', 'B', '0', '3'};
constexpr int STORE_LEVEL = 19;  // zstd level of stored blobs (decoding speed does not depend on it)
constexpr const char* TYPES_URL = "https://raw.githubusercontent.com/wiedehopf/tar1090-db/master/db/icao_aircraft_types.js";
constexpr const char* AIRCRAFT_URL = "https://raw.githubusercontent.com/wiedehopf/tar1090-db/csv/aircraft.csv.gz";

// ---------------------------------------------------------------- gzip

// Inflates a gzip member; stops once `limit` bytes are out (0 = all).
std::string gunzip(std::string_view in, std::size_t limit = 0) {
  z_stream z{};
  if (inflateInit2(&z, 16 + MAX_WBITS) != Z_OK) throw std::runtime_error("inflateInit2 failed");
  std::string out;
  out.resize(limit ? limit : std::max<std::size_t>(in.size() * 8, 4096));
  z.next_in = reinterpret_cast<Bytef*>(const_cast<char*>(in.data()));
  z.avail_in = uInt(in.size());
  std::size_t have = 0;
  for (;;) {
    z.next_out = reinterpret_cast<Bytef*>(out.data() + have);
    z.avail_out = uInt(out.size() - have);
    const int r = inflate(&z, Z_NO_FLUSH);
    have = out.size() - z.avail_out;
    if (r == Z_STREAM_END) break;
    if (r != Z_OK && r != Z_BUF_ERROR) {
      inflateEnd(&z);
      throw std::runtime_error("corrupt gzip member");
    }
    if (limit && have >= limit) break;
    if (z.avail_out == 0) out.resize(out.size() * 2);
    else if (z.avail_in == 0) break;  // truncated
  }
  inflateEnd(&z);
  out.resize(have);
  return out;
}

// ---------------------------------------------------------------- tar

// Streaming ustar/GNU tar reader: feed() any chunking; calls on_member(name, bytes) for
// each regular file whose name passes want().
class TarStream {
 public:
  std::function<bool(const std::string&)> want;
  std::function<void(std::string name, std::string data)> on_member;

  void feed(std::string_view b) {
    while (!b.empty()) {
      if (state_ == Header) {
        const std::size_t take = std::min(b.size(), 512 - hdr_.size());
        hdr_.append(b.substr(0, take));
        b.remove_prefix(take);
        if (hdr_.size() == 512) start_member();
      } else if (state_ == Data) {
        const std::size_t take = std::size_t(std::min<std::uint64_t>(b.size(), left_));
        if (keep_) data_.append(b.substr(0, take));
        b.remove_prefix(take);
        left_ -= take;
        if (!left_) end_member();
      } else {  // padding to the next 512-byte boundary
        const std::size_t take = std::size_t(std::min<std::uint64_t>(b.size(), pad_));
        b.remove_prefix(take);
        pad_ -= take;
        if (!pad_) state_ = Header;
      }
    }
  }
  bool ended() const { return zero_blocks_ >= 1; }

 private:
  enum State { Header, Data, Pad } state_ = Header;
  std::string hdr_, data_, name_, long_name_;
  std::uint64_t left_ = 0, size_ = 0, pad_ = 0;
  char type_ = 0;
  bool keep_ = false;
  int zero_blocks_ = 0;

  static std::uint64_t octal(std::string_view f) {
    if (!f.empty() && (static_cast<unsigned char>(f[0]) & 0x80)) {  // base-256
      std::uint64_t v = 0;
      for (std::size_t i = 1; i < f.size(); ++i) v = (v << 8) | static_cast<unsigned char>(f[i]);
      return v;
    }
    std::uint64_t v = 0;
    for (char c : f)
      if (c >= '0' && c <= '7') v = v * 8 + std::uint64_t(c - '0');
    return v;
  }
  static std::string cstr(std::string_view f) { return std::string(f.substr(0, std::min(f.size(), f.find('\0')))); }

  void start_member() {
    if (std::all_of(hdr_.begin(), hdr_.end(), [](char c) { return c == 0; })) {
      ++zero_blocks_;
      hdr_.clear();
      return;
    }
    std::string_view h(hdr_);
    type_ = h[156];
    size_ = octal(h.substr(124, 12));
    name_ = cstr(h.substr(0, 100));
    const std::string prefix = cstr(h.substr(345, 155));
    if (h.substr(257, 5) == "ustar" && !prefix.empty()) name_ = prefix + "/" + name_;
    if (!long_name_.empty() && type_ != 'L') name_ = std::move(long_name_), long_name_.clear();
    keep_ = type_ == 'L' || ((type_ == '0' || type_ == 0) && (!want || want(name_)));
    data_.clear();
    if (keep_) data_.reserve(std::size_t(size_));
    left_ = size_;
    pad_ = (512 - size_ % 512) % 512;
    hdr_.clear();
    if (left_) state_ = Data;
    else end_member();
  }
  void end_member() {
    if (type_ == 'L') long_name_ = cstr(data_);
    else if (keep_ && on_member) on_member(std::move(name_), std::move(data_));
    data_.clear();
    state_ = pad_ ? Pad : Header;
  }
};

// ---------------------------------------------------------------- columnar blobs

template <class T>
void put(std::string& s, const T& v) {
  s.append(reinterpret_cast<const char*>(&v), sizeof v);
}
template <class T>
T get(std::string_view& s) {
  if (s.size() < sizeof(T)) throw std::runtime_error("truncated track store");
  T v;
  std::memcpy(&v, s.data(), sizeof v);
  s.remove_prefix(sizeof v);
  return v;
}

struct IndexEntry {
  std::uint32_t icao = 0;
  char type[4] = {};
  char reg[8] = {};
  char kind = '?';
  std::uint8_t engines = 0, engine = 0, pad = 0;
  float west = 0, south = 0, east = 0, north = 0;
  double t0 = 0, t1 = 0;
  std::uint64_t offset = 0;
  std::uint32_t clen = 0, nfix = 0;
};
static_assert(sizeof(IndexEntry) == 72);

// Ingest's scratch spill (every fix, fixed-width columns; never stored).
std::string fixes_blob(const Track& tr) {
  const std::size_t n = tr.fixes.size();
  std::string raw;
  raw.reserve(n * 29);
  const double t0 = n ? tr.fixes[0].t : 0;
  for (auto& f : tr.fixes) put(raw, std::int32_t(std::lround((f.t - t0) * 1000.0)));
  for (auto& f : tr.fixes) put(raw, std::int32_t(std::lround(f.lat * 1e6)));
  for (auto& f : tr.fixes) put(raw, std::int32_t(std::lround(f.lon * 1e6)));
  for (auto& f : tr.fixes) put(raw, f.alt_m);
  for (auto& f : tr.fixes) put(raw, f.gs);
  for (auto& f : tr.fixes) put(raw, f.track);
  for (auto& f : tr.fixes) put(raw, f.vrate);
  for (auto& f : tr.fixes) put(raw, std::uint8_t(std::uint8_t(f.alt_src) | (f.leg_start ? 0x80 : 0)));
  return raw;
}

std::vector<Fix> fixes_of(std::string_view raw, std::size_t n, double t0) {
  std::vector<Fix> fx(n);
  for (auto& f : fx) f.t = t0 + get<std::int32_t>(raw) / 1000.0;
  for (auto& f : fx) f.lat = get<std::int32_t>(raw) / 1e6;
  for (auto& f : fx) f.lon = get<std::int32_t>(raw) / 1e6;
  for (auto& f : fx) f.alt_m = get<float>(raw);
  for (auto& f : fx) f.gs = get<float>(raw);
  for (auto& f : fx) f.track = get<float>(raw);
  for (auto& f : fx) f.vrate = get<float>(raw);
  for (auto& f : fx) {
    const auto b = get<std::uint8_t>(raw);
    f.alt_src = AltSource(b & 0x7f), f.leg_start = b & 0x80;
  }
  return fx;
}

IndexEntry index_of(const Track& tr) {
  IndexEntry e;
  e.icao = tr.icao;
  std::memcpy(e.type, tr.type.data(), std::min<std::size_t>(4, tr.type.size()));
  std::memcpy(e.reg, tr.reg.data(), std::min<std::size_t>(8, tr.reg.size()));
  e.kind = tr.info.kind, e.engines = std::uint8_t(tr.info.engines), e.engine = std::uint8_t(tr.info.engine);
  e.west = 180, e.south = 90, e.east = -180, e.north = -90;
  for (auto& f : tr.fixes) {
    e.west = std::min(e.west, float(f.lon)), e.east = std::max(e.east, float(f.lon));
    e.south = std::min(e.south, float(f.lat)), e.north = std::max(e.north, float(f.lat));
  }
  e.t0 = tr.fixes.front().t, e.t1 = tr.fixes.back().t;
  e.nfix = std::uint32_t(tr.fixes.size());
  return e;
}

Track track_of(const IndexEntry& e) {
  Track tr;
  tr.icao = e.icao;
  tr.type = std::string(e.type, strnlen(e.type, 4));
  tr.reg = std::string(e.reg, strnlen(e.reg, 8));
  tr.info = {e.kind, e.engines, EngineType(e.engine)};
  return tr;
}

// ---------------------------------------------------------------- v3: trajectory nodes
//
// A flight is a smooth curve, and the search reads it as one: a cubic Hermite curve
// through positions and velocities (doppler.cpp). The store keeps only the nodes such a
// curve needs, not ADS-B's fixes (one every 0.5-20 s):
//
// 1. Spikes out: a fix far from where both neighbours put it while they agree (a bad
//    position decode, two aircraft on one address: > SPIKE_POS_M; a barometric glitch:
//    > SPIKE_M up or down).
// 2. Smoothing: each fix's position and velocity from a quadratic fitted by least squares
//    to the fixes within SMOOTH_S (positions, and reported velocities weighted
//    VEL_WEIGHT). ADS-B's jitter (metres, ~1 m/s) is averaged out, not followed.
// 3. Nodes, per gap-free piece (split at leg starts and gaps > PIECE_GAP_S): from a node,
//    the farthest fix whose Hermite curve from the node passes within TOL of every
//    smoothed fix in between, at most MAX_SPAN_S on. A steady leg is a node every
//    MAX_SPAN_S; turns, climbs and speed changes add the nodes they need.
// 4. Nodes sit on whole seconds from the track's first fix (moved along their velocity).
//
// On 2026-09-08 (35k aircraft, 36.9M fixes): 8.5M nodes, 48 MB (v1 0.68 GB, v2 ~0.22 GB;
// 2026-11-12: 33.0M fixes, 41 MB);
// against the raw fixes, positions within 11 m (median) / 31 m (95%), radial velocity
// (Doppler) within 0.7 / 5 m/s, about the raw fixes' own disagreement between reported
// velocities and positions.
//
// A node is coded against the previous ones, in integers only (every machine decodes the
// same values):
//   dt        1 s
//   velocity  east, north, up in VQ_MM mm/s: residual from the previous node's, changed
//             by half its last change
//   position  lat/lon in LQ deg, alt in AQ_MM mm: residual from the previous node moved
//             by the mean of the two velocities (trapezoid)
//   flags     alt source, leg start
// each value zigzagged into 16 bits, split into low/high byte planes (larger values
// escape to varints at the end), then zstd: 5.7 bytes a node.
struct Tol {
  double pos_h, pos_v, vel_h, vel_v;
};
constexpr Tol TOL{25.0, 15.0, 5.0, 3.0};  // m, m, m/s, m/s
constexpr double MAX_SPAN_S = 100.0;     // below split_tracks' 120 s gap
constexpr double PIECE_GAP_S = 120.0;
constexpr double VEL_WEIGHT = 10.0;  // ~ position noise (m) / velocity noise (m/s)
constexpr double SPIKE_M = 20.0;      // altitude
constexpr double SPIKE_POS_M = 300.0;
constexpr double SMOOTH_S = 6.0;          // smoothing half-window
constexpr std::int64_t VQ_MM = 500;       // velocity unit, mm/s
constexpr double LQ = 5e-5;               // lat/lon unit, deg (5.6 m)
constexpr std::int64_t LQ_MM = 5560;      // LQ in mm along a meridian
constexpr std::int64_t AQ_MM = 4000;      // altitude unit, mm

void put_var(std::string& s, std::int64_t v) {
  std::uint64_t u = (std::uint64_t(v) << 1) ^ std::uint64_t(v >> 63);
  while (u >= 0x80) s += char(std::uint8_t(u) | 0x80), u >>= 7;
  s += char(u);
}
std::int64_t get_var(std::string_view& s) {
  std::uint64_t u = 0;
  for (int shift = 0;; shift += 7) {
    if (s.empty() || shift > 63) throw std::runtime_error("truncated track store");
    const auto b = std::uint8_t(s.front());
    s.remove_prefix(1);
    u |= std::uint64_t(b & 0x7f) << shift;
    if (!(b & 0x80)) break;
  }
  return std::int64_t(u >> 1) ^ -std::int64_t(u & 1);
}
constexpr int COLUMNS = 7;  // dt, ve, vn, vu, lat, lon, alt
constexpr std::uint64_t ESCAPE = 0xffff;

// a / b rounded to nearest (b > 0).
std::int64_t div_round(std::int64_t a, std::int64_t b) { return a >= 0 ? (a + b / 2) / b : -((-a + b / 2) / b); }

struct QNode {
  std::int64_t t = 0, ve = 0, vn = 0, vu = 0, lat = 0, lon = 0, alt = 0;
  std::uint8_t flags = 0;
};
// The previous node moved by the trapezoid of the two velocities; across a gap or a
// leg start, where there is no motion to follow, the previous position. Only a predictor
// (its residual is stored), so approximate constants are fine; cos(lat) is quantised to
// 1/4096 so that the result is exact integers.
void predict(const QNode& p, QNode& q) {
  const std::int64_t dt = q.t - p.t;
  q.lat = p.lat, q.lon = p.lon, q.alt = p.alt;
  if ((q.flags & 4) || dt > std::int64_t(PIECE_GAP_S)) return;
  const std::int64_t c = std::max<std::int64_t>(1, std::llround(std::cos(double(p.lat) * LQ * DEG) * 4096.0));
  q.lat += div_round((p.vn + q.vn) * dt * VQ_MM, 2 * LQ_MM);
  q.lon += div_round((p.ve + q.ve) * dt * VQ_MM * 4096, 2 * LQ_MM * c);
  q.alt += div_round((p.vu + q.vu) * dt * VQ_MM, 2 * AQ_MM);
}

// Velocity predictor: the previous node's, changed by half its last change (scaled by
// the time steps), within a piece. Needs nd[i].t and flags.
void predict_vel(const std::vector<QNode>& nd, std::size_t i, QNode& out) {
  const QNode& a = nd[i - 1];
  out.ve = a.ve, out.vn = a.vn, out.vu = a.vu;
  auto starts = [&](std::size_t k) { return k == 0 || (nd[k].flags & 4) || nd[k].t - nd[k - 1].t > std::int64_t(PIECE_GAP_S); };
  if (i < 2 || starts(i) || starts(i - 1)) return;
  const QNode& b = nd[i - 2];
  const std::int64_t h = nd[i].t - a.t, h0 = a.t - b.t;
  out.ve += div_round((a.ve - b.ve) * h, 2 * h0), out.vn += div_round((a.vn - b.vn) * h, 2 * h0), out.vu += div_round((a.vu - b.vu) * h, 2 * h0);
}

std::string nodes_blob(const std::vector<QNode>& nd) {
  const std::size_t n = nd.size();
  std::vector<std::int64_t> v(n * COLUMNS);
  for (std::size_t i = 0; i < n; ++i) {
    const QNode& q = nd[i];
    QNode pr, pred = q;  // first node: absolute values
    pred.lat = pred.lon = pred.alt = 0;
    if (i) pr = nd[i - 1], predict(pr, pred);
    QNode pv = pr;
    if (i) predict_vel(nd, i, pv);
    const std::int64_t row[COLUMNS] = {q.t - pr.t, q.ve - pv.ve, q.vn - pv.vn, q.vu - pv.vu, q.lat - pred.lat, q.lon - pred.lon, q.alt - pred.alt};
    for (int c = 0; c < COLUMNS; ++c) v[std::size_t(c) * n + i] = row[c];
  }
  std::string out(v.size() * 2 + n, '\0'), esc;
  for (std::size_t i = 0; i < v.size(); ++i) {
    std::uint64_t u = (std::uint64_t(v[i]) << 1) ^ std::uint64_t(v[i] >> 63);
    if (u >= ESCAPE) put_var(esc, v[i]), u = ESCAPE;
    out[i] = char(u & 0xff), out[v.size() + i] = char(u >> 8);
  }
  for (std::size_t i = 0; i < n; ++i) out[2 * v.size() + i] = char(nd[i].flags);
  return out + esc;
}

std::vector<Fix> nodes_of(std::string_view raw, std::size_t n, double t0) {
  const std::size_t m = n * COLUMNS;
  if (raw.size() < 2 * m + n) throw std::runtime_error("truncated track store");
  std::string_view esc = raw.substr(2 * m + n);
  std::vector<std::int64_t> v(m);
  for (std::size_t i = 0; i < m; ++i) {
    const std::uint64_t u = std::uint64_t(std::uint8_t(raw[i])) | std::uint64_t(std::uint8_t(raw[m + i])) << 8;
    v[i] = u == ESCAPE ? get_var(esc) : std::int64_t(u >> 1) ^ -std::int64_t(u & 1);
  }
  std::vector<Fix> fx(n);
  std::vector<QNode> nd(n);
  QNode prev;
  const double vq = double(VQ_MM) / 1000.0;
  for (std::size_t i = 0; i < n; ++i) {
    auto c = [&](int k) { return v[std::size_t(k) * n + i]; };
    QNode& q = nd[i];
    q.t = prev.t + c(0), q.flags = std::uint8_t(raw[2 * m + i]);
    QNode pv = prev;
    if (i) predict_vel(nd, i, pv);
    q.ve = pv.ve + c(1), q.vn = pv.vn + c(2), q.vu = pv.vu + c(3);
    if (i) predict(prev, q);
    q.lat += c(4), q.lon += c(5), q.alt += c(6);
    Fix& f = fx[i];
    f.t = t0 + double(q.t);
    f.lat = double(q.lat) * LQ, f.lon = double(q.lon) * LQ, f.alt_m = float(double(q.alt) * double(AQ_MM) / 1000.0);
    const double ve = double(q.ve) * vq, vn = double(q.vn) * vq;
    f.gs = float(std::hypot(ve, vn));
    f.track = float(std::fmod(std::atan2(ve, vn) / DEG + 360.0, 360.0));
    f.vrate = float(double(q.vu) * vq);
    f.alt_src = AltSource(q.flags & 3), f.leg_start = q.flags & 4;
    prev = q;
  }
  return fx;
}

// Cubic Hermite from (pa, va) at 0 to (pb, vb) at h, at t, as Path::state.
void hermite(const Vec3& pa, const Vec3& va, const Vec3& pb, const Vec3& vb, double h, double t, Vec3& p, Vec3& v) {
  const double s = t / h, s2 = s * s, s3 = s2 * s;
  p = pa * (2 * s3 - 3 * s2 + 1) + va * ((s3 - 2 * s2 + s) * h) + pb * (-2 * s3 + 3 * s2) + vb * ((s3 - s2) * h);
  v = pa * ((6 * s2 - 6 * s) / h) + va * (3 * s2 - 4 * s + 1) + pb * ((-6 * s2 + 6 * s) / h) + vb * (3 * s2 - 2 * s);
}

// The nodes of a track's fixes (time order), quantised.
std::vector<QNode> simplify(const std::vector<Fix>& all, double t0) {
  // Distinct times.
  std::vector<Fix> fx;
  fx.reserve(all.size());
  for (auto& f : all)
    if (fx.empty() || f.t > fx.back().t + 1e-3) fx.push_back(f);
    else if (f.leg_start) fx.back().leg_start = true;
  std::vector<Vec3> P, V;
  std::vector<char> hasv;
  auto states = [&] {
    const std::size_t n = fx.size();
    P.resize(n), V.resize(n), hasv.resize(n);
    for (std::size_t i = 0; i < n; ++i) {
      const Fix& f = fx[i];
      P[i] = ecef(f.lat, f.lon, std::isfinite(f.alt_m) ? f.alt_m : 0.0f);
      hasv[i] = std::isfinite(f.gs) && std::isfinite(f.track);
      V[i] = hasv[i] ? velocity_ecef(f.lat, f.lon, f.gs, f.track, std::isfinite(f.vrate) ? f.vrate : 0.0f) : Vec3{NAN, NAN, NAN};
    }
  };
  // Spikes: a fix far from where both neighbours put it while they agree with each other
  // (a bad position decode, two aircraft on one address, a barometric glitch).
  states();
  {
    auto moved = [&](std::size_t a, double t) {
      const Vec3 v = hasv[a] ? V[a] : Vec3{0, 0, 0};
      return P[a] + v * (t - fx[a].t);
    };
    std::vector<char> drop(fx.size());
    for (std::size_t i = 1; i + 1 < fx.size(); ++i) {
      if (fx[i].leg_start || fx[i + 1].leg_start) continue;
      const std::size_t a = i - 1, b = i + 1;
      if (fx[b].t - fx[a].t > PIECE_GAP_S) continue;
      const double m = (moved(a, fx[i].t) - P[i]).norm(), mb = (moved(b, fx[i].t) - P[i]).norm(), ab = (moved(a, fx[b].t) - P[b]).norm();
      const Vec3 up = P[i] * (1.0 / P[i].norm());
      const double dz = (P[i] - (P[a] + P[b]) * 0.5).dot(up), zab = std::abs((P[b] - P[a]).dot(up));
      if ((m > SPIKE_POS_M && mb > SPIKE_POS_M && ab < SPIKE_POS_M) || (std::abs(dz) > SPIKE_M && zab < SPIKE_M / 2)) drop[i] = 1;
    }
    std::size_t w = 0;
    for (std::size_t i = 0; i < fx.size(); ++i)
      if (!drop[i]) fx[w++] = fx[i];
      else if (fx[i].leg_start && w) fx[w - 1].leg_start = true;
    fx.resize(w);
  }
  states();
  const std::size_t n = fx.size();
  std::vector<std::size_t> piece{0};
  for (std::size_t i = 1; i < n; ++i)
    if (fx[i].leg_start || fx[i].t - fx[i - 1].t > PIECE_GAP_S) piece.push_back(i);
  piece.push_back(n);
  // Smoothed state per fix: a quadratic in time, least squares over the fixes within
  // SMOOTH_S (positions, and reported velocities weighted VEL_WEIGHT), in the fix's frame.
  std::vector<Vec3> S(n), SV(n);
  for (std::size_t pk = 0; pk + 1 < piece.size(); ++pk) {
    const std::size_t a = piece[pk], b = piece[pk + 1];
    std::size_t lo = a, hi = a;
    for (std::size_t i = a; i < b; ++i) {
      while (fx[i].t - fx[lo].t > SMOOTH_S) ++lo;
      while (hi < b && fx[hi].t - fx[i].t <= SMOOTH_S) ++hi;
      // p(t) = c0 + c1 t + c2 t^2 per axis (t from fix i): 3x3 normal equations, shared
      // matrix, three right-hand sides.
      double M[3][3] = {}, R[3][3] = {};
      const double w2 = VEL_WEIGHT * VEL_WEIGHT;
      for (std::size_t k = lo; k < hi; ++k) {
        const double t = fx[k].t - fx[i].t, row[3] = {1, t, t * t};
        const Vec3 d = P[k] - P[i];
        const double y[3] = {d.x, d.y, d.z};
        for (int r = 0; r < 3; ++r) {
          for (int c = 0; c < 3; ++c) M[r][c] += row[r] * row[c];
          for (int c = 0; c < 3; ++c) R[r][c] += row[r] * y[c];
        }
        if (hasv[k]) {
          const double vr[3] = {0, 1, 2 * t}, vy[3] = {V[k].x, V[k].y, V[k].z};
          for (int r = 0; r < 3; ++r) {
            for (int c = 0; c < 3; ++c) M[r][c] += w2 * vr[r] * vr[c];
            for (int c = 0; c < 3; ++c) R[r][c] += w2 * vr[r] * vy[c];
          }
        }
      }
      // Too few equations for a quadratic: a line (c2 = 0), else the fix as reported.
      int dim = 3;
      auto solve = [&](int d, double out[3][3]) {
        double A[3][4];
        for (int c = 0; c < 3; ++c) {
          for (int r = 0; r < d; ++r) {
            for (int k = 0; k < d; ++k) A[r][k] = M[r][k];
            A[r][d] = R[r][c];
          }
          for (int p = 0; p < d; ++p) {
            if (std::abs(A[p][p]) < 1e-9) return false;
            for (int r = p + 1; r < d; ++r) {
              const double f = A[r][p] / A[p][p];
              for (int k = p; k <= d; ++k) A[r][k] -= f * A[p][k];
            }
          }
          for (int r = d - 1; r >= 0; --r) {
            double s = A[r][d];
            for (int k = r + 1; k < d; ++k) s -= A[r][k] * out[k][c];
            out[r][c] = s / A[r][r];
          }
          for (int r = d; r < 3; ++r) out[r][c] = 0;
        }
        return true;
      };
      double C[3][3];
      bool good = hi - lo >= 3 && solve(dim, C);
      if (!good) dim = 2, good = solve(dim, C);
      if (good) {
        S[i] = P[i] + Vec3{C[0][0], C[0][1], C[0][2]}, SV[i] = {C[1][0], C[1][1], C[1][2]};
      } else {
        S[i] = P[i];
        SV[i] = hasv[i] ? V[i] : (b - a > 1 ? (P[i + 1 < b ? i + 1 : i] - P[i > a ? i - 1 : i]) * (1.0 / std::max(1e-3, fx[i + 1 < b ? i + 1 : i].t - fx[i > a ? i - 1 : i].t)) : Vec3{0, 0, 0});
      }
    }
  }
  // Nodes: from a node, the farthest fix j such that the Hermite curve between their
  // smoothed states passes within TOL of every smoothed state in between.
  auto ok = [&](std::size_t i, std::size_t j) {
    if (fx[j].t - fx[i].t > MAX_SPAN_S) return false;
    const double h = fx[j].t - fx[i].t;
    for (std::size_t k = i + 1; k < j; ++k) {
      Vec3 p, v;
      hermite(S[i], SV[i], S[j], SV[j], h, fx[k].t - fx[i].t, p, v);
      const Vec3 up = S[k] * (1.0 / S[k].norm()), dp = p - S[k], dv = v - SV[k];
      const double pz = dp.dot(up), vz = dv.dot(up);
      if (!(std::abs(pz) <= TOL.pos_v && dp.dot(dp) - pz * pz <= TOL.pos_h * TOL.pos_h)) return false;
      if (!(std::abs(vz) <= TOL.vel_v && dv.dot(dv) - vz * vz <= TOL.vel_h * TOL.vel_h)) return false;
    }
    return true;
  };
  std::vector<std::size_t> keep;
  for (std::size_t pk = 0; pk + 1 < piece.size(); ++pk) {
    const std::size_t a = piece[pk], b = piece[pk + 1];
    std::size_t i = a;
    keep.push_back(a);
    while (i + 1 < b) {
      std::size_t good = i + 1, step = 1;
      while (good + step < b && ok(i, good + step)) good += step, step *= 2;
      std::size_t lo = good, hi = std::min(b, good + step);
      while (hi - lo > 1) {
        const std::size_t mid = lo + (hi - lo) / 2;
        (ok(i, mid) ? lo : hi) = mid;
      }
      keep.push_back(lo);
      i = lo;
    }
  }
  std::vector<QNode> nd;
  nd.reserve(keep.size());
  const double vq = double(VQ_MM) / 1000.0;
  for (std::size_t i : keep) {
    const Fix& f = fx[i];
    // On a whole second: moved along the smoothed velocity.
    const double t = std::round(f.t - t0);
    if (!nd.empty() && t <= double(nd.back().t)) {  // that second has its node already
      if (f.leg_start) nd.back().flags |= 4;
      continue;
    }
    const Geodetic g = geodetic(S[i] + SV[i] * (t - (f.t - t0)));
    const Vec3 ve = Enu(g.lat, g.lon).rot(SV[i]);
    auto qv = [&](double x) { return std::isfinite(x) ? std::clamp<std::int64_t>(std::llround(x / vq), -5000, 5000) : 0; };
    QNode q;
    q.t = std::int64_t(t);
    q.ve = qv(ve.x), q.vn = qv(ve.y), q.vu = qv(ve.z);
    q.lat = std::llround(g.lat / LQ), q.lon = std::llround(g.lon / LQ), q.alt = std::llround(g.h * 1000.0 / double(AQ_MM));
    q.flags = std::uint8_t(std::uint8_t(f.alt_src) | (f.leg_start ? 4 : 0));
    nd.push_back(q);
  }
  return nd;
}

std::string zstd_compress(std::string_view raw, int level) {
  std::string out(ZSTD_compressBound(raw.size()), '\0');
  const std::size_t r = ZSTD_compress(out.data(), out.size(), raw.data(), raw.size(), level);
  if (ZSTD_isError(r)) throw std::runtime_error(std::string("zstd: ") + ZSTD_getErrorName(r));
  out.resize(r);
  return out;
}
std::string zstd_decompress(std::string_view c) {
  const auto n = ZSTD_getFrameContentSize(c.data(), c.size());
  if (n == ZSTD_CONTENTSIZE_ERROR || n == ZSTD_CONTENTSIZE_UNKNOWN) throw std::runtime_error("corrupt track blob");
  std::string out(std::size_t(n), '\0');
  const std::size_t r = ZSTD_decompress(out.data(), out.size(), c.data(), c.size());
  if (ZSTD_isError(r) || r != n) throw std::runtime_error("corrupt track blob");
  return out;
}

// ---------------------------------------------------------------- altitude offsets

// Geometric-minus-barometric offsets (ft) per 2 deg x 2 deg x 3 h cell, a bounded sample each.
class RegionOffsets {
 public:
  static std::uint64_t key(double lat, double lon, double t) {
    const auto a = std::uint64_t(std::clamp(int((lat + 90) / 2), 0, 89));
    const auto b = std::uint64_t(std::clamp(int((lon + 180) / 2), 0, 179));
    const auto c = std::uint64_t(std::max<i64>(0, i64(std::floor(t / 10800.0)) + 1000000));
    return (c << 16) | (a << 8) | b;
  }
  void add(std::uint64_t k, float off) {
    std::lock_guard lk(mu_);
    auto& v = s_[k];
    if (v.size() < 4000) v.push_back(off);
  }
  void finish() {
    for (auto& [k, v] : s_)
      if (v.size() >= 5) {
        std::nth_element(v.begin(), v.begin() + std::ptrdiff_t(v.size() / 2), v.end());
        med_[k] = v[v.size() / 2];
      }
  }
  std::optional<float> at(double lat, double lon, double t) const {
    auto it = med_.find(key(lat, lon, t));
    if (it == med_.end()) return std::nullopt;
    return it->second;
  }

 private:
  std::mutex mu_;
  std::unordered_map<std::uint64_t, std::vector<float>> s_;
  std::unordered_map<std::uint64_t, float> med_;
};

double num(const json::value& v) {
  if (v.is_double()) return v.get_double();
  if (v.is_int64()) return double(v.get_int64());
  if (v.is_uint64()) return double(v.get_uint64());
  return NAN;
}

}  // namespace

// ---------------------------------------------------------------- public

const char* engine_name(EngineType e) {
  switch (e) {
    case EngineType::Piston: return "piston";
    case EngineType::Turboprop: return "turboprop";
    case EngineType::Jet: return "jet";
    case EngineType::Electric: return "electric";
    case EngineType::Rocket: return "rocket";
    default: return "unknown";
  }
}

std::string Track::id() const {
  char b[8];
  std::snprintf(b, sizeof b, "%06x", icao);
  return b;
}

Bbox Bbox::grown(double m) const {
  const double dlat = m / 111320.0;
  const double c = std::max(0.05, std::cos(std::max(std::abs(south), std::abs(north)) * DEG));
  const double dlon = dlat / c;
  return {std::max(-180.0, west - dlon), std::max(-90.0, south - dlat), std::min(180.0, east + dlon), std::min(90.0, north + dlat)};
}

TypeDb TypeDb::parse(std::string_view text) {
  TypeDb db;
  auto v = parse_json(text);
  for (auto& kv : v.as_object()) {
    const std::string desc = jstr(kv.value(), "desc");
    if (desc.size() != 3 || kv.key().size() > 4) continue;
    TypeInfo t;
    t.kind = desc[0];
    t.engines = desc[1] >= '0' && desc[1] <= '9' ? desc[1] - '0' : 0;
    switch (desc[2]) {
      case 'P': t.engine = EngineType::Piston; break;
      case 'T': t.engine = EngineType::Turboprop; break;
      case 'J': t.engine = EngineType::Jet; break;
      case 'E': t.engine = EngineType::Electric; break;
      case 'R': t.engine = EngineType::Rocket; break;
      default: break;
    }
    std::array<char, 4> code{};
    std::memcpy(code.data(), kv.key().data(), kv.key().size());
    db.codes_.emplace_back(code, t);
  }
  std::sort(db.codes_.begin(), db.codes_.end(), [](auto& a, auto& b) { return a.first < b.first; });
  return db;
}

std::optional<TypeInfo> TypeDb::find(std::string_view d) const {
  if (d.empty() || d.size() > 4) return std::nullopt;
  std::array<char, 4> code{};
  std::memcpy(code.data(), d.data(), d.size());
  auto it = std::lower_bound(codes_.begin(), codes_.end(), code, [](auto& a, auto& b) { return a.first < b; });
  if (it == codes_.end() || it->first != code) return std::nullopt;
  return it->second;
}

const TypeDb& TypeDb::get() {
  static const TypeDb db = [] {
    const fs::path p = adsb_dir() / "icao_aircraft_types.json";
    auto text = read_file(p);
    if (!text) {
      auto body = net::run_sync<std::string>([](net::asio::io_context& io) -> net::asio::awaitable<std::string> {
        net::Http http(io);
        auto r = co_await http.get(TYPES_URL);
        co_return r.body;
      });
      if (body.size() > 2 && static_cast<unsigned char>(body[0]) == 0x1f && static_cast<unsigned char>(body[1]) == 0x8b) body = gunzip(body);
      parse_json(body);  // validate before caching
      fs::create_directories(p.parent_path());
      atomic_write(p, body);
      text = std::move(body);
    }
    return TypeDb::parse(*text);
  }();
  return db;
}

AircraftDb AircraftDb::parse(std::string_view csv) {
  AircraftDb db;
  while (!csv.empty()) {
    const auto nl = csv.find('\n');
    std::string_view line = csv.substr(0, nl);
    csv.remove_prefix(nl == std::string_view::npos ? csv.size() : nl + 1);
    // icao;registration;type;flags;description;...
    const auto a = line.find(';');
    if (a == std::string_view::npos || a != 6) continue;
    const auto b = line.find(';', a + 1);
    if (b == std::string_view::npos) continue;
    const auto c = line.find(';', b + 1);
    const std::string_view type = line.substr(b + 1, (c == std::string_view::npos ? line.size() : c) - b - 1);
    if (type.empty() || type.size() > 4) continue;
    std::array<char, 4> t{};
    std::memcpy(t.data(), type.data(), type.size());
    db.rows_.emplace_back(std::uint32_t(std::strtoul(std::string(line.substr(0, 6)).c_str(), nullptr, 16)), t);
  }
  std::sort(db.rows_.begin(), db.rows_.end());
  return db;
}

std::string AircraftDb::type(std::uint32_t icao) const {
  auto it = std::lower_bound(rows_.begin(), rows_.end(), icao, [](auto& r, std::uint32_t k) { return r.first < k; });
  if (it == rows_.end() || it->first != icao) return {};
  return std::string(it->second.data(), strnlen(it->second.data(), 4));
}

const AircraftDb& AircraftDb::get() {
  static const AircraftDb db = [] {
    const fs::path p = adsb_dir() / "aircraft.csv.gz";
    auto gz = read_file(p);
    if (!gz) try {
        auto body = net::run_sync<std::string>([](net::asio::io_context& io) -> net::asio::awaitable<std::string> {
          net::Http http(io);
          auto r = co_await http.fetch({.url = AIRCRAFT_URL, .total_timeout_s = 600, .decompress = false});
          if (!r.ok()) throw std::runtime_error("HTTP " + std::to_string(r.status) + " " + r.error);
          co_return r.body;
        });
        gunzip(body);  // validate before caching
        fs::create_directories(p.parent_path());
        atomic_write(p, body);
        gz = std::move(body);
      } catch (const std::exception& e) {
        std::cerr << "warning: aircraft registry unavailable (" << e.what() << "): types missing from traces stay unknown" << std::endl;
        return AircraftDb();
      }
    return AircraftDb::parse(gunzip(*gz));
  }();
  return db;
}

std::map<std::uint32_t, RegistryEntry> registry_lookup(const std::set<std::uint32_t>& icaos) {
  std::map<std::uint32_t, RegistryEntry> out;
  if (icaos.empty()) return out;
  AircraftDb::get();  // downloads the registry when it is missing
  auto gz = read_file(adsb_dir() / "aircraft.csv.gz");
  if (!gz) return out;
  const std::string csv = gunzip(*gz);
  std::string_view rest(csv);
  while (!rest.empty() && out.size() < icaos.size()) {
    const auto nl = rest.find('\n');
    std::string_view line = rest.substr(0, nl);
    rest.remove_prefix(nl == std::string_view::npos ? rest.size() : nl + 1);
    if (line.size() < 7 || line[6] != ';') continue;
    const auto icao = std::uint32_t(std::strtoul(std::string(line.substr(0, 6)).c_str(), nullptr, 16));
    if (!icaos.count(icao)) continue;
    // icao;registration;type;flags;description;year;owner;
    std::vector<std::string_view> f;
    for (std::size_t a = 0; a <= line.size();) {
      const auto b = std::min(line.size(), line.find(';', a));
      f.push_back(line.substr(a, b - a));
      a = b + 1;
    }
    f.resize(std::max<std::size_t>(f.size(), 7));
    out[icao] = {std::string(f[1]), std::string(f[2]), std::string(f[4]), std::string(f[5]), std::string(f[6])};
  }
  return out;
}

fs::path adsb_dir() { return store::cache_dir() / "adsb"; }
fs::path day_path(const std::string& day) { return adsb_dir() / (day + ".trk"); }
// A store in an older format is removed (it would not be read): the day is fetched again.
bool has_day(const std::string& day) {
  const fs::path p = day_path(day);
  if (!fs::exists(p)) return false;
  char m[8] = {};
  std::ifstream(p, std::ios::binary).read(m, 8);
  if (std::memcmp(m, MAGIC, 8) == 0) return true;
  std::error_code ec;
  fs::remove(p, ec);
  return false;
}

std::vector<std::string> stored_days() {
  std::vector<std::string> out;
  std::error_code ec;
  for (auto& e : fs::directory_iterator(adsb_dir(), ec))
    if (e.path().extension() == ".trk" && has_day(e.path().stem().string())) out.push_back(e.path().stem().string());
  std::sort(out.begin(), out.end());
  return out;
}

namespace {
struct OffsetSample {
  double lat, lon, t;
  float off_ft;
};
std::optional<Track> parse_trace_impl(std::string_view text, const TypeDb& types, bool keep_jets, const std::optional<Bbox>& bbox,
                                      std::vector<OffsetSample>* samples) {
  json::value v = parse_json(text);
  if (!v.is_object()) return std::nullopt;
  Track tr;
  const std::string hex = jstr(v, "icao");
  if (hex.empty() || hex[0] == '~') return std::nullopt;  // non-ICAO (TIS-B) addresses
  tr.icao = std::uint32_t(std::strtoul(hex.c_str(), nullptr, 16));
  tr.type = jstr(v, "t");
  tr.reg = jstr(v, "r");
  if (tr.type.empty()) tr.type = AircraftDb::get().type(tr.icao);
  if (auto ti = types.find(tr.type)) tr.info = *ti;
  if (tr.info.engine == EngineType::Jet && !keep_jets) return std::nullopt;
  const double base = jnum(v, "timestamp");
  const json::value* trace = jget(v, "trace");
  if (!trace || !trace->is_array() || !std::isfinite(base)) return std::nullopt;
  std::vector<float> offsets;
  for (auto& pv : trace->as_array()) {
    if (!pv.is_array()) continue;
    auto& p = pv.as_array();
    if (p.size() < 8) continue;
    Fix f;
    f.t = base + num(p[0]);
    f.lat = num(p[1]), f.lon = num(p[2]);
    if (!std::isfinite(f.t) || !std::isfinite(f.lat) || !std::isfinite(f.lon)) continue;
    if (p[3].is_string()) continue;  // "ground"
    const int flags = int(num(p[6]));
    f.leg_start = flags & 2;
    double baro = NAN, geom = NAN;
    if (flags & 8) geom = num(p[3]);
    else baro = num(p[3]);
    if (p.size() > 10 && !std::isfinite(geom)) geom = num(p[10]);
    if (!std::isfinite(geom) && p[8].is_object()) geom = jnum(p[8], "alt_geom");
    if (std::isfinite(geom) && std::isfinite(baro)) {
      offsets.push_back(float(geom - baro));
      if (samples && offsets.size() % 8 == 1) samples->push_back({f.lat, f.lon, f.t, float(geom - baro)});
    }
    if (std::isfinite(geom)) f.alt_m = float(geom * FT), f.alt_src = AltSource::Geometric;
    else if (std::isfinite(baro)) f.alt_m = float(baro * FT), f.alt_src = AltSource::BaroRaw;
    else continue;
    f.gs = float(num(p[4]) * KT);
    f.track = float(num(p[5]));
    const double vr = (p.size() > 11 && std::isfinite(num(p[11]))) ? num(p[11]) : num(p[7]);
    f.vrate = float(vr * FPM);
    if (bbox && !bbox->contains(f.lat, f.lon)) continue;
    tr.fixes.push_back(f);
  }
  if (tr.fixes.empty()) return std::nullopt;
  std::sort(tr.fixes.begin(), tr.fixes.end(), [](const Fix& a, const Fix& b) { return a.t < b.t; });
  if (offsets.size() >= 3) {
    std::nth_element(offsets.begin(), offsets.begin() + std::ptrdiff_t(offsets.size() / 2), offsets.end());
    const float off = offsets[offsets.size() / 2] * float(FT);
    for (auto& f : tr.fixes)
      if (f.alt_src == AltSource::BaroRaw) f.alt_m += off, f.alt_src = AltSource::BaroTrack;
  }
  return tr;
}
}  // namespace

std::optional<Track> parse_trace(std::string_view text, const TypeDb& types, bool keep_jets, const std::optional<Bbox>& bbox) {
  return parse_trace_impl(text, types, keep_jets, bbox, nullptr);
}

namespace {
// A store: MAGIC, the node blobs in index order, zstd(index), u32 count, u32 index size.
// fixes(e) gives each entry's full fixes (called in parallel), reduced to nodes here;
// returns the bytes written.
template <class Fixes>
std::uint64_t write_store(std::ostream& of, std::vector<IndexEntry> idx, Fixes&& fixes) {
  of.write(MAGIC, 8);
  std::uint64_t off = 0;
  constexpr std::size_t BATCH = 2048;  // encoded in parallel, written in order
  for (std::size_t s = 0; s < idx.size(); s += BATCH) {
    const std::size_t m = std::min(BATCH, idx.size() - s);
    std::vector<std::string> c(m);
    parallel_for(m, [&](std::size_t i) {
      auto& e = idx[s + i];
      const auto nd = simplify(fixes(e), e.t0);
      e.nfix = std::uint32_t(nd.size());
      c[i] = zstd_compress(nodes_blob(nd), STORE_LEVEL);
    });
    for (std::size_t i = 0; i < m; ++i) {
      idx[s + i].offset = off, idx[s + i].clen = std::uint32_t(c[i].size());
      of.write(c[i].data(), std::streamsize(c[i].size()));
      off += c[i].size();
    }
  }
  const auto zi = zstd_compress({reinterpret_cast<const char*>(idx.data()), idx.size() * sizeof(IndexEntry)}, STORE_LEVEL);
  of.write(zi.data(), std::streamsize(zi.size()));
  std::string tail;
  put(tail, std::uint32_t(idx.size()));
  put(tail, std::uint32_t(zi.size()));
  of.write(tail.data(), std::streamsize(tail.size()));
  return 8 + off + zi.size() + tail.size();
}
}  // namespace

std::string encode_tracks(const std::vector<Track>& tracks) {
  std::vector<IndexEntry> idx;
  std::vector<const Track*> src;
  for (auto& tr : tracks)
    if (!tr.fixes.empty()) idx.push_back(index_of(tr)), idx.back().offset = src.size(), src.push_back(&tr);
  std::ostringstream os;
  write_store(os, idx, [&](const IndexEntry& e) { return src[e.offset]->fixes; });
  return os.str();
}

namespace {

struct DayFile {
  MappedFile mf;
  std::vector<IndexEntry> idx;
  explicit DayFile(const fs::path& p) : mf(p) {
    std::string_view v = mf.view();
    if (v.size() < 16 || std::memcmp(v.data(), MAGIC, 8) != 0) throw std::runtime_error("not a track store: " + p.string());
    std::string_view tail = v.substr(v.size() - 8);
    const auto n = get<std::uint32_t>(tail), zlen = get<std::uint32_t>(tail);
    if (std::size_t(zlen) + 16 > v.size()) throw std::runtime_error("truncated track store: " + p.string());
    const std::string raw = zstd_decompress(v.substr(v.size() - 8 - zlen, zlen));
    if (raw.size() != std::size_t(n) * sizeof(IndexEntry)) throw std::runtime_error("corrupt track store index: " + p.string());
    idx.resize(n);
    std::memcpy(idx.data(), raw.data(), raw.size());
  }
  std::vector<Fix> fixes(const IndexEntry& e) const { return nodes_of(zstd_decompress(mf.view().substr(8 + e.offset, e.clen)), e.nfix, e.t0); }
  // What identifies this store's contents: its size and its tail (aircraft, index size).
  std::uint64_t signature() const {
    std::string_view v = mf.view();
    std::uint64_t tail = 0;
    std::memcpy(&tail, v.data() + v.size() - 8, 8);
    return std::uint64_t(v.size()) * 0x9e3779b97f4a7c15ull ^ tail;
  }
};

// ---------------------------------------------------------------- chunk index (<day>.idx)
//
// A sidecar to the store, per aircraft (in the store's index order): its nodes in
// CHUNK_S slots, each slot's box (0.01 deg, rounded outwards) and speed range (1 m/s,
// node speed sqrt(gs^2 + vrate^2)). A search over a large box, a narrow window or a
// speed range decompresses only aircraft with a matching chunk. Built from the store
// alone (at ingest, or on first use when missing or stale), so older stores need no
// rebuild.
constexpr char IDX_MAGIC[8] = {'G', 'L', 'A', 'D', 'X', 'I', '0', '1'};
constexpr double CHUNK_S = 600.0;

struct Chunk {
  std::int32_t slot = 0;  // floor(t / CHUNK_S)
  std::int16_t south = 0, north = 0, west = 0, east = 0;  // 0.01 deg
  std::uint8_t vlo = 0, vhi = 0;                         // m/s
};

struct DayIndex {
  std::vector<std::uint32_t> first;  // per aircraft: its chunks are [first[i], first[i + 1])
  std::vector<Chunk> chunks;
  // Has aircraft i a node in [t0, t1] inside the box (and, given, at a speed in [vlo, vhi])?
  bool any(std::size_t i, double t0, double t1, const Bbox& b, double vlo = 0, double vhi = INFINITY) const {
    const auto s0 = std::int32_t(std::floor(t0 / CHUNK_S)), s1 = std::int32_t(std::floor(t1 / CHUNK_S));
    for (std::uint32_t k = first[i]; k < first[i + 1]; ++k) {
      const Chunk& c = chunks[k];
      if (c.slot < s0 || c.slot > s1 || c.east * 0.01 < b.west || c.west * 0.01 > b.east || c.north * 0.01 < b.south || c.south * 0.01 > b.north) continue;
      if (c.vhi < vlo || c.vlo > vhi) continue;
      return true;
    }
    return false;
  }
};

std::vector<Chunk> chunks_of(const std::vector<Fix>& fx) {
  std::vector<Chunk> out;
  for (auto& f : fx) {
    const auto slot = std::int32_t(std::floor(f.t / CHUNK_S));
    const auto s = std::int16_t(std::floor(f.lat * 100)), n = std::int16_t(std::ceil(f.lat * 100));
    const auto w = std::int16_t(std::floor(f.lon * 100)), e = std::int16_t(std::ceil(f.lon * 100));
    std::uint8_t vlo = 0, vhi = 255;
    if (std::isfinite(f.gs)) {
      const double v = std::hypot(double(f.gs), std::isfinite(f.vrate) ? double(f.vrate) : 0.0);
      vlo = std::uint8_t(std::clamp(std::floor(v), 0.0, 255.0)), vhi = std::uint8_t(std::clamp(std::ceil(v), 0.0, 255.0));
    }
    if (out.empty() || out.back().slot != slot) {
      out.push_back({slot, s, n, w, e, vlo, vhi});
      continue;
    }
    Chunk& c = out.back();
    c.south = std::min(c.south, s), c.north = std::max(c.north, n), c.west = std::min(c.west, w), c.east = std::max(c.east, e);
    c.vlo = std::min(c.vlo, vlo), c.vhi = std::max(c.vhi, vhi);
  }
  return out;
}

// Columnar (each field's bytes in turn) for zstd.
std::string encode_index(const DayIndex& ix, std::uint64_t sig) {
  std::string raw;
  const std::size_t n = ix.first.size() - 1;
  for (std::size_t i = 0; i < n; ++i) put(raw, std::uint16_t(ix.first[i + 1] - ix.first[i]));
  std::int32_t prev = 0;
  for (auto& c : ix.chunks) put(raw, std::int32_t(c.slot - prev)), prev = c.slot;
  for (auto& c : ix.chunks) put(raw, c.south);
  for (auto& c : ix.chunks) put(raw, std::int16_t(c.north - c.south));
  for (auto& c : ix.chunks) put(raw, c.west);
  for (auto& c : ix.chunks) put(raw, std::int16_t(c.east - c.west));
  for (auto& c : ix.chunks) put(raw, c.vlo);
  for (auto& c : ix.chunks) put(raw, std::uint8_t(c.vhi - c.vlo));
  std::string out(IDX_MAGIC, 8);
  put(out, sig);
  put(out, std::uint32_t(n));
  put(out, std::uint32_t(ix.chunks.size()));
  out += zstd_compress(raw, STORE_LEVEL);
  return out;
}

std::optional<DayIndex> decode_index(std::string_view v, std::uint64_t sig, std::size_t n_aircraft) {
  if (v.size() < 24 || std::memcmp(v.data(), IDX_MAGIC, 8) != 0) return std::nullopt;
  v.remove_prefix(8);
  if (get<std::uint64_t>(v) != sig || get<std::uint32_t>(v) != n_aircraft) return std::nullopt;
  const std::size_t m = get<std::uint32_t>(v);
  const std::string raw = zstd_decompress(v);
  if (raw.size() != n_aircraft * 2 + m * 14) return std::nullopt;
  std::string_view r = raw;
  DayIndex ix;
  ix.first.resize(n_aircraft + 1);
  for (std::size_t i = 0; i < n_aircraft; ++i) ix.first[i + 1] = ix.first[i] + get<std::uint16_t>(r);
  if (ix.first.back() != m) return std::nullopt;
  ix.chunks.resize(m);
  std::int32_t prev = 0;
  for (auto& c : ix.chunks) c.slot = prev += get<std::int32_t>(r);
  for (auto& c : ix.chunks) c.south = get<std::int16_t>(r);
  for (auto& c : ix.chunks) c.north = std::int16_t(c.south + get<std::int16_t>(r));
  for (auto& c : ix.chunks) c.west = get<std::int16_t>(r);
  for (auto& c : ix.chunks) c.east = std::int16_t(c.west + get<std::int16_t>(r));
  for (auto& c : ix.chunks) c.vlo = get<std::uint8_t>(r);
  for (auto& c : ix.chunks) c.vhi = std::uint8_t(c.vlo + get<std::uint8_t>(r));
  return ix;
}

DayIndex build_index(const DayFile& df) {
  std::vector<std::vector<Chunk>> per(df.idx.size());
  parallel_for(df.idx.size(), [&](std::size_t i) { per[i] = chunks_of(df.fixes(df.idx[i])); });
  DayIndex ix;
  ix.first.push_back(0);
  for (auto& c : per) {
    if (c.size() > 0xffff) c.resize(0xffff);  // (a day holds 144 slots)
    ix.chunks.insert(ix.chunks.end(), c.begin(), c.end());
    ix.first.push_back(std::uint32_t(ix.chunks.size()));
  }
  return ix;
}

// The day's chunk index: read, or (missing, stale, unreadable) built and written.
DayIndex day_index(const std::string& day, const DayFile& df, bool force = false) {
  const fs::path p = index_path(day);
  if (!force)
    if (auto txt = read_file(p)) try {
        if (auto ix = decode_index(*txt, df.signature(), df.idx.size())) return std::move(*ix);
      } catch (const std::exception&) {  // damaged: rebuilt below
      }
  DayIndex ix = build_index(df);
  atomic_write(p, encode_index(ix, df.signature()));
  return ix;
}

}  // namespace

fs::path index_path(const std::string& day) { return adsb_dir() / (day + ".idx"); }

IndexStats ensure_index(const std::string& day, bool force) {
  DayFile df(day_path(day));
  const auto t = std::chrono::steady_clock::now();
  const DayIndex ix = day_index(day, df, force);
  IndexStats st;
  st.aircraft = df.idx.size(), st.chunks = ix.chunks.size();
  st.seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - t).count();
  std::error_code ec;
  st.bytes = fs::file_size(index_path(day), ec), st.store_bytes = fs::file_size(day_path(day), ec);
  return st;
}

namespace {

// Aircraft-minutes per cell for one stored day, cached next to it.
std::map<std::pair<int, int>, double> day_coverage(const std::string& day, bool force = false) {
  std::map<std::pair<int, int>, double> out;
  const fs::path side = coverage_path(day);
  std::error_code ec;
  if (!force)
    if (auto txt = read_file(side); txt && (!has_day(day) || fs::last_write_time(side, ec) >= fs::last_write_time(day_path(day), ec))) {
      for (auto& c : parse_json(*txt).as_object().at("cells").as_array()) {
        auto& a = c.as_array();
        out[{int(as_number(a[0])), int(as_number(a[1]))}] = as_number(a[2]);
      }
      return out;
    }
  DayFile df(day_path(day));
  std::mutex mu;
  parallel_for(df.idx.size(), [&](std::size_t i) {
    const auto fx = df.fixes(df.idx[i]);
    std::set<std::tuple<int, int, i64>> seen;  // (cell, minute) of this aircraft
    auto add = [&](double lat, double lon, double t) {
      seen.insert({int(std::floor((lat + 90) / COVERAGE_DEG)), int(std::floor((lon + 180) / COVERAGE_DEG)), i64(std::floor(t / 60))});
    };
    // Nodes are up to MAX_SPAN_S apart: the minutes between them count too.
    for (std::size_t k = 0; k < fx.size(); ++k) {
      const Fix& f = fx[k];
      add(f.lat, f.lon, f.t);
      if (k + 1 < fx.size() && !fx[k + 1].leg_start && fx[k + 1].t - f.t <= PIECE_GAP_S)
        for (double t = f.t + 30; t < fx[k + 1].t; t += 30) {
          const double s = (t - f.t) / (fx[k + 1].t - f.t);
          add(f.lat + s * (fx[k + 1].lat - f.lat), f.lon + s * (fx[k + 1].lon - f.lon), t);
        }
    }
    std::map<std::pair<int, int>, double> loc;
    for (auto& [a, b, m] : seen) loc[{a, b}] += 1;
    std::lock_guard lk(mu);
    for (auto& [k, v] : loc) out[k] += v;
  });
  json::array cells;
  for (auto& [k, v] : out) cells.push_back(json::array{k.first, k.second, v});
  atomic_write(side, dump(json::object{{"day", day}, {"cell_deg", COVERAGE_DEG}, {"cells", cells}}));
  return out;
}

}  // namespace

fs::path coverage_path(const std::string& day) { return adsb_dir() / (day + ".cov"); }
void ensure_coverage(const std::string& day) { day_coverage(day); }

// Merged over every day with a sidecar; recomputed only when the sidecars change.
std::string coverage_json() {
  static std::mutex mu;
  static std::string cached, cached_sig;
  std::vector<std::string> days;
  std::string sig;
  std::error_code ec;
  for (auto& e : fs::directory_iterator(adsb_dir(), ec)) {
    const auto ext = e.path().extension();
    if (ext == ".cov" || ext == ".trk") {
      const std::string d = e.path().stem().string();
      if (ext == ".trk" && fs::exists(coverage_path(d))) continue;
      days.push_back(d);
    }
  }
  std::sort(days.begin(), days.end());
  for (auto& d : days) {
    std::error_code e2;
    sig += d + ":" + std::to_string(fs::last_write_time(coverage_path(d), e2).time_since_epoch().count()) + ";";
  }
  std::lock_guard lk(mu);
  if (!cached.empty() && sig == cached_sig) return cached;
  const int rows = int(180 / COVERAGE_DEG), cols = int(360 / COVERAGE_DEG);
  std::vector<double> all(std::size_t(rows) * std::size_t(cols), 0.0);
  // Per-region aircraft-minutes for each listed day (the dashboard's region lanes). A cell
  // belongs to the first region whose box holds its centre; the rest is "Oceans & other".
  struct Region { const char* id; const char* name; double w, s, e, n; };
  static constexpr Region REGIONS[] = {
      {"europe", "Europe", -25, 35, 45, 72},       {"middle_east", "Middle East", 34, 12, 63, 38},
      {"north_america", "North America", -170, 7, -50, 84}, {"south_america", "South America", -92, -56, -30, 7},
      {"africa", "Africa", -20, -36, 52, 37},      {"oceania", "Oceania", 110, -50, 180, -10},
      {"asia", "Asia", 45, -11, 180, 82},
  };
  constexpr std::size_t NREG = std::size(REGIONS) + 1;
  auto region_of = [](int row, int col) {
    const double lat = row * COVERAGE_DEG - 90 + COVERAGE_DEG / 2, lon = col * COVERAGE_DEG - 180 + COVERAGE_DEG / 2;
    for (std::size_t i = 0; i < std::size(REGIONS); ++i)
      if (lon >= REGIONS[i].w && lon < REGIONS[i].e && lat >= REGIONS[i].s && lat < REGIONS[i].n) return i;
    return std::size(REGIONS);
  };
  std::vector<json::array> per_region(NREG);
  json::array listed;
  for (auto& d : days) {
    try {
      std::array<double, NREG> sums{};
      for (auto& [k, v] : day_coverage(d))
        if (k.first >= 0 && k.first < rows && k.second >= 0 && k.second < cols) {
          all[std::size_t(k.first) * std::size_t(cols) + std::size_t(k.second)] += v;
          sums[region_of(k.first, k.second)] += v;
        }
      for (std::size_t i = 0; i < NREG; ++i) per_region[i].push_back(json::value(sums[i]));
      listed.push_back(json::value(d));
    } catch (const std::exception& e) {
      std::cerr << "coverage of " << d << ": " << e.what() << std::endl;
    }
  }
  json::array cells;
  for (int a = 0; a < rows; ++a)
    for (int b = 0; b < cols; ++b)
      if (double v = all[std::size_t(a) * std::size_t(cols) + std::size_t(b)]; v > 0) cells.push_back(json::array{a * COVERAGE_DEG - 90, b * COVERAGE_DEG - 180, v});
  json::array regions;
  for (std::size_t i = 0; i < NREG; ++i) {
    json::object r{{"minutes", per_region[i]}};
    if (i < std::size(REGIONS)) {
      const auto& g = REGIONS[i];
      r["id"] = g.id, r["name"] = g.name, r["bbox"] = json::array{g.w, g.s, g.e, g.n};
    } else {
      r["id"] = "other", r["name"] = "Oceans & other";
    }
    regions.push_back(r);
  }
  cached = dump(json::object{{"cell_deg", COVERAGE_DEG}, {"days", listed}, {"cells", cells}, {"regions", regions}});
  cached_sig = sig;
  return cached;
}

void mark_used(const std::string& day) { atomic_write(adsb_dir() / (day + ".used"), ""); }

namespace {
std::mutex pin_mu;
std::multiset<std::string> pinned_days;  // over every live DayPinScope, all threads
thread_local DayPinScope* pin_scope = nullptr;
}  // namespace

DayPinScope::DayPinScope() : prev_(pin_scope) { pin_scope = this; }

DayPinScope::~DayPinScope() {
  pin_scope = prev_;
  std::lock_guard lk(pin_mu);
  for (auto& d : days_) pinned_days.erase(pinned_days.find(d));
}

void DayPinScope::pin(const std::string& day) {
  if (std::find(days_.begin(), days_.end(), day) != days_.end()) return;
  days_.push_back(day);
  std::lock_guard lk(pin_mu);
  pinned_days.insert(day);
}

// Keeps the pinned days and, of the rest, the ones used last: GEOLOC_AIRPLANE_KEEP_DAYS in
// all, more only while pinned days alone exceed it.
void evict_days() {
  const int keep = std::atoi(env("GEOLOC_AIRPLANE_KEEP_DAYS").c_str());
  if (keep <= 0) return;
  auto used = [](const std::string& d) {
    std::error_code ec;
    auto t = fs::last_write_time(adsb_dir() / (d + ".used"), ec);
    return ec ? fs::last_write_time(day_path(d), ec) : t;
  };
  std::set<std::string> pins;
  {
    std::lock_guard lk(pin_mu);
    pins.insert(pinned_days.begin(), pinned_days.end());
  }
  auto days = stored_days();
  std::sort(days.begin(), days.end(), [&](auto& a, auto& b) { return used(a) > used(b); });
  const auto n_pinned = std::size_t(std::count_if(days.begin(), days.end(), [&](auto& d) { return pins.contains(d); }));
  std::size_t room = std::size_t(keep) > n_pinned ? std::size_t(keep) - n_pinned : 0;
  for (auto& d : days) {
    if (pins.contains(d)) continue;
    if (room > 0) {
      --room;
      continue;
    }
    for (const char* ext : {".trk", ".idx", ".used"}) fs::remove(adsb_dir() / (d + ext));
  }
}

bool ensure_day(const std::string& day) {
  if (pin_scope) pin_scope->pin(day);
  if (has_day(day)) return true;
  if (!bucket::enabled()) return false;
  try {
    return bucket::days().contains(day) && bucket::pull_day(day);
  } catch (const std::exception& e) {
    std::cerr << "ADS-B bucket: " << e.what() << std::endl;
    return false;
  }
}

std::vector<Track> decode_tracks(std::string_view bytes) {
  TempFile tmp(".trk");
  atomic_write(tmp.path, bytes);
  DayFile df(tmp.path);
  std::vector<Track> out;
  for (auto& e : df.idx) {
    auto tr = track_of(e);
    tr.fixes = df.fixes(e);
    out.push_back(std::move(tr));
  }
  return out;
}

IngestStats ingest_day(const std::string& day, const IngestOptions& o) {
  const auto t_day = parse_iso_utc(day);
  if (!t_day || day.size() != 10) throw std::invalid_argument("day must be YYYY-MM-DD: " + day);
  const TypeDb& types = TypeDb::get();
  const AircraftDb& aircraft = AircraftDb::get();
  fs::create_directories(adsb_dir());
  IngestStats st;

  // Workers inflate and parse members; kept tracks go, compressed, to a spill file in
  // arrival order (memory stays bounded whatever the day's size).
  const fs::path spill_path = adsb_dir() / (day + ".spill");
  std::ofstream spill(spill_path, std::ios::binary | std::ios::trunc);
  std::vector<IndexEntry> idx;
  std::uint64_t spill_size = 0;
  RegionOffsets region;
  std::mutex out_mu, q_mu;
  std::condition_variable q_cv, q_space;
  std::deque<std::pair<std::string, std::string>> queue;  // (member name, gzip bytes)
  bool closing = false;
  std::atomic<std::size_t> members{0}, kept{0}, jets{0}, ground{0}, unknown{0};
  std::exception_ptr err;
  const std::size_t q_max = 256;

  auto work = [&] {
    for (;;) {
      std::string gz, name;
      {
        std::unique_lock lk(q_mu);
        q_cv.wait(lk, [&] { return !queue.empty() || closing; });
        if (queue.empty()) return;
        name = std::move(queue.front().first), gz = std::move(queue.front().second);
        queue.pop_front();
      }
      q_space.notify_one();
      try {
        ++members;
        // The type designator sits in the first few hundred bytes: drop jets unparsed.
        const std::string head = gunzip(gz, 512);
        std::string type;
        if (auto p = head.find("\"t\":\""); p != std::string::npos) type = head.substr(p + 5, head.find('"', p + 5) - p - 5);
        if (type.empty())  // trace_full_<icao>.json: the registry may know the type
          if (auto p = name.rfind("trace_full_"); p != std::string::npos)
            type = aircraft.type(std::uint32_t(std::strtoul(name.substr(p + 11, 6).c_str(), nullptr, 16)));
        const auto ti = types.find(type);
        if (!ti) ++unknown;
        if (ti && ti->engine == EngineType::Jet && !o.keep_jets) {
          ++jets;
          continue;
        }
        std::vector<OffsetSample> samples;
        auto tr = parse_trace_impl(gunzip(gz), types, o.keep_jets, o.bbox, &samples);
        if (!tr) {
          ++ground;
          continue;
        }
        for (auto& x : samples) region.add(RegionOffsets::key(x.lat, x.lon, x.t), x.off_ft);
        auto e = index_of(*tr);
        auto c = zstd_compress(fixes_blob(*tr), 1);  // a scratch copy: fast
        ++kept;
        std::lock_guard lk(out_mu);
        e.offset = spill_size, e.clen = std::uint32_t(c.size());
        spill.write(c.data(), std::streamsize(c.size()));
        spill_size += c.size();
        idx.push_back(e);
      } catch (...) {
        std::lock_guard lk(out_mu);
        if (!err) err = std::current_exception();
      }
    }
  };
  std::vector<std::jthread> pool;
  const int nt = std::max(1, worker_threads(o.threads) - 1);
  for (int i = 0; i < nt; ++i) pool.emplace_back(work);

  TarStream tar;
  tar.want = [](const std::string& n) { return n.find("trace_full_") != std::string::npos && n.ends_with(".json"); };
  tar.on_member = [&](std::string name, std::string data) {
    std::unique_lock lk(q_mu);
    q_space.wait(lk, [&] { return queue.size() < q_max; });
    queue.emplace_back(std::move(name), std::move(data));
    lk.unlock();
    q_cv.notify_one();
  };
  auto finish_workers = [&] {
    {
      std::lock_guard lk(q_mu);
      closing = true;
    }
    q_cv.notify_all();
    pool.clear();
  };

  try {
    if (!o.parts.empty()) {
      std::vector<char> buf(8 << 20);
      for (auto& part : o.parts) {
        std::ifstream in(part, std::ios::binary);
        if (!in) throw std::runtime_error("cannot read " + part);
        while (in) {
          in.read(buf.data(), std::streamsize(buf.size()));
          const auto got = std::size_t(in.gcount());
          st.bytes_in += got;
          tar.feed({buf.data(), got});
        }
      }
      st.release = "local:" + fs::path(o.parts.front()).filename().string();
    } else {
      // The release for the day: prod first, then staging (not the MLAT-only one). Its
      // assets are found by name (one .tar, or parts .tar.aa, .tar.ab, ...), without the
      // GitHub API and its rate limit.
      const std::string year = day.substr(0, 4), tag = "v" + day.substr(0, 4) + "." + day.substr(5, 2) + "." + day.substr(8, 2) + "-planes-readsb-";
      net::run_sync_void([&](net::asio::io_context& io) -> net::asio::awaitable<void> {
        net::Http http(io);
        auto exists = [&](const std::string& url) -> net::asio::awaitable<bool> {
          for (int attempt = 0;; ++attempt) {
            if (attempt) co_await net::sleep_for(double(1 << attempt));
            auto r = co_await http.fetch({.url = url, .method = "HEAD"});
            if (r.ok()) co_return true;
            if (r.status == 404) co_return false;
            if (attempt == 3) throw std::runtime_error("ADSB.lol release lookup failed: " + url + ": HTTP " + std::to_string(r.status) + " " + r.error);
          }
        };
        std::vector<std::string> urls;
        for (const char* pod : {"prod-0", "staging-0"}) {
          const std::string base = "https://github.com/adsblol/globe_history_" + year + "/releases/download/" + tag + pod + "/" + tag + pod + ".tar";
          if (co_await exists(base)) {
            urls.push_back(base);
          } else {
            for (char a = 'a'; a <= 'z' && urls.size() < 676; ++a) {
              bool more = true;
              for (char b = 'a'; b <= 'z'; ++b) {
                const std::string u = base + "." + a + b;
                if (!co_await exists(u)) {
                  more = false;
                  break;
                }
                urls.push_back(u);
              }
              if (!more) break;
            }
          }
          st.release = tag + pod;
          if (!urls.empty()) break;
        }
        if (urls.empty()) throw std::runtime_error("no ADSB.lol release for " + day);
        for (auto& u : urls) {
          std::cerr << "downloading " << u << std::endl;
          std::uint64_t got = 0, next_report = 256ull << 20;
          // A dropped connection resumes where it stopped (a byte range): the tar reader
          // just keeps reading.
          for (int attempt = 0;; ++attempt) {
            net::Request rq{.url = u,
                            .on_data =
                                [&](std::string_view b) {
                                  tar.feed(b);
                                  got += b.size();
                                  if (got >= next_report) {
                                    std::cerr << "  " << (got >> 20) << " MiB, " << kept.load() << " aircraft kept" << std::endl;
                                    next_report += 256ull << 20;
                                  }
                                  return true;
                                },
                            .stall_timeout_s = 120,
                            .decompress = false};
            if (got) rq.range = {got, (std::uint64_t(1) << 50)};
            auto r = co_await http.fetch(std::move(rq));
            if (r.ok()) break;
            if (attempt == 8 || (r.error.empty() && r.status >= 400 && r.status < 500 && r.status != 429 && r.status != 416))
              throw std::runtime_error("download failed: " + u + ": HTTP " + std::to_string(r.status) + " " + r.error);
            std::cerr << "  interrupted at " << (got >> 20) << " MiB (" << (r.error.empty() ? "HTTP " + std::to_string(r.status) : r.error)
                      << "), resuming" << std::endl;
            co_await net::sleep_for(double(5 << std::min(attempt, 4)));
          }
          st.bytes_in += got;
        }
      });
    }
  } catch (...) {
    finish_workers();
    spill.close();
    fs::remove(spill_path);
    throw;
  }
  finish_workers();
  spill.close();
  if (err) {
    fs::remove(spill_path);
    std::rethrow_exception(err);
  }
  st.members = members, st.kept = kept, st.jets = jets, st.ground_only = ground, st.unknown_type = unknown;

  // Regional pressure offsets from the kept aircraft that report both altitudes, applied
  // to those that report barometric altitude alone. Then the final file, sorted by start.
  region.finish();
  MappedFile sp(spill_path);
  auto blob = [&](const IndexEntry& e) { return sp.view().substr(e.offset, e.clen); };
  std::sort(idx.begin(), idx.end(), [](const IndexEntry& a, const IndexEntry& b) { return a.t0 < b.t0; });
  const fs::path out = day_path(day), tmp_out = out.string() + ".tmp";
  std::ofstream of(tmp_out, std::ios::binary | std::ios::trunc);
  std::atomic<std::size_t> nfix{0}, b_track{0}, b_region{0}, b_raw{0};
  st.bytes_out = write_store(of, idx, [&](const IndexEntry& e) {
    auto fx = fixes_of(zstd_decompress(blob(e)), e.nfix, e.t0);
    nfix += fx.size();
    for (auto& f : fx) {
      if (f.alt_src == AltSource::BaroRaw)
        if (auto off = region.at(f.lat, f.lon, f.t)) f.alt_m += *off * float(FT), f.alt_src = AltSource::BaroRegion;
      b_track += f.alt_src == AltSource::BaroTrack;
      b_region += f.alt_src == AltSource::BaroRegion;
      b_raw += f.alt_src == AltSource::BaroRaw;
    }
    return fx;
  });
  st.fixes = nfix, st.baro_track = b_track, st.baro_region = b_region, st.baro_raw = b_raw;
  of.close();
  if (!of) throw std::runtime_error("cannot write " + tmp_out.string());
  fs::rename(tmp_out, out);
  day_coverage(day, true);
  ensure_index(day, true);
  mark_used(day);
  evict_days();
  sp = MappedFile();
  fs::remove(spill_path);
  return st;
}

std::vector<Track> load_tracks(double t0, double t1, const std::optional<Bbox>& bbox, const std::optional<std::pair<double, double>>& speed) {
  std::vector<Track> out;
  // A day's archive holds that UTC day's traces; a window near midnight needs both.
  for (i64 d = floor_div(i64(std::floor(t0)), 86400) - 1; d <= floor_div(i64(std::floor(t1)), 86400); ++d) {
    const std::string day = iso_date(d * 86400);
    if (!has_day(day)) continue;
    DayFile df(day_path(day));
    {
      std::error_code ec;  // last use, for a bounded store's eviction
      fs::last_write_time(adsb_dir() / (day + ".used"), fs::file_time_type::clock::now(), ec);
    }
    const DayIndex ix = day_index(day, df);
    for (std::size_t i = 0; i < df.idx.size(); ++i) {
      const IndexEntry& e = df.idx[i];
      if (e.t1 < t0 - 300 || e.t0 > t1 + 300) continue;
      if (bbox && (e.east < bbox->west || e.west > bbox->east || e.north < bbox->south || e.south > bbox->north)) continue;
      // Kept below: nodes in the window, one either side, inside the box. A segment
      // needs two of them within PIECE_GAP_S, so one node in the box within
      // PIECE_GAP_S of the window: without such a chunk the aircraft would yield nothing.
      if (!ix.any(i, t0 - PIECE_GAP_S - 1, t1 + PIECE_GAP_S + 1, bbox.value_or(Bbox{}), speed ? speed->first : 0.0, speed ? speed->second : INFINITY))
        continue;
      Track tr = track_of(e);
      if (tr.type.empty())  // stored before the registry knew it
        if (auto t = AircraftDb::get().type(tr.icao); !t.empty())
          if (auto ti = TypeDb::get().find(t)) tr.type = t, tr.info = *ti;
      auto fx = df.fixes(e);
      // Fixes in the window plus one either side, inside the box.
      std::size_t a = std::size_t(std::lower_bound(fx.begin(), fx.end(), t0, [](const Fix& f, double t) { return f.t < t; }) - fx.begin());
      std::size_t b = std::size_t(std::upper_bound(fx.begin(), fx.end(), t1, [](double t, const Fix& f) { return t < f.t; }) - fx.begin());
      if (a > 0) --a;
      if (b < fx.size()) ++b;
      for (std::size_t i = a; i < b; ++i)
        if (!bbox || bbox->contains(fx[i].lat, fx[i].lon)) tr.fixes.push_back(fx[i]);
      if (!tr.fixes.empty()) out.push_back(std::move(tr));
    }
  }
  return out;
}

std::vector<Track> split_tracks(const std::vector<Track>& tracks, double max_gap_s) {
  std::vector<Track> out;
  for (auto& tr : tracks) {
    Track cur = tr;
    cur.fixes.clear();
    for (auto& f : tr.fixes) {
      if (!cur.fixes.empty() && (f.leg_start || f.t - cur.fixes.back().t > max_gap_s)) {
        if (cur.fixes.size() >= 2) out.push_back(cur);
        cur.fixes.clear();
      }
      if (cur.fixes.empty() || f.t > cur.fixes.back().t) cur.fixes.push_back(f);
    }
    if (cur.fixes.size() >= 2) out.push_back(std::move(cur));
  }
  return out;
}

}  // namespace geo::air
