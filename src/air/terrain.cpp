#include "air/terrain.hpp"

#include <zlib.h>

#include <cmath>
#include <cstring>
#include <iostream>

#include "net/http.hpp"
#include "store/store.hpp"

namespace geo::air {

namespace {

constexpr const char* DEM_URL = "https://copernicus-dem-90m.s3.amazonaws.com/";
constexpr const char* GEOID_URL = "https://cdn.proj.org/us_nga_egm96_15.tif";

struct Tiff {
  std::string_view b;
  bool le = true;
  std::uint16_t u16(std::size_t o) const {
    if (o + 2 > b.size()) throw std::runtime_error("truncated TIFF");
    const auto* p = reinterpret_cast<const unsigned char*>(b.data() + o);
    return le ? std::uint16_t(p[0] | p[1] << 8) : std::uint16_t(p[1] | p[0] << 8);
  }
  std::uint32_t u32(std::size_t o) const {
    if (o + 4 > b.size()) throw std::runtime_error("truncated TIFF");
    const auto* p = reinterpret_cast<const unsigned char*>(b.data() + o);
    return le ? std::uint32_t(p[0] | p[1] << 8 | p[2] << 16 | std::uint32_t(p[3]) << 24)
              : std::uint32_t(p[3] | p[2] << 8 | p[1] << 16 | std::uint32_t(p[0]) << 24);
  }
  double f64(std::size_t o) const {
    std::uint64_t lo = u32(o), hi = u32(o + 4);
    std::uint64_t bits = le ? (hi << 32 | lo) : (lo << 32 | hi);
    double d;
    std::memcpy(&d, &bits, 8);
    return d;
  }
};

struct Tag {
  std::uint16_t type = 0;
  std::uint32_t count = 0;
  std::size_t at = 0;  // offset of the values
};

std::string inflate_all(std::string_view in, std::size_t expect) {
  std::string out(expect, '\0');
  uLongf n = uLongf(expect);
  const int r = uncompress(reinterpret_cast<Bytef*>(out.data()), &n, reinterpret_cast<const Bytef*>(in.data()), uLong(in.size()));
  if (r != Z_OK && r != Z_BUF_ERROR) throw std::runtime_error("corrupt TIFF tile");
  out.resize(n);
  return out;
}

std::string fetch(const std::string& url, long* status) {
  return net::run_sync<std::string>([&](net::asio::io_context& io) -> net::asio::awaitable<std::string> {
    net::Http http(io);
    for (int attempt = 0;; ++attempt) {
      auto r = co_await http.fetch({.url = url, .total_timeout_s = 300});
      *status = r.status;
      if (r.ok() || r.status == 404 || r.status == 403 || attempt >= 2) co_return r.ok() ? r.body : std::string();
      co_await net::sleep_for(2.0 * (attempt + 1));
    }
  });
}

}  // namespace

double Raster::sample(double x, double y) const {
  double fx = (x - x0) / dx, fy = (y0 - y) / dy;
  if (area) fx -= 0.5, fy -= 0.5;
  if (fx < -1 || fy < -1 || fx > w || fy > h) return NAN;  // one pixel of slack: point grids stop a step short of the tile edge
  fx = std::clamp(fx, 0.0, double(w - 1)), fy = std::clamp(fy, 0.0, double(h - 1));
  const int i = std::min(w - 2, int(fx)), j = std::min(h - 2, int(fy));
  const double a = fx - i, c = fy - j;
  auto at = [&](int ii, int jj) {
    const float z = v[std::size_t(jj) * std::size_t(w) + std::size_t(ii)];
    return (std::isfinite(nodata) && z == nodata) ? NAN : double(z);
  };
  return (1 - a) * (1 - c) * at(i, j) + a * (1 - c) * at(i + 1, j) + (1 - a) * c * at(i, j + 1) + a * c * at(i + 1, j + 1);
}

Raster read_geotiff(std::string_view bytes) {
  Tiff t{bytes, true};
  if (bytes.size() < 8) throw std::runtime_error("not a TIFF");
  if (bytes.substr(0, 2) == "MM") t.le = false;
  else if (bytes.substr(0, 2) != "II") throw std::runtime_error("not a TIFF");
  if (t.u16(2) != 42) throw std::runtime_error("BigTIFF or not a TIFF");
  const std::size_t ifd = t.u32(4);
  std::map<int, Tag> tags;
  const int n = t.u16(ifd);
  for (int i = 0; i < n; ++i) {
    const std::size_t e = ifd + 2 + 12 * std::size_t(i);
    Tag tg{t.u16(e + 2), t.u32(e + 4), e + 8};
    const int sz = tg.type == 3 ? 2 : tg.type == 4 ? 4 : tg.type == 12 ? 8 : tg.type == 16 ? 8 : 1;
    if (std::size_t(sz) * tg.count > 4) tg.at = t.u32(e + 8);
    tags[t.u16(e)] = tg;
  }
  auto val = [&](int tag, std::size_t k = 0, double fallback = NAN) -> double {
    auto it = tags.find(tag);
    if (it == tags.end() || k >= it->second.count) return fallback;
    const Tag& tg = it->second;
    switch (tg.type) {
      case 3: return t.u16(tg.at + 2 * k);
      case 4: return t.u32(tg.at + 4 * k);
      case 12: return t.f64(tg.at + 8 * k);
      default: return fallback;
    }
  };
  Raster r;
  r.w = int(val(256)), r.h = int(val(257));
  const int bits = int(val(258, 0, 32)), comp = int(val(259, 0, 1)), pred = int(val(317, 0, 1)), fmt = int(val(339, 0, 1));
  if (bits != 32 || fmt != 3 || int(val(277, 0, 1)) != 1) throw std::runtime_error("only single-band float32 GeoTIFFs are read");
  if (comp != 1 && comp != 8 && comp != 32946) throw std::runtime_error("unsupported TIFF compression " + std::to_string(comp));
  r.dx = val(33550, 0), r.dy = val(33550, 1);
  const double ti = val(33922, 0), tj = val(33922, 1);
  r.x0 = val(33922, 3) - ti * r.dx, r.y0 = val(33922, 4) + tj * r.dy;
  // GTRasterTypeGeoKey (1025): 1 PixelIsArea, 2 PixelIsPoint.
  if (auto it = tags.find(34735); it != tags.end())
    for (std::uint32_t k = 4; k + 3 < it->second.count; k += 4)
      if (val(34735, k) == 1025) r.area = val(34735, k + 3) == 1;
  if (auto it = tags.find(42113); it != tags.end()) r.nodata = float(std::atof(std::string(bytes.substr(it->second.at, it->second.count)).c_str()));
  if (!(r.w > 0 && r.h > 0 && r.dx > 0 && r.dy > 0)) throw std::runtime_error("GeoTIFF without georeferencing");
  r.v.assign(std::size_t(r.w) * std::size_t(r.h), NAN);

  const bool tiled = tags.count(322);
  const int bw = tiled ? int(val(322)) : r.w, bh = tiled ? int(val(323)) : int(val(278, 0, r.h));
  const int offs_tag = tiled ? 324 : 273, cnt_tag = tiled ? 325 : 279;
  const int across = (r.w + bw - 1) / bw, down = (r.h + bh - 1) / bh;
  for (int by = 0; by < down; ++by)
    for (int bx = 0; bx < across; ++bx) {
      const std::size_t k = std::size_t(by) * std::size_t(across) + std::size_t(bx);
      const std::size_t off = std::size_t(val(offs_tag, k)), len = std::size_t(val(cnt_tag, k));
      if (!off || off + len > bytes.size()) continue;
      const std::size_t raw_len = std::size_t(bw) * std::size_t(bh) * 4;
      std::string raw = comp == 1 ? std::string(bytes.substr(off, len)) : inflate_all(bytes.substr(off, len), raw_len);
      if (raw.size() < raw_len) raw.resize(raw_len, '\0');
      auto* p = reinterpret_cast<unsigned char*>(raw.data());
      std::vector<float> blk(std::size_t(bw) * std::size_t(bh));
      if (pred == 3) {  // floating-point predictor: byte-wise differences, then byte planes MSB first
        std::vector<unsigned char> row(std::size_t(bw) * 4);
        for (int y = 0; y < bh; ++y) {
          unsigned char* q = p + std::size_t(y) * std::size_t(bw) * 4;
          for (std::size_t i = 1; i < std::size_t(bw) * 4; ++i) q[i] = static_cast<unsigned char>(q[i] + q[i - 1]);
          for (int x = 0; x < bw; ++x) {
            unsigned char be[4] = {q[x], q[bw + x], q[2 * bw + x], q[3 * bw + x]};
            const std::uint32_t u = std::uint32_t(be[0]) << 24 | std::uint32_t(be[1]) << 16 | std::uint32_t(be[2]) << 8 | be[3];
            std::memcpy(&blk[std::size_t(y) * std::size_t(bw) + std::size_t(x)], &u, 4);
          }
        }
      } else {
        for (std::size_t i = 0; i < blk.size(); ++i) {
          std::uint32_t u = t.le ? std::uint32_t(p[4 * i] | p[4 * i + 1] << 8 | p[4 * i + 2] << 16 | std::uint32_t(p[4 * i + 3]) << 24)
                                 : std::uint32_t(p[4 * i + 3] | p[4 * i + 2] << 8 | p[4 * i + 1] << 16 | std::uint32_t(p[4 * i]) << 24);
          std::memcpy(&blk[i], &u, 4);
        }
        if (pred == 2) throw std::runtime_error("integer predictor on float data");
      }
      for (int y = 0; y < bh && by * bh + y < r.h; ++y)
        for (int x = 0; x < bw && bx * bw + x < r.w; ++x)
          r.v[std::size_t(by * bh + y) * std::size_t(r.w) + std::size_t(bx * bw + x)] = blk[std::size_t(y) * std::size_t(bw) + std::size_t(x)];
    }
  return r;
}

Terrain::Terrain(bool online) : online_(online), coarse_(new std::atomic<const Coarse*>[180 * 360]) {
  for (int i = 0; i < 180 * 360; ++i) coarse_[std::size_t(i)] = nullptr;
}
Terrain::~Terrain() = default;

std::shared_ptr<const Raster> Terrain::load_tile(int lat0, int lon0, bool* sea) {
  char name[96];
  std::snprintf(name, sizeof name, "Copernicus_DSM_COG_30_%c%02d_00_%c%03d_00_DEM", lat0 >= 0 ? 'N' : 'S', std::abs(lat0), lon0 >= 0 ? 'E' : 'W',
                std::abs(lon0));
  const fs::path dir = store::cache_dir() / "dem", path = dir / (std::string(name) + ".tif"), sea_mark = dir / (std::string(name) + ".sea");
  *sea = false;
  if (auto b = read_file(path)) return std::make_shared<const Raster>(read_geotiff(*b));
  if (fs::exists(sea_mark)) return *sea = true, nullptr;
  if (!online_) {
    missing_ = true;
    return nullptr;
  }
  std::lock_guard dl(dl_mu_);  // one download at a time; another thread may have fetched it
  if (auto b = read_file(path)) return std::make_shared<const Raster>(read_geotiff(*b));
  long status = 0;
  const std::string body = fetch(std::string(DEM_URL) + name + "/" + name + ".tif", &status);
  fs::create_directories(dir);
  if (!body.empty()) {
    auto r = std::make_shared<const Raster>(read_geotiff(body));
    atomic_write(path, body);
    return r;
  }
  if (status == 404 || status == 403) {
    atomic_write(sea_mark, "");  // not in the catalogue: open water
    return *sea = true, nullptr;
  }
  missing_ = true;
  std::cerr << "warning: DEM tile " << name << " unavailable (HTTP " << status << "); using the geoid" << std::endl;
  return nullptr;
}

std::shared_ptr<const Raster> Terrain::full(int lat0, int lon0) {
  {
    std::lock_guard lk(mu_);
    for (std::size_t i = lru_.size(); i-- > 0;)
      if (lru_[i].first == std::pair{lat0, lon0}) {
        auto r = lru_[i].second;
        if (i + 1 != lru_.size()) std::rotate(lru_.begin() + std::ptrdiff_t(i), lru_.begin() + std::ptrdiff_t(i) + 1, lru_.end());
        return r;
      }
  }
  bool sea = false;
  auto r = load_tile(lat0, lon0, &sea);
  std::lock_guard lk(mu_);
  lru_.emplace_back(std::pair{lat0, lon0}, r);
  if (lru_.size() > 32) lru_.erase(lru_.begin());
  return r;
}

const Terrain::Coarse* Terrain::coarse(int lat0, int lon0) {
  const std::size_t k = std::size_t(std::clamp(lat0 + 90, 0, 179)) * 360 + std::size_t(((lon0 + 180) % 360 + 360) % 360);
  if (const Coarse* c = coarse_[k].load(std::memory_order_acquire)) return c;
  auto r = full(lat0, lon0);
  auto c = std::make_unique<Coarse>();
  for (int i = 0; i <= COARSE; ++i)
    for (int j = 0; j <= COARSE; ++j) {
      // Mean over the block centred at (lat0 + i/COARSE, lon0 + j/COARSE).
      double sum = 0;
      int n = 0;
      if (r)
        for (int a = -2; a <= 2; ++a)
          for (int b = -2; b <= 2; ++b) {
            const double h = r->sample(lon0 + (j + b * 0.2) / COARSE, lat0 + (i + a * 0.2) / COARSE);
            if (std::isfinite(h)) sum += h, ++n;
          }
      c->v[i][j] = n ? float(sum / n) : 0.0f;
    }
  std::lock_guard lk(mu_);
  if (const Coarse* old = coarse_[k].load()) return old;
  coarse_store_.push_back(std::move(c));
  coarse_[k].store(coarse_store_.back().get(), std::memory_order_release);
  return coarse_store_.back().get();
}

double Terrain::geoid(double lat, double lon) {
  std::call_once(geoid_once_, [&] {
    const fs::path p = store::cache_dir() / "dem" / "us_nga_egm96_15.tif";
    auto b = read_file(p);
    if (!b && online_) {
      long status = 0;
      std::string body = fetch(GEOID_URL, &status);
      if (!body.empty()) {
        fs::create_directories(p.parent_path());
        atomic_write(p, body);
        b = std::move(body);
      }
    }
    if (b) geoid_ = std::make_shared<const Raster>(read_geotiff(*b));
    else missing_ = true;
  });
  if (!geoid_) return 0.0;
  const Raster& g = *geoid_;
  double x = lon;
  if (x >= g.x0 + g.dx * g.w - 1e-9) x -= 360.0;  // the grid ends a step before +180
  double n = g.sample(x, lat);
  if (!std::isfinite(n)) n = g.sample(x - 360.0, lat);
  return std::isfinite(n) ? n : 0.0;
}

double Terrain::ground(double lat, double lon) {
  const double n = geoid(lat, lon);
  auto r = full(int(std::floor(lat)), int(std::floor(lon)));
  if (!r) return n;
  double h = r->sample(lon, lat);
  if (!std::isfinite(h)) h = 0.0;  // voids: sea level
  return h + n;
}

double Terrain::ground_coarse(double lat, double lon) {
  const int la = int(std::floor(lat)), lo = int(std::floor(lon));
  const Coarse* c = coarse(la, lo);
  const double fy = (lat - la) * COARSE, fx = (lon - lo) * COARSE;
  const int i = std::clamp(int(fy), 0, COARSE - 1), j = std::clamp(int(fx), 0, COARSE - 1);
  const double a = fy - i, b = fx - j;
  const double h = (1 - a) * (1 - b) * c->v[i][j] + (1 - a) * b * c->v[i][j + 1] + a * (1 - b) * c->v[i + 1][j] + a * b * c->v[i + 1][j + 1];
  return h + geoid(lat, lon);
}

}  // namespace geo::air
