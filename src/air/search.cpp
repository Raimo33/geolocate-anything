#include "air/search.hpp"

#include <algorithm>
#include <bit>
#include <chrono>
#include <cmath>
#include <limits>
#include <map>
#include <mutex>
#include <set>
#include <unordered_map>
#include <unordered_set>

#include "util/parallel.hpp"
#include "util/time.hpp"

namespace geo::air {

namespace {

constexpr double M_PER_DEG = 111195.0;
constexpr double TILE_DEG = 0.25;

// Cells: rows of constant latitude step; each row's longitude step keeps cells square.
struct CellGrid {
  double dlat;
  explicit CellGrid(double m) : dlat(m / M_PER_DEG) {}
  i64 row(double lat) const { return i64(std::floor(lat / dlat)); }
  double lat_c(i64 r) const { return (double(r) + 0.5) * dlat; }
  double dlon(i64 r) const { return dlat / std::max(0.05, std::cos(lat_c(r) * DEG)); }
  i64 col(i64 r, double lon) const { return i64(std::floor(lon / dlon(r))); }
  double lon_c(i64 r, i64 c) const { return (double(c) + 0.5) * dlon(r); }
  static std::uint64_t key(i64 r, i64 c) { return (std::uint64_t(r + (1 << 24)) << 32) | std::uint32_t(c + (i64(1) << 30)); }
  static i64 krow(std::uint64_t k) { return i64(k >> 32) - (1 << 24); }
  static i64 kcol(std::uint64_t k) { return i64(k & 0xffffffffu) - (i64(1) << 30); }
};

struct Seg {
  const Track* tr = nullptr;
  Path path;  // ECEF
  bool eligible = true;  // type allowed as an event source
  std::vector<char> f0_ok;  // per event: the type can make its tone
  Bbox box;                 // holds every point of the curve
  double vlo = 0, vhi = INFINITY;  // bounds of the curve's speed
};

}  // namespace

// From the nodes alone. Between nodes a and b (h apart) the curve is
// p0 + h01 (p1 - p0) + h (h10 v0 + h11 v1), |h10|, |h11| <= 4/27: within
// (4/27) h (|v0| + |v1|) of the chord. Its velocity is (1 - s) v0 + s v1 +
// 6 s (1 - s) (c - (v0 + v1) / 2), c the chord velocity: within 1.5 |c - (v0 + v1) / 2|
// of the segment from v0 to v1.
PathBounds path_bounds(const Path& path) {
  const auto& pts = path.points();
  double south = 90, north = -90, west = 180, east = -180, grow = 0, vlo = INFINITY, vhi = 0;
  for (std::size_t i = 0; i < pts.size(); ++i) {
    const Geodetic g = geodetic(pts[i].p);
    south = std::min(south, g.lat), north = std::max(north, g.lat), west = std::min(west, g.lon), east = std::max(east, g.lon);
    if (i + 1 == pts.size()) break;
    const PathPoint &a = pts[i], &b = pts[i + 1];
    const double h = b.t - a.t;
    if (h <= 0) {
      vlo = std::min(vlo, a.v.norm()), vhi = std::max(vhi, a.v.norm());
      continue;
    }
    const double va = a.v.norm(), vb = b.v.norm();
    // The chord bulges off the endpoints' box by far less than 1% of its length.
    grow = std::max(grow, 4.0 / 27.0 * h * (va + vb) + 0.01 * (b.p - a.p).norm());
    const Vec3 dv = (b.p - a.p) * (1.0 / h) - (a.v + b.v) * 0.5;
    const double e = 1.5 * dv.norm();
    const Vec3 w = b.v - a.v;  // nearest point of the velocity segment to 0
    const double ww = w.dot(w), s = ww > 0 ? std::clamp(-a.v.dot(w) / ww, 0.0, 1.0) : 0.0;
    vlo = std::min(vlo, std::max(0.0, (a.v + w * s).norm() - e)), vhi = std::max(vhi, std::max(va, vb) + e);
  }
  grow = 1.01 * grow + 50.0;
  return {Bbox{west, south, east, north}.grown(grow), vlo - 0.01, vhi + 0.01};
}

namespace {

bool overlaps(const Bbox& a, const Bbox& b) { return !(a.east < b.west || a.west > b.east || a.north < b.south || a.south > b.north); }

struct Piece {
  int seg = 0;
  double ta = 0, tb = 0;
  double lat_a = 0, lon_a = 0, h_a = 0, lat_b = 0, lon_b = 0, h_b = 0;
  double speed = 0;
  bool first = false, last = false;
};

struct Entry {
  int seg = -1;
  float r = INFINITY;
  float speed = 0;
  double t_emit = 0;
  bool edge = false;
};

struct Hit {  // a corridor entry that passed the range test
  int event, seg;
  double r, speed, t_rx, a, b;  // start interval [a, b]
};

struct Survivor {
  std::uint64_t cell;
  std::vector<std::pair<int, int>> combo;  // (event, seg)
  double a, b;
  double score = -INFINITY;
};

// Cuts every segment overlapping [wa, wb] into pieces of <= step seconds.
// Segments rejected by `pre` are skipped whole (it must reject only segments none of
// whose pieces `keep` would keep, or none of which could matter).
std::vector<Piece> make_pieces(const std::vector<Seg>& segs, double wa, double wb, double step, const std::function<bool(const Seg&, double)>& keep,
                               const std::function<bool(const Seg&)>& pre = {}) {
  std::vector<Piece> out;
  for (int si = 0; si < int(segs.size()); ++si) {
    const Seg& s = segs[std::size_t(si)];
    if (pre && !pre(s)) continue;
    const double a = std::max(wa, s.path.t0()), b = std::min(wb, s.path.t1());
    if (b - a < 1.0) continue;
    const int n = std::max(1, int(std::ceil((b - a) / step)));
    Vec3 p, v;
    s.path.state(a, p, v);
    Geodetic ga = geodetic(p);
    double sa = v.norm();
    for (int i = 1; i <= n; ++i) {
      const double tb = a + (b - a) * i / n;
      s.path.state(tb, p, v);
      const Geodetic gb = geodetic(p);
      const double sb = v.norm();
      // The first and last pieces are edges (segment end or window cut): a minimum there
      // is not a closest approach.
      Piece pc{si, a + (b - a) * (i - 1) / n, tb, ga.lat, ga.lon, ga.h, gb.lat, gb.lon, gb.h, 0.5 * (sa + sb), i == 1, i == n};
      if (keep(s, pc.speed)) out.push_back(pc);
      ga = gb, sa = sb;
    }
  }
  return out;
}

// Closest distance from the piece to each cell of its slab (the cells whose foot on
// the piece's line falls within it, with some overlap so turns leave no gaps) within
// horizontal distance rho of the line, into `map` (one segment's cells: the minimum over
// its pieces). Cells outside the tile or rejected by `want` are skipped.
// A piece's frame and footprint in one tile: the cells it rasterizes are exactly those
// `cols` admits (shared by the rasterizer, the pair masks and the exact re-evaluation).
struct PieceGeom {
  double rho = 0, mx = 0, bx = 0, by = 0, bz = 0, L2 = 0, Lh2 = 0, Lh = 0;
  i64 r0 = 0, r1 = -1;  // rows
};

PieceGeom piece_geom(const Piece& pc, const CellGrid& g, double rho, const Bbox& tile) {
  PieceGeom q;
  q.rho = rho;
  const double ca = std::cos(pc.lat_a * DEG);
  q.mx = ca * M_PER_DEG;
  q.bx = (pc.lon_b - pc.lon_a) * q.mx, q.by = (pc.lat_b - pc.lat_a) * M_PER_DEG, q.bz = pc.h_b - pc.h_a;
  q.L2 = q.bx * q.bx + q.by * q.by + q.bz * q.bz, q.Lh2 = q.bx * q.bx + q.by * q.by, q.Lh = std::sqrt(q.Lh2);
  const double dlat = rho / M_PER_DEG + g.dlat;
  const double lat_lo = std::max(tile.south, std::min(pc.lat_a, pc.lat_b) - dlat), lat_hi = std::min(tile.north, std::max(pc.lat_a, pc.lat_b) + dlat);
  if (lat_lo <= lat_hi) q.r0 = g.row(lat_lo), q.r1 = g.row(lat_hi);
  return q;
}

// Columns [c0, c1] of row r inside the piece's slab (the cells whose foot on the
// piece's line falls within it, with some overlap so turns leave no gaps) within
// horizontal distance rho of the line; false when none. Cell centres outside the tile
// must still be skipped by the caller.
bool piece_cols(const Piece& pc, const PieceGeom& q, const CellGrid& g, const Bbox& tile, i64 r, i64& c0, i64& c1) {
  constexpr double SMIN = -0.3, SMAX = 1.3;
  const double lat = g.lat_c(r);
  if (lat < tile.south || lat >= tile.north) return false;
  const double oy = (lat - pc.lat_a) * M_PER_DEG, rho = q.rho;
  // ox in [lo, hi] for k * ox in [u, v].
  auto solve = [](double k, double u, double v, double& lo, double& hi) {
    if (std::abs(k) < 1e-9) {
      if (u > 0 || v < 0) lo = 1, hi = 0;  // 0 * ox outside [u, v]: no solution
      else lo = -INFINITY, hi = INFINITY;
      return;
    }
    lo = u / k, hi = v / k;
    if (lo > hi) std::swap(lo, hi);
  };
  double lo = -rho, hi = rho;
  if (q.Lh > 1.0) {
    double l1, h1, l2, h2;
    solve(q.bx, SMIN * q.Lh2 - oy * q.by, SMAX * q.Lh2 - oy * q.by, l1, h1);  // inside the slab
    solve(q.by, oy * q.bx - rho * q.Lh, oy * q.bx + rho * q.Lh, l2, h2);      // within rho of the line
    lo = std::max({l1, l2, -rho - q.Lh}), hi = std::min({h1, h2, rho + q.Lh});
  } else if (std::abs(oy) > rho) {
    return false;
  }
  if (lo > hi) return false;
  const double lon_lo = std::max(tile.west, pc.lon_a + lo / q.mx), lon_hi = std::min(tile.east, pc.lon_a + hi / q.mx);
  if (lon_lo > lon_hi) return false;
  c0 = g.col(r, lon_lo), c1 = g.col(r, lon_hi);
  return true;
}

// Does the piece rasterize cell (r, c)?
bool piece_covers(const Piece& pc, const PieceGeom& q, const CellGrid& g, const Bbox& tile, i64 r, i64 c) {
  if (r < q.r0 || r > q.r1) return false;
  i64 c0, c1;
  if (!piece_cols(pc, q, g, tile, r, c0, c1) || c < c0 || c > c1) return false;
  const double lon = g.lon_c(r, c);
  return lon >= tile.west && lon < tile.east;
}

// The piece's closest distance to cell (r, c) folded into e (the minimum over pieces).
void piece_cell(const Piece& pc, const PieceGeom& q, const CellGrid& g, double mic, Terrain& terrain, i64 r, i64 c, Entry& e) {
  const double lat = g.lat_c(r), lon = g.lon_c(r, c);
  const double oy = (lat - pc.lat_a) * M_PER_DEG;
  const double ox = (lon - pc.lon_a) * q.mx, hor2 = ox * ox + oy * oy;
  // Listener height in the piece's tangent frame: ground + mic, minus Earth's curvature.
  const double oz = terrain.ground_coarse(lat, lon) + mic - pc.h_a - hor2 / (2 * EARTH_R);
  const double s_raw = q.L2 > 0 ? (ox * q.bx + oy * q.by + oz * q.bz) / q.L2 : 0.0;
  const double sc = std::clamp(s_raw, 0.0, 1.0);
  const double dx = sc * q.bx - ox, dy = sc * q.by - oy, dz = sc * q.bz - oz;
  const float rr = float(std::sqrt(dx * dx + dy * dy + dz * dz));
  if (rr < e.r) {
    e.seg = pc.seg;
    e.r = rr;
    e.speed = float(pc.speed);
    e.t_emit = pc.ta + sc * (pc.tb - pc.ta);
    e.edge = (pc.first && s_raw <= 0.0) || (pc.last && s_raw >= 1.0);
  }
}

// The columns of each row that hold wanted cells (a sparse set's hull per row).
struct RowSpans {
  i64 r0 = 0;
  std::vector<std::pair<i64, i64>> cols;  // per row from r0; first > second: none
  template <class Keys>
  explicit RowSpans(const Keys& keys) {
    i64 lo = std::numeric_limits<i64>::max(), hi = std::numeric_limits<i64>::min();
    for (auto k : keys) lo = std::min(lo, CellGrid::krow(k)), hi = std::max(hi, CellGrid::krow(k));
    if (lo > hi) return;
    r0 = lo;
    cols.assign(std::size_t(hi - lo + 1), {std::numeric_limits<i64>::max(), std::numeric_limits<i64>::min()});
    for (auto k : keys) {
      auto& [a, b] = cols[std::size_t(CellGrid::krow(k) - lo)];
      a = std::min(a, CellGrid::kcol(k)), b = std::max(b, CellGrid::kcol(k));
    }
  }
  // Narrows [c0, c1] to row r's wanted columns; false when none remain.
  bool clip(i64 r, i64& c0, i64& c1) const {
    if (r < r0 || r >= r0 + i64(cols.size())) return false;
    const auto& [a, b] = cols[std::size_t(r - r0)];
    c0 = std::max(c0, a), c1 = std::min(c1, b);
    return c0 <= c1;
  }
};

// Closest distance from the piece to each cell it covers into `map` (one segment's
// cells: the minimum over its pieces). Cells rejected by `want(key)` (or outside
// `spans`) are skipped.
template <class Want>
void rasterize(const Piece& pc, const CellGrid& g, double rho, double mic, Terrain& terrain, const Bbox& tile, Want&& want,
               std::unordered_map<std::uint64_t, Entry>& map, const RowSpans* spans = nullptr) {
  const PieceGeom q = piece_geom(pc, g, rho, tile);
  for (i64 r = q.r0; r <= q.r1; ++r) {
    i64 c0, c1;
    if (!piece_cols(pc, q, g, tile, r, c0, c1)) continue;
    if (spans && !spans->clip(r, c0, c1)) continue;
    for (i64 c = c0; c <= c1; ++c) {
      const double lon = g.lon_c(r, c);
      if (lon < tile.west || lon >= tile.east) continue;
      const std::uint64_t key = CellGrid::key(r, c);
      if (!want(key)) continue;
      piece_cell(pc, q, g, mic, terrain, r, c, map[key]);
    }
  }
}

bool in_reach(const Piece& pc, const Bbox& reach) {
  return !(std::max(pc.lat_a, pc.lat_b) < reach.south || std::min(pc.lat_a, pc.lat_b) > reach.north || std::max(pc.lon_a, pc.lon_b) < reach.west ||
           std::min(pc.lon_a, pc.lon_b) > reach.east);
}

// Every segment's closest approaches to the tile's cells: pieces are grouped by segment
// (make_pieces emits them in segment order); fn(cell, entry) once per (segment, cell).
// `want(key, piece index)` selects the cells each piece writes; `skip(piece index)`
// drops whole pieces.
template <class Want, class Skip, class Fn>
void corridors(const std::vector<Piece>& pieces, const CellGrid& g, double rho, double mic, Terrain& terrain, const Bbox& tile, Want&& want, Skip&& skip,
               const std::function<double(const Piece&)>& rho_of, Fn&& fn, const RowSpans* spans = nullptr) {
  std::unordered_map<std::uint64_t, Entry> map;
  int cur = -1;
  auto flush = [&] {
    for (auto& [k, e] : map) fn(k, e);
    map.clear();
  };
  const Bbox reach = tile.grown(rho);
  for (std::size_t i = 0; i < pieces.size(); ++i) {
    const Piece& pc = pieces[i];
    if (pc.seg != cur) flush(), cur = pc.seg;
    if (!in_reach(pc, reach) || skip(i)) continue;
    rasterize(pc, g, rho_of(pc), mic, terrain, tile, [&](std::uint64_t k) { return want(k, i); }, map, spans);
  }
  flush();
}
template <class Want, class Fn>
void corridors(const std::vector<Piece>& pieces, const CellGrid& g, double rho, double mic, Terrain& terrain, const Bbox& tile, Want&& want,
               const std::function<double(const Piece&)>& rho_of, Fn&& fn, const RowSpans* spans = nullptr) {
  corridors(pieces, g, rho, mic, terrain, tile, [&](std::uint64_t k, std::size_t) { return want(k); }, [](std::size_t) { return false; }, rho_of, fn, spans);
}

// One event's candidate pieces: flying at its speed in its window.
struct EvPieces {
  std::vector<Piece> pieces;
  double dlo, dhi, rho_max;
};

// A piece within reach of one tile, for the pair-first stage.
struct LocalPiece {
  std::size_t pi = 0;  // index in the event's pieces
  PieceGeom q;
  double a = INFINITY, b = -INFINITY;  // start interval any of its entries can imply
  double lon0 = 0, lon1 = 0;           // longitudes of the cells it covers (with slack)
  bool keep = false;
  std::vector<std::pair<int, int>> partners;  // (event, local index): compatible pieces of other events
};

struct PairStage {
  std::vector<std::vector<LocalPiece>> local;                            // per event, in piece order
  std::vector<std::unordered_map<int, std::vector<std::size_t>>> by_seg;  // per event: segment -> local indices
  std::size_t pieces = 0, kept = 0, pairs = 0;

