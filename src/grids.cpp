#include "grids.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>

#include "data.hpp"
#include "util/json.hpp"
#include "util/time.hpp"

namespace geo::grids {

std::string day_of(double t) { return iso_date(static_cast<i64>(std::floor(t))); }
std::string today() { return day_of(now_seconds()); }
std::string normalize_day(const std::string& s) {
  if (s.empty()) return today();
  if (s.size() == 7) return s + "-01";
  return s.substr(0, 10);
}

static void add_unique(std::vector<std::string>& out, const std::vector<std::string>& v) {
  for (auto& c : v)
    if (std::find(out.begin(), out.end(), c) == out.end()) out.push_back(c);
}

std::vector<std::string> Grid::countries_at(const std::string& day) const {
  std::vector<std::string> out;
  for (auto& a : areas)
    if (a.active(day)) add_unique(out, a.include);
  return out;
}

std::vector<std::string> Grid::countries() const {
  auto now = countries_at(today());
  if (!now.empty() || areas.empty()) return now;
  const Area* last = &areas[0];
  for (auto& a : areas)
    if (a.end.value_or("9999") > last->end.value_or("9999")) last = &a;
  std::vector<std::string> out;
  add_unique(out, last->include);
  return out;
}

bool Grid::exists_at(const std::string& day) const {
  return std::any_of(areas.begin(), areas.end(), [&](auto& a) { return a.active(day); });
}

std::optional<std::string> Grid::status_at(const std::string& day) const {
  std::vector<std::string> st;
  for (auto& a : areas)
    if (a.active(day)) st.push_back(a.status);
  if (st.empty()) return std::nullopt;
  bool same = std::all_of(st.begin(), st.end(), [&](auto& s) { return s == st[0]; });
  return same ? st[0] : std::string("mixed");
}

const Grid* GridTable::find(const std::string& id) const {
  for (auto& g : grids)
    if (g.id == id) return &g;
  return nullptr;
}

const Grid& GridTable::get(const std::string& id) const {
  if (auto* g = find(id)) return *g;
  std::vector<std::string> ids;
  for (auto& g : grids) ids.push_back(g.id);
  std::sort(ids.begin(), ids.end());
  std::string known;
  for (auto& i : ids) known += (known.empty() ? "" : ", ") + i;
  throw std::out_of_range("unknown grid '" + id + "'; known grids: " + known);
}

std::vector<const Grid*> GridTable::tracked() const {
  std::vector<const Grid*> o;
  for (auto& g : grids)
    if (g.tracked()) o.push_back(&g);
  return o;
}

static std::vector<std::string> codes(const json::value* refs) {
  std::vector<std::string> out;
  if (!refs || !refs->is_array()) return out;
  for (auto& r : refs->get_array()) {
    std::string c = r.is_string() ? std::string(r.get_string()) : jstr(r, "iso");
    if (!c.empty() && std::find(out.begin(), out.end(), c) == out.end()) out.push_back(c);
  }
  return out;
}

static std::optional<std::string> opt_str(const json::value& v, std::string_view key) {
  auto* p = jget(v, key);
  if (p && p->is_string()) return std::string(p->get_string());
  return std::nullopt;
}

const GridTable& load_grids() {
  static const GridTable table = [] {
    GridTable t;
    auto raw = parse_json(data::grids_json());
    for (auto& g : raw.at("grids").get_array()) {
      Grid grid;
      grid.id = jstr(g, "id");
      grid.name = jstr(g, "name");
      grid.nominal_hz = static_cast<int>(jint(g, "nominal_hz", 50));
      grid.reference = jstr(g, "reference", "tracked");
      grid.notes = jstr(g, "notes");
      if (auto* areas = jget(g, "areas")) {
        for (auto& a : areas->get_array()) {
          Area ar;
          ar.include = codes(jget(a, "include"));
          ar.exclude = codes(jget(a, "exclude"));
          ar.start = opt_str(a, "from");
          ar.end = opt_str(a, "to");
          ar.status = jstr(a, "status", "synchronous");
          if (ar.status != "synchronous" && ar.status != "dc_linked" && ar.status != "isolated")
            throw std::runtime_error("unknown area status '" + ar.status + "'");
          ar.source = jstr(a, "source");
          grid.areas.push_back(std::move(ar));
        }
      } else {  // v1: whole-period countries minus "except"
        Area ar;
        ar.include = codes(jget(g, "countries"));
        ar.exclude = codes(jget(g, "except"));
        grid.areas.push_back(std::move(ar));
      }
      t.grids.push_back(std::move(grid));
    }
    for (auto& c : raw.at("sixty_hz_countries").get_array()) t.sixty_hz_countries.emplace_back(c.get_string());
    if (auto* mc = jget(raw, "mixed_countries"))
      for (auto& kv : mc->get_object()) t.mixed_countries[std::string(kv.key())] = std::string(kv.value().get_string());
    return t;
  }();
  return table;
}

// ---------------------------------------------------------------- point in grid

namespace {

struct Shape {
  std::string grid, status;
  std::optional<std::string> start, end;
  double bbox[4];
  std::vector<std::vector<std::pair<double, double>>> rings;  // closed lon/lat rings
  std::vector<std::pair<std::size_t, std::size_t>> polys;     // (first ring, ring count)
  bool active(const std::string& d) const { return (!start || *start <= d) && (!end || d < *end); }
};

const std::vector<Shape>& load_shapes() {
  static const std::vector<Shape> shapes = [] {
    std::vector<Shape> out;
    auto raw = parse_json(data::grid_areas_json());
    for (auto& f : raw.at("features").get_array()) {
      const auto& p = f.at("properties");
      const auto& geom = f.at("geometry");
      Shape s;
      s.grid = jstr(p, "grid", jstr(p, "id"));
      s.start = opt_str(p, "from");
      s.end = opt_str(p, "to");
      s.status = jstr(p, "status");
      std::string type = jstr(geom, "type");
      std::vector<const json::array*> polys;
      if (type == "Polygon")
        polys.push_back(&geom.at("coordinates").get_array());
      else if (type == "MultiPolygon")
        for (auto& pl : geom.at("coordinates").get_array()) polys.push_back(&pl.get_array());
      double mnx = INFINITY, mny = INFINITY, mxx = -INFINITY, mxy = -INFINITY;
      for (auto* poly : polys) {
        s.polys.emplace_back(s.rings.size(), poly->size());
        for (auto& ring : *poly) {
          std::vector<std::pair<double, double>> r;
          r.reserve(ring.get_array().size());
          for (auto& pt : ring.get_array()) {
            double x = as_number(pt.get_array()[0]), y = as_number(pt.get_array()[1]);
            r.emplace_back(x, y);
            mnx = std::min(mnx, x);
            mny = std::min(mny, y);
            mxx = std::max(mxx, x);
            mxy = std::max(mxy, y);
          }
          s.rings.push_back(std::move(r));
        }
      }
      if (s.rings.empty()) continue;
      s.bbox[0] = mnx;
      s.bbox[1] = mny;
      s.bbox[2] = mxx;
      s.bbox[3] = mxy;
      out.push_back(std::move(s));
    }
    return out;
  }();
  return shapes;
}

bool inside_ring(const std::vector<std::pair<double, double>>& r, double x, double y) {
  std::size_t cnt = 0;
  for (std::size_t i = 0; i + 1 < r.size(); ++i) {
    auto [x0, y0] = r[i];
    auto [x1, y1] = r[i + 1];
    if ((y0 > y) != (y1 > y)) {
      double xi = x0 + (y - y0) * (x1 - x0) / (y1 - y0);
      cnt += x < xi;
    }
  }
  return cnt % 2;
}

bool contains(const Shape& s, double lon, double lat) {
  if (!(s.bbox[0] <= lon && lon <= s.bbox[2] && s.bbox[1] <= lat && lat <= s.bbox[3])) return false;
  for (auto [first, n] : s.polys) {
    if (!inside_ring(s.rings[first], lon, lat)) continue;
    bool hole = false;
    for (std::size_t k = 1; k < n && !hole; ++k) hole = inside_ring(s.rings[first + k], lon, lat);
    if (!hole) return true;
  }
  return false;
}

double distance_km(const Shape& s, double lon, double lat) {
  const double k = std::cos(lat * M_PI / 180.0);
  double best = INFINITY;
  for (auto& r : s.rings)
    for (std::size_t i = 0; i + 1 < r.size(); ++i) {
      double ax = (r[i].first - lon) * k, ay = r[i].second - lat;
      double bx = (r[i + 1].first - lon) * k, by = r[i + 1].second - lat;
      double dx = bx - ax, dy = by - ay, L = dx * dx + dy * dy;
      double t = L > 0 ? std::clamp(-(ax * dx + ay * dy) / L, 0.0, 1.0) : 0.0;
      best = std::min(best, std::hypot(ax + t * dx, ay + t * dy));
    }
  return best * 111.32;
}

// list({p.grid: p for p in items}.values()): first position, last value.
std::vector<Placement> dedupe(const std::vector<Placement>& v) {
  std::vector<Placement> out;
  for (auto& p : v) {
    auto it = std::find_if(out.begin(), out.end(), [&](auto& o) { return o.grid == p.grid; });
    if (it == out.end())
      out.push_back(p);
    else
      *it = p;
  }
  return out;
}

}  // namespace

std::vector<Placement> grids_at(double lat, double lon, const std::string& day, double snap_km) {
  const std::string d = normalize_day(day);
  std::vector<const Shape*> act;
  for (auto& s : load_shapes())
    if (s.active(d)) act.push_back(&s);
  std::vector<Placement> hits;
  for (auto* s : act)
    if (contains(*s, lon, lat)) hits.push_back({s->grid, s->status, 0.0});
  if (!hits.empty()) return dedupe(hits);
  const double pad = snap_km / 111.32 / std::max(std::cos(lat * M_PI / 180.0), 0.05);
  std::vector<Placement> near;
  for (auto* s : act)
    if (s->bbox[0] - pad <= lon && lon <= s->bbox[2] + pad && s->bbox[1] - pad <= lat && lat <= s->bbox[3] + pad) {
      double km = distance_km(*s, lon, lat);
      if (km <= snap_km) near.push_back({s->grid, s->status, km});
    }
  std::stable_sort(near.begin(), near.end(), [](auto& a, auto& b) { return a.distance_km < b.distance_km; });
  return dedupe(near);
}

std::optional<std::array<double, 4>> grid_extent(const std::string& grid, const std::string& day) {
  const std::string d = normalize_day(day);
  std::optional<std::array<double, 4>> box;
  for (auto& s : load_shapes()) {
    if (s.grid != grid || !s.active(d)) continue;
    if (!box) box = std::array<double, 4>{s.bbox[0], s.bbox[1], s.bbox[2], s.bbox[3]};
    else (*box)[0] = std::min((*box)[0], s.bbox[0]), (*box)[1] = std::min((*box)[1], s.bbox[1]), (*box)[2] = std::max((*box)[2], s.bbox[2]),
         (*box)[3] = std::max((*box)[3], s.bbox[3]);
  }
  return box;
}

std::vector<std::vector<Placement>> grids_at_days(double lat, double lon, const std::vector<std::string>& days, double snap_km) {
  std::map<std::string, std::vector<Placement>> memo;
  std::vector<std::vector<Placement>> out;
  const auto& shapes = load_shapes();
  for (auto& day : days) {
    const std::string d = normalize_day(day);
    std::string sig(shapes.size(), '0');
    for (std::size_t i = 0; i < shapes.size(); ++i) sig[i] = shapes[i].active(d) ? '1' : '0';
    auto it = memo.find(sig);
    if (it == memo.end()) it = memo.emplace(sig, grids_at(lat, lon, d, snap_km)).first;
    out.push_back(it->second);
  }
  return out;
}

}  // namespace geo::grids
