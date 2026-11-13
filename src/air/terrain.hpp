// Ground height above the WGS84 ellipsoid, for the listener's position.
//
// ADS-B geometric altitude is height above the ellipsoid; a slant distance needs the
// listener's height in the same datum. Copernicus GLO-90 (90 m DEM, heights above the
// EGM2008 geoid, one 1 deg GeoTIFF per tile on a public bucket) plus the EGM96 geoid
// undulation (PROJ's 15' grid; EGM96 and EGM2008 differ by ~1 m, far below the DEM's own
// error). Tiles are fetched once and cached under <cache>/dem/; a tile the bucket does
// not have is sea, height = geoid. Offline (or when a download fails) the height falls
// back to the geoid alone, and `complete()` says so.
#pragma once

#include <atomic>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "air/adsb.hpp"

namespace geo::air {

// A point-registered float raster (one band) decoded from a GeoTIFF.
struct Raster {
  int w = 0, h = 0;
  double x0 = 0, y0 = 0, dx = 1, dy = 1;  // centre of pixel (0,0); steps (dy > 0 means south)
  bool area = false;                      // PixelIsArea: (x0, y0) is the corner of pixel (0,0)
  float nodata = NAN;
  std::vector<float> v;
  // Bilinear value at (x = lon, y = lat); NaN outside or on nodata.
  double sample(double x, double y) const;
};
Raster read_geotiff(std::string_view bytes);

class Terrain {
 public:
  explicit Terrain(bool online = true);
  ~Terrain();
  // Ground height above the ellipsoid, m, from the full 90 m DEM (tiles loaded on demand,
  // the last 32 kept). Thread-safe.
  double ground(double lat, double lon);
  // The same from block means of 1/40 deg (~2.8 km): for screening wide areas without
  // holding full tiles. Lock-free once a tile's summary exists.
  double ground_coarse(double lat, double lon);
  double geoid(double lat, double lon);
  bool complete() const { return !missing_; }  // false: some heights are the geoid alone

  static constexpr int COARSE = 40;  // blocks per degree

 private:
  struct Coarse {
    float v[COARSE + 1][COARSE + 1];  // block means at block centres, edges repeated
  };
  std::shared_ptr<const Raster> load_tile(int lat0, int lon0, bool* sea);  // downloads as needed; null: sea or missing
  std::shared_ptr<const Raster> full(int lat0, int lon0);
  const Coarse* coarse(int lat0, int lon0);
  bool online_;
  std::atomic<bool> missing_ = false;
  std::mutex mu_, dl_mu_;
  std::vector<std::pair<std::pair<int, int>, std::shared_ptr<const Raster>>> lru_;  // most recent last
  std::unique_ptr<std::atomic<const Coarse*>[]> coarse_;  // 180 x 360, owned
  std::vector<std::unique_ptr<Coarse>> coarse_store_;
  std::shared_ptr<const Raster> geoid_;
  std::once_flag geoid_once_;
};

}  // namespace geo::air