  // Can cell `key` be in a combination through local piece li of event ei? Only if
  // kept partners of >= min_events - 1 other events cover it too.
  bool mask(const std::vector<EvPieces>& evp, const CellGrid& g, const Bbox& tile, int min_events, std::size_t ei, std::size_t li,
            std::uint64_t key) const {
    const LocalPiece& l = local[ei][li];
    if (l.partners.size() > 64) return true;  // busy airspace: testing costs more than it saves
    const i64 r = CellGrid::krow(key), c = CellGrid::kcol(key);
    std::uint64_t seen = 0;
    int n = 0;
    for (auto [ej, lj] : l.partners) {
      if ((seen >> ej) & 1) continue;
      const LocalPiece& m = local[std::size_t(ej)][std::size_t(lj)];
      if (!m.keep || !piece_covers(evp[std::size_t(ej)].pieces[m.pi], m.q, g, tile, r, c)) continue;
      seen |= std::uint64_t(1) << ej;
      if (++n >= min_events - 1) return true;
    }
    return false;
  }
};

// Pieces of each event within reach of the tile, paired with those of other events
// whose start intervals overlap and whose footprints can share a cell; pieces with
// partners in fewer than min_events - 1 other events are dropped (repeatedly: a
// dropped piece is no one's partner).
template <class RhoOf>
PairStage pair_stage(const std::vector<Event>& ev, const std::vector<EvPieces>& evp, const CellGrid& g, const Bbox& tile, const SearchOptions& o,
                     int min_events, RhoOf&& rho_of) {
  PairStage ps;
  const std::size_t n = ev.size();
  ps.local.resize(n), ps.by_seg.resize(n);
  const double lat_abs = std::min(89.9, std::max(std::abs(tile.south), std::abs(tile.north)));
  const double slack = 2 * g.dlat / std::max(0.05, std::cos(lat_abs * DEG));  // a cell centre beyond the span's ends
  std::vector<double> maxlen(n, 0.0);
  for (std::size_t ei = 0; ei < n; ++ei) {
    const Event& e = ev[ei];
    const EvPieces& ep = evp[ei];
    const Bbox reach = tile.grown(ep.rho_max);
    auto& loc = ps.local[ei];
    for (std::size_t i = 0; i < ep.pieces.size(); ++i) {
      const Piece& pc = ep.pieces[i];
      if (!in_reach(pc, reach)) continue;
      LocalPiece l;
      l.pi = i;
      const double rho = rho_of(ei, pc);
      l.q = piece_geom(pc, g, rho, tile);
      if (l.q.r0 <= l.q.r1) {
        const double a = std::max(o.start_min, pc.ta + ep.dlo / o.c - e.tc_hi - o.tc_tol_s) - 1e-3;
        const double b = std::min(o.start_max, pc.tb + ep.dhi / o.c - e.tc_lo + o.tc_tol_s) + 1e-3;
        if (a <= b) {
          const double w = (rho + l.q.Lh) / l.q.mx;
          l.a = a, l.b = b, l.keep = true;
          l.lon0 = std::max(tile.west, pc.lon_a - w) - slack, l.lon1 = std::min(tile.east, pc.lon_a + w) + slack;
          maxlen[ei] = std::max(maxlen[ei], b - a);
        }
      }
      ps.by_seg[ei][pc.seg].push_back(loc.size());
      loc.push_back(std::move(l));
    }
    ps.pieces += loc.size();
  }
  // Pairs: by start interval (sorted), then rows and longitudes.
  std::vector<std::vector<std::size_t>> by_a(n);
  for (std::size_t ei = 0; ei < n; ++ei) {
    for (std::size_t li = 0; li < ps.local[ei].size(); ++li)
      if (ps.local[ei][li].keep) by_a[ei].push_back(li);
    std::sort(by_a[ei].begin(), by_a[ei].end(), [&](auto x, auto y) { return ps.local[ei][x].a < ps.local[ei][y].a; });
  }
  for (std::size_t ei = 0; ei < n; ++ei)
    for (std::size_t ej = ei + 1; ej < n; ++ej) {
      auto& lj = ps.local[ej];
      const auto& oj = by_a[ej];
      for (std::size_t pi : by_a[ei]) {
        LocalPiece& p = ps.local[ei][pi];
        auto it = std::lower_bound(oj.begin(), oj.end(), p.a - maxlen[ej], [&](std::size_t x, double v) { return lj[x].a < v; });
        for (; it != oj.end() && lj[*it].a <= p.b; ++it) {
          LocalPiece& q = lj[*it];
          if (q.b < p.a || q.q.r1 < p.q.r0 || p.q.r1 < q.q.r0 || q.lon1 < p.lon0 || p.lon1 < q.lon0) continue;
          p.partners.emplace_back(int(ej), int(*it));
          q.partners.emplace_back(int(ei), int(pi));
          ++ps.pairs;
        }
      }
    }
  for (int iter = 0; iter < 16; ++iter) {
    bool changed = false;
    for (std::size_t ei = 0; ei < n; ++ei)
      for (auto& p : ps.local[ei]) {
        if (!p.keep) continue;
        std::uint64_t seen = 0;
        for (auto [ej, lj] : p.partners)
          if (ps.local[std::size_t(ej)][std::size_t(lj)].keep) seen |= std::uint64_t(1) << ej;
        if (std::popcount(seen) < min_events - 1) p.keep = false, changed = true;
      }
    if (!changed) break;
  }
  for (auto& loc : ps.local)
    for (auto& p : loc) ps.kept += p.keep;
  return ps;
}

// Curve of f/f0 against absolute reception time for one path and listener, tabulated.
struct RatioTable {
  double t0 = 0, dt = 0.032;
  std::vector<float> r;  // NaN where the path does not cover the emission
  float at(double t) const {
    const double x = (t - t0) / dt;
    if (x < 0 || x > double(r.size() - 1)) return NAN;
    const std::size_t i = std::min(r.size() - 2, std::size_t(x));
    const double w = x - double(i);
    return float((1 - w) * r[i] + w * r[i + 1]);
  }
};

RatioTable ratio_table(const Path& path, const Vec3& o, double ta, double tb, double c, double dt = 0.032) {
  RatioTable t;
  t.t0 = ta, t.dt = dt;
  const std::size_t n = std::size_t(std::max(2.0, std::ceil((tb - ta) / t.dt) + 1));
  t.r.resize(n, NAN);
  double guess = NAN;  // each solve starts from the previous emission time
  for (std::size_t i = 0; i < n; ++i)
    if (auto em = emission_for(path, o, ta + double(i) * t.dt, c, std::isfinite(guess) ? guess + t.dt : NAN))
      t.r[i] = float(em->ratio(c)), guess = em->tau;
    else
      guess = NAN;
  return t;
}

// Score of one event against a tabulated curve, recording start `start`, rest-frequency
// offset delta (relative): mean z along every line, times sqrt(frames x lines).
// Frames of an event's window, every `step`-th.
std::pair<int, int> frames_of(const Spectrogram& s, const Event& e) {
  return {std::max(0, int(std::ceil(s.frame_at(e.tc - e.half_window_s)))), std::min(s.nf - 1, int(std::floor(s.frame_at(e.tc + e.half_window_s))))};
}

// Ridge evidence at frame j for model frequency f: the strongest bin within a few
// sigma, its frequency refined by a parabola through its neighbours, weighted by how
// close the model passes to it (Gaussian, sigma ~0.4%: ADS-B, wind and sound-speed
// errors). Brightness alone saturates within a bin or two of a strong tone; this keeps
// discriminating at a tenth of a bin. Frames without a ridge (peak z < 1.5) give 0.
float ridge_at(const Spectrogram& s, int j, double f) {
  const double sigma = std::max(0.3, 0.004 * f);
  const double b = s.bin_at(f), R = std::max(1.5, 3 * sigma) / s.df;
  const int lo = std::max(1, int(std::floor(b - R))), hi = std::min(s.nb - 2, int(std::ceil(b + R)));
  if (j < 0 || j >= s.nf || lo > hi) return 0.0f;
  const float* row = s.row(j);
  int k = lo;
  for (int i = lo + 1; i <= hi; ++i)
    if (row[i] > row[k]) k = i;
  if (row[k] < 1.5f) return 0.0f;
  const double ym = row[k - 1], y0 = row[k], yp = row[k + 1], den = ym - 2 * y0 + yp;
  const double p = den < 0 ? std::clamp(0.5 * (ym - yp) / den, -0.5, 0.5) : 0.0;
  const double fpk = s.freq(k + p), zpk = y0 - 0.25 * (ym - yp) * p;
  const double d = (f - fpk) / sigma;
  return float(zpk * std::exp(-0.5 * d * d));
}

enum class Metric { Brightness, Ridge };

// Sum along every line for rest-frequency offset delta, given the curve's f/f0 at each
// scored frame; normalised by sqrt(frames x lines) (missing coverage counts as 0).
double sum_lines(const Spectrogram& s, const Event& e, int j0, int step, const std::vector<float>& ratio, double delta,
                 Metric m = Metric::Brightness) {
  double sum = 0;
  for (std::size_t i = 0; i < ratio.size(); ++i) {
    if (!std::isfinite(ratio[i])) continue;
    const int j = j0 + int(i) * step;
    for (auto& l : e.lines) {
      const double f = l.f0 * (1 + delta) * ratio[i];
      const float z = m == Metric::Ridge ? ridge_at(s, j, f) : s.at(j, f);
      if (std::isfinite(z)) sum += z;
    }
  }
  return sum / std::sqrt(double(ratio.size() * std::max<std::size_t>(1, e.lines.size())));
}

void ratios_at(const Spectrogram& s, const Event& e, const RatioTable& rt, double start, int step, std::vector<float>& out, int& j0) {
  auto [a, b] = frames_of(s, e);
  j0 = a;
  out.clear();
  for (int j = a; j <= b; j += step) out.push_back(rt.at(start + s.frame_time(j)));
}

struct JointFit {
  double score = -INFINITY, start = NAN;
  std::vector<double> scores, deltas;
};

struct FitQuality {
  int frame_step;     // every n-th frame
  double t_step;      // start-time grid
  double d_step;      // rest-frequency grid (relative)
  bool refine;        // then a fine pass around the best, scored on ridges
  double table_dt;    // curve tabulation step
};
constexpr FitQuality QUICK{2, 0.5, 0.004, false, 0.128}, FULL{1, 0.25, 0.0025, true, 0.032};

// One clock for every matched event: start swept over [a, b], each event's rest
// frequency swept on its own; coarse grid, then (FULL) a fine one around the best.
JointFit joint_fit(const Spectrogram& s, const std::vector<Event>& ev, const std::vector<std::pair<int, const Path*>>& combo, const Vec3& o, double a,
                   double b, double c, const FitQuality& q = FULL, double sweep = 0.02) {
  std::vector<RatioTable> tabs;
  for (auto& [ei, path] : combo) {
    const Event& e = ev[std::size_t(ei)];
    tabs.push_back(ratio_table(*path, o, a + e.tc - e.half_window_s - 1, b + e.tc + e.half_window_s + 1, c, q.table_dt));
  }
  std::vector<float> ratio;
  auto eval = [&](double start, int fstep, double dstep, double dspan, const std::vector<double>* centre, std::vector<double>& sc,
                  std::vector<double>& dl, Metric m = Metric::Brightness) {
    double tot = 0;
    for (std::size_t k = 0; k < combo.size(); ++k) {
      const Event& e = ev[std::size_t(combo[k].first)];
      int j0 = 0;
      ratios_at(s, e, tabs[k], start, fstep, ratio, j0);
      double best = -INFINITY, bd = 0;
      const double c0 = centre ? (*centre)[k] : 0.0;
      for (double d = c0 - dspan; d <= c0 + dspan + 1e-12; d += dstep) {
        const double v = sum_lines(s, e, j0, fstep, ratio, d, m);
        if (v > best) best = v, bd = d;
      }
      sc[k] = best, dl[k] = bd, tot += best;
    }
    return tot;
  };
  JointFit f;
  f.scores.assign(combo.size(), 0), f.deltas.assign(combo.size(), 0);
  std::vector<double> sc(combo.size()), dl(combo.size());
  for (double t = a; t <= b + 1e-9; t += q.t_step) {
    const double tot = eval(t, q.frame_step, q.d_step, sweep, nullptr, sc, dl);
    if (tot > f.score) f.score = tot, f.start = t, f.scores = sc, f.deltas = dl;
  }
  if (!std::isfinite(f.start) || !q.refine) return f;
  const JointFit coarse = f;
  f.score = -INFINITY;
  for (double t = coarse.start - q.t_step; t <= coarse.start + q.t_step + 1e-9; t += 0.032) {
    if (t < a - 1e-9 || t > b + 1e-9) continue;
    const double tot = eval(t, 1, 0.0005, 0.004, &coarse.deltas, sc, dl, Metric::Ridge);
    if (tot > f.score) f.score = tot, f.start = t, f.scores = sc, f.deltas = dl;
  }
  return f;
}

// All combinations, one entry per event at most, of >= min_events events whose start
// intervals intersect.
using PairOk = std::function<bool(int ev_a, int seg_a, int ev_b, int seg_b)>;
void combos(const std::vector<std::vector<Hit>>& by_event, std::size_t i, std::vector<std::pair<int, int>>& cur, double a, double b, int min_events,
            std::vector<Survivor>& out, std::uint64_t cell, std::size_t cap, const PairOk& pair_ok) {
  if (out.size() >= cap) return;
  if (i == by_event.size()) {
    if (int(cur.size()) >= min_events) out.push_back({cell, cur, a, b});
    return;
  }
  for (auto& h : by_event[i]) {
    const double na = std::max(a, h.a), nb = std::min(b, h.b);
    if (na > nb) continue;
    if (pair_ok && std::any_of(cur.begin(), cur.end(), [&](auto& p) { return !pair_ok(p.first, p.second, h.event, h.seg); })) continue;
    cur.emplace_back(h.event, h.seg);
    combos(by_event, i + 1, cur, na, nb, min_events, out, cell, cap, pair_ok);
    cur.pop_back();
  }
  // This event unexplained (if enough remain to reach min_events).
  if (int(cur.size() + by_event.size() - i - 1) >= min_events) combos(by_event, i + 1, cur, a, b, min_events, out, cell, cap, pair_ok);
}

std::string fmt_utc(double t) { return std::isfinite(t) ? iso(t) : "?"; }

}  // namespace

void judge(SearchResult& res) {
  res.reasons.erase(std::remove_if(res.reasons.begin(), res.reasons.end(), [](const std::string& r) { return r.find("explained;") != std::string::npos; }),
                    res.reasons.end());
  if (res.candidates.empty()) {
    res.level = AirLevel::None;
    return;
  }
  // How well the real trajectories explain the curves, and by how much the leading
  // combination of flights beats the next.
  const Candidate& top = res.candidates.front();
  double min_fit = INFINITY;
  for (auto& m : top.matches) min_fit = std::min(min_fit, m.fit);
  const double margin = res.candidates.size() > 1 ? (top.score - res.candidates[1].score) / std::max(1e-9, top.score) : 1.0;
  const int nm = int(top.matches.size());
  char b[200];
  std::snprintf(b, sizeof b, "%d flyover(s) explained; weakest fit %.2f of a straight-pass fit; lead over the next flight combination %.0f%%", nm, min_fit,
                100 * margin);
  res.reasons.insert(res.reasons.begin(), b);
  if (nm >= 2 && min_fit >= 0.7 && margin >= 0.2) res.level = AirLevel::Strong;
  else if (min_fit >= 0.5 && margin >= 0.1) res.level = AirLevel::Moderate;
  else res.level = AirLevel::Weak;
  if (nm < 2 && res.level == AirLevel::Strong) res.level = AirLevel::Moderate;
}

SearchResult search_regions(const Spectrogram& spec, const std::vector<Event>& events, const TrackSource& tracks_for, Terrain& terrain,
                            const SearchOptions& o, double region_deg, const std::function<void(const std::string&)>& progress) {
  std::vector<Bbox> regions;
  // A narrow time window (a hum second) leaves few tracks even over a continent: one pass,
  // parallel over the whole box, beats loading it region by region.
  if (o.start_max - o.start_min < 6 * 3600.0) region_deg = std::max({region_deg, o.box.east - o.box.west, o.box.north - o.box.south});
  for (double la = o.box.south; la < o.box.north; la += region_deg)
    for (double lo = o.box.west; lo < o.box.east; lo += region_deg)
      regions.push_back({lo, la, std::min(lo + region_deg, o.box.east), std::min(la + region_deg, o.box.north)});
  SearchResult all;
  std::map<std::string, std::size_t> by_combo;
  // Per-region budgets shrink with the number of regions (a wide box would otherwise
  // refine every region's local best), and a region's survivors must come within 60% of
  // the best answer found so far to be refined at all.
  const double share = std::min(1.0, 4.0 / double(std::max<std::size_t>(1, regions.size())));
  double best_so_far = -INFINITY;
  for (std::size_t i = 0; i < regions.size(); ++i) {
    SearchOptions ro = o;
    ro.box = regions[i];
    ro.max_scored = std::max(100, int(o.max_scored * share));
    ro.max_subgrid = std::max(30, int(o.max_subgrid * share));
    ro.max_refine = regions.size() > 4 ? 2 : o.max_refine;
    if (std::isfinite(best_so_far)) ro.score_floor = std::max(o.score_floor, 0.6 * best_so_far);
    const auto t_load = std::chrono::steady_clock::now();
    auto tracks = tracks_for(regions[i].grown(std::max(15000.0, 2 * o.silence_factor * 9000.0)));
    all.wall_load += std::chrono::duration<double>(std::chrono::steady_clock::now() - t_load).count();
    if (progress) {
      char b[160];
      std::snprintf(b, sizeof b, "region %zu/%zu [%.1f,%.1f,%.1f,%.1f]: %zu flight segments", i + 1, regions.size(), regions[i].west, regions[i].south,
                    regions[i].east, regions[i].north, tracks.size());
      progress(b);
    }
    if (tracks.empty()) continue;
    auto r = search(spec, events, tracks, terrain, ro);
    all.segments += r.segments, all.corridor_cells += r.corridor_cells, all.consistent_cells += r.consistent_cells;
    all.silent_cells += r.silent_cells, all.scored += r.scored;
    all.type_pairs += r.type_pairs, all.type_pruned_pairs += r.type_pruned_pairs, all.type_pruned_segments += r.type_pruned_segments;
    all.cpu_corridors += r.cpu_corridors, all.cpu_centre += r.cpu_centre, all.cpu_pairs += r.cpu_pairs;
    all.pair_pieces += r.pair_pieces, all.pair_kept += r.pair_kept, all.cpu_silence += r.cpu_silence, all.wall_pieces += r.wall_pieces;
    all.wall_tiles += r.wall_tiles, all.wall_subgrid += r.wall_subgrid, all.wall_refine += r.wall_refine, all.seconds += r.seconds;
    all.events_used = r.events_used;
    for (auto& c : r.candidates) {
      best_so_far = std::max(best_so_far, c.score);
      auto it = by_combo.find(c.combo);
      if (it == by_combo.end()) by_combo[c.combo] = all.candidates.size(), all.candidates.push_back(std::move(c));
      else if (c.score > all.candidates[it->second].score) all.candidates[it->second] = std::move(c);
    }
    for (auto& why : r.reasons)
      if (why.find("explained;") == std::string::npos && std::find(all.reasons.begin(), all.reasons.end(), why) == all.reasons.end())
        all.reasons.push_back(why);
  }
  std::sort(all.candidates.begin(), all.candidates.end(), [](const Candidate& a, const Candidate& b) { return a.score > b.score; });
  if (int(all.candidates.size()) > o.top_k) all.candidates.resize(std::size_t(o.top_k));
  if (!all.candidates.empty())
    all.reasons.erase(std::remove(all.reasons.begin(), all.reasons.end(), "no location heard matching aircraft at these times"), all.reasons.end());
  all.terrain_complete = terrain.complete();
  judge(all);
  return all;
}

Path path_of(const Track& tr) {
  std::vector<PathPoint> pts;
  for (auto& f : tr.fixes) {
    if (!std::isfinite(f.alt_m)) continue;
    Vec3 v{NAN, NAN, NAN};
    if (std::isfinite(f.gs) && std::isfinite(f.track)) v = velocity_ecef(f.lat, f.lon, f.gs, f.track, std::isfinite(f.vrate) ? f.vrate : 0.0f);
    pts.push_back({f.t, ecef(f.lat, f.lon, f.alt_m), v});
  }
  if (pts.size() < 2) return {};
  return Path(std::move(pts));
}

const char* air_level_name(AirLevel l) {
  switch (l) {
    case AirLevel::Strong: return "strong";
    case AirLevel::Moderate: return "moderate";
    case AirLevel::Weak: return "weak";
    default: return "none";
  }
}

std::pair<double, double> event_score(const Spectrogram& spec, const Event& e, const Path& path, const Vec3& o, double start, double c, double sweep,
                                      bool ridge) {
  auto rt = ratio_table(path, o, start + e.tc - e.half_window_s - 1, start + e.tc + e.half_window_s + 1, c);
  std::vector<float> ratio;
  int j0 = 0;
  ratios_at(spec, e, rt, start, 1, ratio, j0);
  double best = -INFINITY, bd = 0;
  for (double d = -sweep; d <= sweep + 1e-12; d += 0.0005) {
    const double v = sum_lines(spec, e, j0, 1, ratio, d, ridge ? Metric::Ridge : Metric::Brightness);
    if (v > best) best = v, bd = d;
  }
  return {best, e.lines.empty() ? 0.0 : e.lines[0].f0 * (1 + bd)};
}

SearchResult search(const Spectrogram& spec, const std::vector<Event>& all_events, const std::vector<Track>& tracks, Terrain& terrain,
                    const SearchOptions& o) {
  const auto clock0 = std::chrono::steady_clock::now();
  SearchResult res;
  if (!std::isfinite(o.start_min) || !std::isfinite(o.start_max) || o.start_max < o.start_min)
    throw std::invalid_argument("aircraft search needs a window for the recording's start");
  // The strongest events, kept in time order.
  std::vector<int> order(all_events.size());
  for (std::size_t i = 0; i < order.size(); ++i) order[i] = int(i);
  std::sort(order.begin(), order.end(), [&](int a, int b) { return all_events[std::size_t(a)].score > all_events[std::size_t(b)].score; });
  if (int(order.size()) > o.max_events) order.resize(std::size_t(o.max_events));
  std::sort(order.begin(), order.end());
  std::vector<Event> ev;
  for (int i : order) ev.push_back(all_events[std::size_t(i)]), res.events_used.push_back(i);
  // Reference: each event's ridge score along its best straight pass (tc and rest
  // frequency polished), the scale of the trajectory scores' fit ratios.
  for (auto& e : ev) {
    e.half_window_s = std::min(o.max_window_s, e.half_window_s * o.window_factor);
    auto [j0, j1] = frames_of(spec, e);
    std::vector<float> ratio;
    double best = -INFINITY;
    for (double dt = -0.3; dt <= 0.3 + 1e-9; dt += 0.032) {
      ratio.clear();
      for (int j = j0; j <= j1; ++j) ratio.push_back(float(cv_ratio(spec.frame_time(j), e.tc + dt, e.v, e.d, o.c)));
      for (double d = -0.004; d <= 0.004 + 1e-12; d += 0.0005) best = std::max(best, sum_lines(spec, e, j0, 1, ratio, d, Metric::Ridge));
    }
    e.score = best;
  }
  const int min_events = std::min<int>(o.min_events, int(ev.size()));
  if (ev.empty() || min_events < 1) {
    res.reasons.push_back("no flyover to match");
    return res;
  }

  // Segments as ECEF paths.
  std::vector<Seg> segs;
  for (auto& tr : tracks) {
    Path path = path_of(tr);
    if (path.empty()) continue;
    const bool eligible = tr.info.engine != EngineType::Jet && (o.unknown_types || tr.info.engine != EngineType::Unknown);
    Seg s{&tr, std::move(path), eligible};
    segs.push_back(std::move(s));
  }
  parallel_for(segs.size(), [&](std::size_t i) {
    const auto b = path_bounds(segs[i].path);
    segs[i].box = b.box, segs[i].vlo = b.vlo, segs[i].vhi = b.vhi;
  });
  res.segments = segs.size();
  const auto t_pieces = std::chrono::steady_clock::now();

  // Rest-frequency prior: which events each segment's type can sound like.
  std::vector<std::vector<double>> ev_f0s(ev.size());
  for (std::size_t i = 0; i < ev.size(); ++i) {
    for (auto& l : ev[i].lines) ev_f0s[i].push_back(l.f0);
    if (ev_f0s[i].empty() && ev[i].f0 > 0) ev_f0s[i].push_back(ev[i].f0);
  }
  BladePassTable loaded;
  if (o.type_prior && !o.blade_pass) loaded = BladePassTable::load();
  const BladePassTable& bp = o.blade_pass ? *o.blade_pass : loaded;
  for (auto& s : segs) {
    s.f0_ok.assign(ev.size(), 1);
    if (!o.type_prior || !s.eligible) continue;
    int dropped = 0;
    for (std::size_t i = 0; i < ev.size(); ++i) {
      s.f0_ok[i] = bp.compatible(s.tr->type, ev_f0s[i], o.prior);
      dropped += !s.f0_ok[i];
    }
    res.type_pairs += ev.size(), res.type_pruned_pairs += std::size_t(dropped);
    res.type_pruned_segments += dropped == int(ev.size());
  }
  // One aircraft heard twice: its two tones must agree (its RPM is nearly fixed).
  PairOk pair_ok;
  if (o.type_prior && o.same_aircraft_f0_tol > 0)
    pair_ok = [&](int ea, int sa, int eb, int sb) {
      return segs[std::size_t(sa)].tr->icao != segs[std::size_t(sb)].tr->icao ||
             f0_consistent(ev_f0s[std::size_t(ea)], ev_f0s[std::size_t(eb)], o.same_aircraft_f0_tol);
    };

  // Per event: pieces flying at the right speed in the right window.
  const CellGrid grid(o.cell_m);
  std::vector<EvPieces> evp(ev.size());
  double max_d = 0;
  for (std::size_t i = 0; i < ev.size(); ++i) {
    const Event& e = ev[i];
    EvPieces& ep = evp[i];
    ep.dlo = e.d_lo / o.d_factor, ep.dhi = e.d_hi * o.d_factor, ep.rho_max = ep.dhi;
    max_d = std::max(max_d, e.d);
    const double wa = o.start_min + e.tc_lo - o.tc_tol_s - ep.dhi / o.c - 5, wb = o.start_max + e.tc_hi + o.tc_tol_s;
    const double vlo = e.v_lo - o.v_margin, vhi = e.v_hi + o.v_margin;
    // A piece's speed is the mean of the curve's at its ends: inside the segment's bounds.
    ep.pieces = make_pieces(
        segs, wa, wb, 4.0, [&](const Seg& s, double sp) { return s.eligible && s.f0_ok[i] && sp >= vlo && sp <= vhi; },
        [&](const Seg& s) { return s.eligible && s.f0_ok[i] && (!o.segment_bounds || (s.vhi >= vlo && s.vlo <= vhi)); });
  }
  const double silence_m = o.silence_factor * max_d;
  res.wall_pieces = std::chrono::duration<double>(std::chrono::steady_clock::now() - t_pieces).count();
  // Horizontal reach: no farther than the slant range allows at the piece's height.
  auto rho_of = [&](std::size_t ei, const Piece& pc) {
    const double dhi = evp[ei].dhi;
    const double agl = std::min(pc.h_a, pc.h_b) - terrain.ground_coarse(pc.lat_a, pc.lon_a) - 300.0;
    return std::sqrt(std::max(0.0, dhi * dhi - std::max(0.0, agl) * std::max(0.0, agl))) + o.cell_m;
  };
  const bool pair_first = o.pair_first && min_events >= 2 && ev.size() <= 64;

  // Tiles of the search box.
  std::vector<Bbox> tiles;
  for (double la = std::floor(o.box.south / TILE_DEG) * TILE_DEG; la < o.box.north; la += TILE_DEG)
    for (double lo = std::floor(o.box.west / TILE_DEG) * TILE_DEG; lo < o.box.east; lo += TILE_DEG)
      tiles.push_back({std::max(lo, o.box.west), std::max(la, o.box.south), std::min(lo + TILE_DEG, o.box.east), std::min(la + TILE_DEG, o.box.north)});
  std::mutex mu;
  std::vector<Survivor> survivors;
  std::size_t n_corr = 0, n_cons = 0, n_silent = 0, n_pp = 0, n_pk = 0;
  double cpu_corr = 0, cpu_centre = 0, cpu_pairs = 0, cpu_silence = 0;
  auto now = [] { return std::chrono::steady_clock::now(); };
  auto secs = [](auto a, auto b) { return std::chrono::duration<double>(b - a).count(); };
  const auto t_tiles = now();
  parallel_for(tiles.size(), [&](std::size_t ti) {
    const auto t_tile = now();
    const Bbox& tile = tiles[ti];
    auto want_area = [&](std::uint64_t k) {
      if (!o.inside) return true;
      const i64 r = CellGrid::krow(k);
      return o.inside(grid.lat_c(r), grid.lon_c(r, CellGrid::kcol(k)));
    };
    // 1. Corridors.
    std::unordered_map<std::uint64_t, std::vector<std::vector<Hit>>> cells;  // cell -> hits per event
    std::unordered_map<std::uint64_t, bool> area_ok;
    auto want = [&](std::uint64_t k) {
      if (!o.inside) return true;
      auto it = area_ok.find(k);
      if (it != area_ok.end()) return it->second;
      return area_ok[k] = want_area(k);
    };
    // An entry's hit, or nothing when it fails the event's range, edge or clock tests.
    auto hit_of = [&](std::size_t ei, const Entry& en) -> std::optional<Hit> {
      const Event& e = ev[ei];
      const EvPieces& ep = evp[ei];
      if (en.seg < 0 || en.edge || en.r < ep.dlo || en.r > ep.dhi) return std::nullopt;
      const double t_rx = en.t_emit + en.r / o.c;
      const double a = std::max(o.start_min, t_rx - e.tc_hi - o.tc_tol_s), b = std::min(o.start_max, t_rx - e.tc_lo + o.tc_tol_s);
      if (a > b) return std::nullopt;
      return Hit{int(ei), en.seg, en.r, en.speed, t_rx, a, b};
    };
    // Pair-first (two or more flyovers): a cell can only survive the one-clock test
    // where pieces of >= min_events events overlap in space and in the start time they
    // imply. Pieces with too few such partners are not rasterized, and the others only
    // into cells their partners cover; the entries of cells that could still form a
    // combination are then re-evaluated over all of their segment's pieces, so the
    // survivors are exactly those of rasterizing everything.
    PairStage ps;
    if (pair_first) ps = pair_stage(ev, evp, grid, tile, o, min_events, rho_of);
    const auto t_pairs = now();
    for (std::size_t ei = 0; ei < ev.size(); ++ei) {
      const EvPieces& ep = evp[ei];
      auto rho_e = [&](const Piece& pc) { return rho_of(ei, pc); };
      auto emit = [&](std::uint64_t k, const Entry& en) {
        auto h = hit_of(ei, en);
        if (!h) return;
        auto& per = cells[k];
        per.resize(ev.size());
        per[ei].push_back(*h);
      };
      if (!pair_first) {
        corridors(ep.pieces, grid, ep.rho_max, o.mic_height, terrain, tile, want, rho_e, emit);
        continue;
      }
      const auto& loc = ps.local[ei];
      std::size_t li = 0;  // the local index of the piece being rasterized (pieces in reach are all listed, in order)
      corridors(
          ep.pieces, grid, ep.rho_max, o.mic_height, terrain, tile,
          [&](std::uint64_t k, std::size_t) { return want(k) && ps.mask(evp, grid, tile, min_events, ei, li, k); },
          [&](std::size_t i) {
            li = std::size_t(std::lower_bound(loc.begin() + std::ptrdiff_t(li), loc.end(), i, [](const LocalPiece& l, std::size_t v) { return l.pi < v; }) -
                             loc.begin());
            return !loc[li].keep;
          },
          rho_e, emit);
    }
    if (pair_first) {
      for (auto& [k, per] : cells) {
        int n_ev = 0;
        for (auto& h : per) n_ev += !h.empty();
        if (n_ev < min_events) continue;
        const i64 r = CellGrid::krow(k), c = CellGrid::kcol(k);
        for (std::size_t ei = 0; ei < per.size(); ++ei) {
          std::vector<Hit> exact;
          for (auto& h : per[ei]) {
            Entry en;
            for (std::size_t li : ps.by_seg[ei][h.seg]) {
              const LocalPiece& l = ps.local[ei][li];
              const Piece& pc = evp[ei].pieces[l.pi];
              if (piece_covers(pc, l.q, grid, tile, r, c)) piece_cell(pc, l.q, grid, o.mic_height, terrain, r, c, en);
            }
            if (auto x = hit_of(ei, en)) exact.push_back(*x);
          }
          per[ei] = std::move(exact);
        }
      }
    }
    // 2. One clock.
    std::vector<Survivor> surv;
    std::size_t corr = cells.size();
    for (auto& [k, per] : cells) {
      std::vector<std::pair<int, int>> cur;
      combos(per, 0, cur, -INFINITY, INFINITY, min_events, surv, k, surv.size() + 64, pair_ok);
    }
    std::size_t cons = 0;
    {
      std::unordered_set<std::uint64_t> sc;
      for (auto& s : surv) sc.insert(s.cell);
      cons = sc.size();
    }
    const auto t_silence = now();
    // 3. Silence: another aircraft passing closer than the matched flyovers, at a time no
    //    flyover was detected (in the whole recording, or between the first and last
    //    matched flyover), would have been heard: the cell is dropped.
    if (o.silence && !surv.empty()) {
      std::unordered_set<std::uint64_t> want_cells;
      double ta = INFINITY, tb = -INFINITY;
      for (auto& s : surv) {
        want_cells.insert(s.cell);
        if (o.silence_whole) ta = std::min(ta, s.a), tb = std::max(tb, s.b + spec.duration_s);
        else
          for (auto& [ei, sg] : s.combo) ta = std::min(ta, s.a + ev[std::size_t(ei)].tc), tb = std::max(tb, s.b + ev[std::size_t(ei)].tc);
      }
      if (tb > ta) {
        // Only segments that can come within reach of the tile (the others' pieces all
        // fail the corridor's reach test).
        const Bbox reach = tile.grown(silence_m);
        auto pieces = make_pieces(
            segs, ta - silence_m / o.c - 5, tb, 6.0, [](const Seg&, double) { return true; }, [&](const Seg& s) { return !o.segment_bounds || overlaps(s.box, reach); });
        std::unordered_map<std::uint64_t, std::vector<std::pair<int, double>>> passes;  // cell -> (segment, reception time of CPA)
        auto want_s = [&](std::uint64_t k) { return want_cells.count(k) > 0; };
        const RowSpans spans(want_cells);  // (exact: cells outside them are not wanted)
        corridors(
            pieces, grid, silence_m, o.mic_height, terrain, tile, want_s, [&](const Piece&) { return silence_m + o.cell_m; },
            [&](std::uint64_t k, const Entry& en) {
              if (!en.edge && en.r <= silence_m) passes[k].emplace_back(en.seg, en.t_emit + en.r / o.c);
            },
            &spans);
        std::vector<Survivor> kept;
        for (auto& s : surv) {
          const double start = 0.5 * (s.a + s.b);
          double first = INFINITY, last = -INFINITY;
          std::set<std::uint32_t> mine;
          for (auto& [ei, sg] : s.combo) {
            const double t = start + ev[std::size_t(ei)].tc;
            first = std::min(first, t), last = std::max(last, t);
            mine.insert(segs[std::size_t(sg)].tr->icao);
          }
          if (o.silence_whole) first = start, last = start + spec.duration_s;
          bool loud = false;
          if (auto it = passes.find(s.cell); it != passes.end())
            for (auto& [sg, t_rx] : it->second) {
              if (mine.count(segs[std::size_t(sg)].tr->icao) || t_rx < first + 30 || t_rx > last - 30) continue;
              bool heard = false;  // a detected flyover (matched or not) at that time explains it
              for (auto& e : all_events) heard |= std::abs(t_rx - (start + e.tc)) < std::max(30.0, e.half_window_s);
              if (!heard) loud = true;
            }
          if (!loud) kept.push_back(std::move(s));
        }
        surv = std::move(kept);
      }
    }
    std::size_t silent = 0;
    {
      std::unordered_set<std::uint64_t> sc;
      for (auto& s : surv) sc.insert(s.cell);
      silent = sc.size();
    }
    const auto t_centre = now();
    // 4a. Exact curves at the cell centre. A combination of flights usually survives in
    //     a corridor of thousands of contiguous cells whose scores vary smoothly: every
    //     third cell each way is fitted first, then the neighbourhoods of its best four.
    auto quick = [&](Survivor& s) {
      const i64 r = CellGrid::krow(s.cell), c = CellGrid::kcol(s.cell);
      const double lat = grid.lat_c(r), lon = grid.lon_c(r, c);
      const Vec3 op = ecef(lat, lon, terrain.ground(lat, lon) + o.mic_height);
      std::vector<std::pair<int, const Path*>> cb;
      for (auto& [ei, sg] : s.combo) cb.emplace_back(ei, &segs[std::size_t(sg)].path);
      s.score = joint_fit(spec, ev, cb, op, s.a, s.b, o.c, QUICK).score;
    };
    {
      std::map<std::vector<std::pair<int, int>>, std::vector<std::size_t>> groups;
      for (std::size_t i = 0; i < surv.size(); ++i) groups[surv[i].combo].push_back(i);
      std::vector<bool> keep(surv.size(), false);
      for (auto& [combo, idx] : groups) {
        if (idx.size() <= 48) {
          for (auto i : idx) quick(surv[i]), keep[i] = true;
          continue;
        }
        std::unordered_map<std::uint64_t, std::size_t> at;
        for (auto i : idx) at[surv[i].cell] = i;
        std::vector<std::size_t> lattice;
        for (auto i : idx) {
          const i64 r = CellGrid::krow(surv[i].cell), c = CellGrid::kcol(surv[i].cell);
          if (((r % 3) + 3) % 3 == 0 && ((c % 3) + 3) % 3 == 0) lattice.push_back(i);
        }
        if (lattice.empty()) lattice = idx;
        for (auto i : lattice) quick(surv[i]), keep[i] = true;
        std::sort(lattice.begin(), lattice.end(), [&](auto a, auto b) { return surv[a].score > surv[b].score; });
        for (std::size_t k = 0; k < std::min<std::size_t>(4, lattice.size()); ++k) {
          const i64 r0 = CellGrid::krow(surv[lattice[k]].cell), c0 = CellGrid::kcol(surv[lattice[k]].cell);
          for (i64 dr = -2; dr <= 2; ++dr)
            for (i64 dc = -2; dc <= 2; ++dc) {
              auto it = at.find(CellGrid::key(r0 + dr, c0 + dc));
              if (it == at.end() || keep[it->second]) continue;
              quick(surv[it->second]), keep[it->second] = true;
            }
        }
      }
      std::vector<Survivor> scored;
      for (std::size_t i = 0; i < surv.size(); ++i)
        if (keep[i]) scored.push_back(std::move(surv[i]));
      surv = std::move(scored);
    }
    std::lock_guard lk(mu);
    cpu_corr += secs(t_tile, t_centre), cpu_centre += secs(t_centre, now()), cpu_pairs += secs(t_tile, t_pairs);
    cpu_silence += secs(t_silence, t_centre);
    n_corr += corr, n_cons += cons, n_silent += silent, n_pp += ps.pieces, n_pk += ps.kept;
    survivors.insert(survivors.end(), std::make_move_iterator(surv.begin()), std::make_move_iterator(surv.end()));
  });
  res.corridor_cells = n_corr, res.consistent_cells = n_cons, res.silent_cells = n_silent;
  res.cpu_corridors = cpu_corr, res.cpu_centre = cpu_centre, res.cpu_pairs = cpu_pairs, res.wall_tiles = secs(t_tiles, now());
  res.pair_pieces = n_pp, res.pair_kept = n_pk, res.cpu_silence = cpu_silence;
  if (survivors.empty()) {
    res.reasons.push_back("no location heard matching aircraft at these times");
    res.terrain_complete = terrain.complete();
    res.seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - clock0).count();
    return res;
  }

  // 4b. The best survivors on the finer sub-grid.
  // Quick scores rank the survivors; the best get a full fit at the cell centre, and the
  // best of those the sub-grid.
  std::sort(survivors.begin(), survivors.end(), [](const Survivor& a, const Survivor& b) { return a.score > b.score; });
  if (int(survivors.size()) > o.max_scored) survivors.resize(std::size_t(o.max_scored));
  parallel_for(survivors.size(), [&](std::size_t i) {
    Survivor& s = survivors[i];
    const i64 r = CellGrid::krow(s.cell), c = CellGrid::kcol(s.cell);
    const double lat = grid.lat_c(r), lon = grid.lon_c(r, c);
    const Vec3 op = ecef(lat, lon, terrain.ground(lat, lon) + o.mic_height);
    std::vector<std::pair<int, const Path*>> cb;
    for (auto& [ei, sg] : s.combo) cb.emplace_back(ei, &segs[std::size_t(sg)].path);
    s.score = joint_fit(spec, ev, cb, op, s.a, s.b, o.c, FULL).score;
  });
  std::sort(survivors.begin(), survivors.end(), [](const Survivor& a, const Survivor& b) { return a.score > b.score; });
  if (int(survivors.size()) > o.max_subgrid) survivors.resize(std::size_t(o.max_subgrid));
  while (!survivors.empty() && survivors.back().score < o.score_floor) survivors.pop_back();
  if (survivors.empty()) {
    res.reasons.push_back("no location heard matching aircraft at these times");
    res.terrain_complete = terrain.complete();
    res.seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - clock0).count();
    return res;
  }
  auto make_candidate = [&](const Survivor& s, double lat, double lon, double ground) {
    const Vec3 op = ecef(lat, lon, ground + o.mic_height);
    std::vector<std::pair<int, const Path*>> cb;
    for (auto& [ei, sg] : s.combo) cb.emplace_back(ei, &segs[std::size_t(sg)].path);
    auto f = joint_fit(spec, ev, cb, op, s.a, s.b, o.c);
    Candidate cd;
    cd.lat = lat, cd.lon = lon, cd.ground_m = ground, cd.start_utc = f.start, cd.score = f.score;
    for (std::size_t k = 0; k < s.combo.size(); ++k) {
      const auto [ei, sg] = s.combo[k];
      const Event& e = ev[std::size_t(ei)];
      const Seg& seg = segs[std::size_t(sg)];
      Match m;
      m.event = res.events_used[std::size_t(ei)];
      m.flight = seg.tr->id(), m.type = seg.tr->type, m.reg = seg.tr->reg;
      const Approach ap = closest_approach(seg.path, op, o.c);
      m.cpa_m = ap.r, m.speed = ap.speed, m.cpa_utc = ap.t_rx;
      m.score = f.scores[k], m.fit = e.score > 0 ? f.scores[k] / e.score : 0;
      m.f0 = e.lines.empty() ? e.f0 : e.lines[0].f0 * (1 + f.deltas[k]);
      cd.matches.push_back(m);
      cd.combo += (cd.combo.empty() ? "" : "+") + std::to_string(m.event) + ":" + m.flight;
    }
    return cd;
  };
  const auto t_sub = now();
  std::vector<Candidate> cands(survivors.size());
  parallel_for(survivors.size(), [&](std::size_t i) {
    const Survivor& s = survivors[i];
    const i64 r = CellGrid::krow(s.cell), c = CellGrid::kcol(s.cell);
    const double lat_c = grid.lat_c(r), lon_c = grid.lon_c(r, c);
    const int n = std::max(1, int(std::lround(o.cell_m / o.fine_m)));
    Candidate best;
    best.score = -INFINITY;
    for (int a = 0; a < n; ++a)
      for (int b = 0; b < n; ++b) {
        const double dy = (a + 0.5) / n - 0.5, dx = (b + 0.5) / n - 0.5;
        const double lat = lat_c + dy * grid.dlat, lon = lon_c + dx * grid.dlon(r);
        if (o.inside && !o.inside(lat, lon)) continue;
        auto cd = make_candidate(s, lat, lon, terrain.ground(lat, lon));
        if (cd.score > best.score) best = std::move(cd);
      }
    cands[i] = std::move(best);
  });
  res.scored = cands.size();
  res.wall_subgrid = secs(t_sub, now());
  const auto t_ref = now();

  // 5. Best per combination, the leaders refined locally.
  std::map<std::string, std::size_t> by_combo;
  std::vector<std::pair<Candidate, std::size_t>> best;  // candidate, survivor index
  for (std::size_t i = 0; i < cands.size(); ++i) {
    if (!std::isfinite(cands[i].score)) continue;
    auto it = by_combo.find(cands[i].combo);
    if (it == by_combo.end()) by_combo[cands[i].combo] = best.size(), best.emplace_back(cands[i], i);
    else if (cands[i].score > best[it->second].first.score) best[it->second] = {cands[i], i};
  }
  std::sort(best.begin(), best.end(), [](auto& a, auto& b) { return a.first.score > b.first.score; });
  const std::size_t n_refine = std::min<std::size_t>(std::size_t(std::max(1, o.max_refine)), best.size());
  for (std::size_t k = 0; k < n_refine; ++k) {
    const Survivor& s = survivors[best[k].second];
    const Candidate base = best[k].first;
    const double half = 0.5 * o.cell_m + 2 * o.refine_m;
    const int n = int(std::ceil(half / o.refine_m));
    std::vector<Candidate> local(std::size_t((2 * n + 1) * (2 * n + 1)));
    parallel_for(local.size(), [&](std::size_t i) {
      const int a = int(i) / (2 * n + 1) - n, b = int(i) % (2 * n + 1) - n;
      const double lat = base.lat + a * o.refine_m / M_PER_DEG, lon = base.lon + b * o.refine_m / M_PER_DEG / std::cos(base.lat * DEG);
      if (o.inside && !o.inside(lat, lon)) {
        local[i].score = -INFINITY;
        return;
      }
      local[i] = make_candidate(s, lat, lon, terrain.ground(lat, lon));
    });
    for (auto& cd : local)
      if (cd.score > best[k].first.score) best[k].first = cd;
  }
  res.wall_refine = secs(t_ref, now());
  std::sort(best.begin(), best.end(), [](auto& a, auto& b) { return a.first.score > b.first.score; });
  for (std::size_t k = 0; k < best.size() && int(k) < o.top_k; ++k) res.candidates.push_back(std::move(best[k].first));

  judge(res);
  res.terrain_complete = terrain.complete();
  if (!res.terrain_complete) res.reasons.push_back("some ground heights are the geoid alone (DEM tiles unavailable)");
  res.seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - clock0).count();
  return res;
}

json::object result_json(const SearchResult& r, const std::vector<Event>& events) {
  json::array cands;
  for (auto& c : r.candidates) {
    json::array ms;
    for (auto& m : c.matches)
      ms.push_back(json::object{{"event", m.event},
                                {"flight", m.flight},
                                {"type", m.type},
                                {"registration", m.reg},
                                {"cpa_m", m.cpa_m},
                                {"speed_ms", m.speed},
                                {"cpa_utc", iso(m.cpa_utc)},
                                {"score", m.score},
                                {"fit", m.fit},
                                {"f0_hz", m.f0}});
    cands.push_back(json::object{{"lat", c.lat},
                                 {"lon", c.lon},
                                 {"ground_m", c.ground_m},
                                 {"start_utc", fmt_utc(c.start_utc)},
                                 {"start_epoch", c.start_utc},
                                 {"score", c.score},
                                 {"matches", ms}});
  }
  json::array used;
  for (int i : r.events_used) used.push_back(i);
  json::array reasons;
  for (auto& s : r.reasons) reasons.push_back(json::value(s));
  (void)events;
  return {{"level", air_level_name(r.level)},
          {"reasons", reasons},
          {"events_used", used},
          {"candidates", cands},
          {"funnel",
           json::object{{"segments", r.segments}, {"corridor_cells", r.corridor_cells}, {"consistent_cells", r.consistent_cells}, {"silent_cells", r.silent_cells}, {"scored", r.scored},
                        {"type_pairs", r.type_pairs}, {"type_pruned_pairs", r.type_pruned_pairs}, {"type_pruned_segments", r.type_pruned_segments}}},
          {"terrain_complete", r.terrain_complete},
          {"seconds", r.seconds}};
}

std::string render_result(const SearchResult& r, const std::vector<Event>& events) {
  std::string out;
  char b[512];
  std::snprintf(b, sizeof b, "Aircraft search: %s (%.1f s)\n", air_level_name(r.level), r.seconds);
  out += b;
  for (auto& s : r.reasons) out += "  " + s + "\n";
  std::snprintf(b, sizeof b, "  funnel: %zu flight segments -> %zu corridor cells -> %zu with one clock -> %zu silent -> %zu scored\n", r.segments,
                r.corridor_cells, r.consistent_cells, r.silent_cells, r.scored);
  out += b;
  if (r.type_pairs) {
    std::snprintf(b, sizeof b, "  rest-frequency prior: %zu of %zu (segment, event) pairs dropped (%.0f%%), %zu segments for every event\n",
                  r.type_pruned_pairs, r.type_pairs, 100.0 * double(r.type_pruned_pairs) / double(r.type_pairs), r.type_pruned_segments);
    out += b;
  }
  std::snprintf(b, sizeof b, "  time: tiles %.1f s (corridors %.0f cpu-s, centre scoring %.0f cpu-s), sub-grid %.1f s, refinement %.1f s\n", r.wall_tiles,
                r.cpu_corridors, r.cpu_centre, r.wall_subgrid, r.wall_refine);
  out += b;
  std::snprintf(b, sizeof b, "  of which: silence %.0f cpu-s; loading tracks %.1f s, cutting pieces %.1f s\n", r.cpu_silence, r.wall_load, r.wall_pieces);
  out += b;
  if (r.pair_pieces) {
    std::snprintf(b, sizeof b, "  pair-first: %zu of %zu pieces rasterized (pairing %.0f cpu-s)\n", r.pair_kept, r.pair_pieces, r.cpu_pairs);
    out += b;
  }
  for (std::size_t i = 0; i < r.candidates.size() && i < 10; ++i) {
    auto& c = r.candidates[i];
    std::snprintf(b, sizeof b, "  #%zu  %.5f, %.5f  ground %.0f m  start %s  score %.1f\n", i + 1, c.lat, c.lon, c.ground_m, fmt_utc(c.start_utc).c_str(),
                  c.score);
    out += b;
    for (auto& m : c.matches) {
      std::snprintf(b, sizeof b, "        event %d (CPA %.1f s): %s %s %s, CPA %.0f m at %.0f m/s, %s, score %.1f, fit %.2f, f0 %.1f Hz\n", m.event + 1,
                    m.event < int(events.size()) ? events[std::size_t(m.event)].tc : NAN, m.flight.c_str(), m.type.c_str(), m.reg.c_str(), m.cpa_m,
                    m.speed, fmt_utc(m.cpa_utc).c_str(), m.score, m.fit, m.f0);
      out += b;
    }
  }
  return out;
}

}  // namespace geo::air
