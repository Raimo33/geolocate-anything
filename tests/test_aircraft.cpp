// Aircraft Doppler: acoustic model, flyover detection, track store, GeoTIFF reader and
// the location search, on synthetic data (no network: the type tables are written into
// the test cache before first use).
#include <gtest/gtest.h>
#include <zlib.h>

#include <cmath>
#include <cstring>
#include <fstream>
#include <numbers>
#include <random>

#include "air/adsb.hpp"
#include "air/doppler.hpp"
#include "air/events.hpp"
#include "air/geodesy.hpp"
#include "air/search.hpp"
#include "air/spectrogram.hpp"
#include "air/terrain.hpp"
#include "helpers.hpp"
#include "store/store.hpp"
#include "util/files.hpp"

using namespace geo;
using namespace geo::air;

namespace {

std::string gzip(std::string_view in) {
  z_stream z{};
  deflateInit2(&z, 6, Z_DEFLATED, 16 + MAX_WBITS, 8, Z_DEFAULT_STRATEGY);
  std::string out(deflateBound(&z, uLong(in.size())) + 32, '\0');
  z.next_in = reinterpret_cast<Bytef*>(const_cast<char*>(in.data()));
  z.avail_in = uInt(in.size());
  z.next_out = reinterpret_cast<Bytef*>(out.data());
  z.avail_out = uInt(out.size());
  deflate(&z, Z_FINISH);
  out.resize(z.total_out);
  deflateEnd(&z);
  return out;
}

// The registry and designator tables, written once into whatever cache is current
// (the singletons load them on first use and keep them).
void seed_tables() {
  static bool done = false;
  if (done) return;
  done = true;
  const fs::path dir = store::cache_dir() / "adsb";
  fs::create_directories(dir);
  atomic_write(dir / "icao_aircraft_types.json",
               R"({"C172": {"desc": "L1P"}, "PC12": {"desc": "L1T"}, "B350": {"desc": "L2T"}, "E55P": {"desc": "L2J"}, "R44": {"desc": "H1P"}})");
  atomic_write(dir / "aircraft.csv.gz", gzip("A00001;N1;C172;00;;;\nA00002;N2;E55P;00;;;\nA00003;N3;PC12;00;;;\n"));
}

// Straight, level flight in the ENU frame of `f`: position at t, constant velocity.
struct Straight {
  Enu f;
  Vec3 p0, vel;  // ENU at t0
  double t0;
  Vec3 enu(double t) const { return p0 + vel * (t - t0); }
};

Track track_of(const Straight& L, std::uint32_t icao, const std::string& type, double ta, double tb, double step = 5.0) {
  Track tr;
  tr.icao = icao, tr.type = type;
  tr.info.engine = EngineType::Piston;
  for (double t = ta; t <= tb + 1e-9; t += step) {
    const Vec3 pe = L.f.unrot(L.enu(t)) + L.f.o;
    const Geodetic g = geodetic(pe);
    // Velocity at the aircraft's own position: rotate the ENU velocity through ECEF.
    const Vec3 ve = Enu(g.lat, g.lon).rot(L.f.unrot(L.vel));
    Fix x;
    x.t = t, x.lat = g.lat, x.lon = g.lon, x.alt_m = float(g.h);
    x.gs = float(std::hypot(ve.x, ve.y));
    x.track = float(std::fmod(std::atan2(ve.x, ve.y) / DEG + 360.0, 360.0));
    x.vrate = float(ve.z);
    tr.fixes.push_back(x);
  }
  return tr;
}

Path ecef_path(const Track& tr) {
  std::vector<PathPoint> pts;
  for (auto& f : tr.fixes) pts.push_back({f.t, ecef(f.lat, f.lon, f.alt_m), velocity_ecef(f.lat, f.lon, f.gs, f.track, f.vrate)});
  return Path(std::move(pts));
}

// Independent renderer: phase integration of each source's lines, emission time solved
// every 10 ms, 1/r amplitude, white noise.
std::vector<double> render(const std::vector<std::pair<Path, std::vector<double>>>& srcs, const Vec3& o, double start, double dur, double amp,
                           double noise, unsigned seed) {
  const int rate = SPEC_RATE;
  const std::size_t n = std::size_t(dur * rate);
  std::vector<double> x(n);
  std::mt19937_64 rng(seed);
  std::normal_distribution<double> nd(0, noise);
  for (auto& v : x) v = nd(rng);
  for (auto& [path, f0s] : srcs) {
    std::vector<double> phase(f0s.size(), 0.0);
    double ratio = NAN, gain = 0;
    for (std::size_t i = 0; i < n; ++i) {
      if (i % 40 == 0) {
        auto em = emission_for(path, o, start + double(i) / rate);
        ratio = em ? em->ratio() : NAN;
        gain = em ? amp * 1000.0 / em->r : 0;
      }
      if (!std::isfinite(ratio)) continue;
      for (std::size_t k = 0; k < f0s.size(); ++k) {
        phase[k] += 2 * std::numbers::pi * f0s[k] * ratio / rate;
        x[i] += gain / double(k + 1) * std::sin(phase[k]);
      }
    }
  }
  return x;
}

}  // namespace

TEST(AircraftDoppler, ClosedFormMatchesRetardedTimeSolver) {
  // A straight pass at 120 m/s, closest approach 1500 m at t = 1000 (emission).
  const double v = 120, d = 1500, tcpa = 1000;
  std::vector<PathPoint> pts;
  for (double t = 800; t <= 1200; t += 10) pts.push_back({t, {v * (t - tcpa), d, 0}, {v, 0, 0}});
  Path path(pts);
  const Vec3 o{0, 0, 0};
  for (double t = 900; t <= 1100; t += 7.3) {
    auto em = emission_for(path, o, t);
    ASSERT_TRUE(em);
    EXPECT_NEAR(em->ratio(), cv_ratio(t, tcpa + d / SOUND_SPEED, v, d), 1e-7) << t;
    EXPECT_NEAR(em->tau + em->r / SOUND_SPEED, t, 1e-3);
  }
  // At the delayed CPA the tone is at rest frequency; before it, higher.
  EXPECT_NEAR(cv_ratio(tcpa + d / SOUND_SPEED, tcpa + d / SOUND_SPEED, v, d), 1.0, 1e-12);
  EXPECT_GT(cv_ratio(tcpa, tcpa + d / SOUND_SPEED, v, d), 1.0);
  auto ap = closest_approach(path, o);
  EXPECT_NEAR(ap.r, d, 0.01);
  EXPECT_NEAR(ap.tau, tcpa, 0.01);
  EXPECT_TRUE(ap.interior);
  // Emission is not reception delayed by a constant: the two curves differ in shape.
  // 30 s after the delayed CPA the emission is ~24.6 s after closest approach, not 30.
  const double t30 = tcpa + d / SOUND_SPEED + 30;
  auto naive = [&](double t) {  // radial velocity at t - d/c: the curve delayed as a whole
    const double x = v * (t - d / SOUND_SPEED - tcpa), r = std::hypot(x, d);
    return SOUND_SPEED / (SOUND_SPEED + v * x / r);
  };
  EXPECT_GT(std::abs(naive(t30) - cv_ratio(t30, tcpa + d / SOUND_SPEED, v, d)), 0.005);
}

TEST(AircraftDoppler, GeodeticRoundTrip) {
  for (double h : {-30.0, 0.0, 1500.0, 11000.0}) {
    const auto g = geodetic(ecef(40.87693, -73.52498, h));
    EXPECT_NEAR(g.lat, 40.87693, 1e-9);
    EXPECT_NEAR(g.lon, -73.52498, 1e-9);
    EXPECT_NEAR(g.h, h, 1e-4);
  }
}

TEST(AircraftEvents, DetectsAndMeasuresASyntheticFlyover) {
  // 110 Hz + 220 Hz, 125 m/s, 2.5 km slant, CPA heard at 200 s of a 400 s clip.
  const double v = 125, d = 2500, tc = 200;
  std::vector<PathPoint> pts;
  const double tcpa = tc - d / SOUND_SPEED;
  for (double t = -100; t <= 500; t += 5) pts.push_back({t, {v * (t - tcpa), d * 0.6, d * 0.8}, {v, 0, 0}});
  auto x = render({{Path(pts), {110.0, 220.0}}}, {0, 0, 0}, 0, 400, 0.6, 1.0, 7);
  auto spec = whitened_spectrogram(x);
  auto det = detect_events(spec);
  ASSERT_GE(det.events.size(), 1u);
  const Event* e = &det.events[0];
  for (auto& ev : det.events)
    if (ev.score > e->score) e = &ev;
  EXPECT_NEAR(e->tc, tc, 1.0);
  EXPECT_LE(e->v_lo, v + 5);
  EXPECT_GE(e->v_hi, v - 5);
  EXPECT_LE(e->d_lo, d * 1.2);
  EXPECT_GE(e->d_hi, d / 1.2);
  bool f110 = false, f220 = false;
  for (auto& l : e->lines) f110 |= std::abs(l.f0 - 110) < 1.5, f220 |= std::abs(l.f0 - 220) < 2.5;
  EXPECT_TRUE(f110 && f220);
  // Noise alone: nothing above the threshold.
  auto quiet = whitened_spectrogram(render({}, {0, 0, 0}, 0, 400, 0, 1.0, 9));
  EXPECT_TRUE(detect_events(quiet).events.empty());
}

TEST(AircraftStore, EncodeDecodeRoundTrip) {
  Track a;
  a.icao = 0xa1dfeb, a.type = "PC12", a.reg = "N22NY";
  a.info = {'L', 1, EngineType::Turboprop};
  for (int i = 0; i < 50; ++i) {
    Fix f;
    f.t = 1788898649.25 + i * 4.5, f.lat = 40.8 + i * 1e-3, f.lon = -73.5 - i * 2e-3;
    f.alt_m = 1500.5f + float(i), f.gs = 130, f.track = 271.5f, f.vrate = -1.5f;
    f.alt_src = i % 2 ? AltSource::BaroTrack : AltSource::Geometric;
    f.leg_start = i == 10;
    a.fixes.push_back(f);
  }
  auto back = decode_tracks(encode_tracks({a}));
  ASSERT_EQ(back.size(), 1u);
  EXPECT_EQ(back[0].id(), "a1dfeb");
  EXPECT_EQ(back[0].type, "PC12");
  EXPECT_EQ(back[0].reg, "N22NY");
  EXPECT_EQ(back[0].info.engine, EngineType::Turboprop);
  ASSERT_EQ(back[0].fixes.size(), 50u);
  for (std::size_t i = 0; i < 50; ++i) {
    EXPECT_NEAR(back[0].fixes[i].t, a.fixes[i].t, 1e-3);
    EXPECT_NEAR(back[0].fixes[i].lat, a.fixes[i].lat, 1e-6);
    EXPECT_NEAR(back[0].fixes[i].lon, a.fixes[i].lon, 1e-6);
    EXPECT_EQ(back[0].fixes[i].alt_m, a.fixes[i].alt_m);
    EXPECT_EQ(back[0].fixes[i].alt_src, a.fixes[i].alt_src);
    EXPECT_EQ(back[0].fixes[i].leg_start, a.fixes[i].leg_start);
  }
  // Leg starts and gaps split a track into segments.
  auto segs = split_tracks(back, 120.0);
  EXPECT_EQ(segs.size(), 2u);
}

TEST(AircraftStore, IngestsADayFromLocalTarParts) {
  seed_tables();
  // A tar of three traces: a Cessna with geometric and barometric altitude, a jet
  // (dropped by its registry type although the trace has none), one only on the ground.
  auto trace = [](const std::string& icao, const std::string& t, const std::string& pts) {
    return R"({"icao":")" + icao + R"(",)" + (t.empty() ? "" : R"("t":")" + t + R"(",)") + R"("r":"N1","timestamp": 1767225600.000,"trace":[)" + pts + "]}";
  };
  const std::string c172 = trace("a00001", "C172",
                                 R"([0.5,40.0,-100.0,3000,100.0,90.0,0,0,null,"adsb_icao",3100,null,null,null],)"
                                 R"([10.5,40.0,-99.99,3000,100.0,90.0,0,0,null,"adsb_icao",3100,null,null,null],)"
                                 R"([20.5,40.0,-99.98,3000,100.0,90.0,0,0,null,"adsb_icao",null,null,null,null],)"
                                 R"([30.5,40.0,-99.97,3000,100.0,90.0,0,0,null,"adsb_icao",3100,null,null,null])");
  const std::string jet = trace("a00002", "", R"([0.5,41.0,-100.0,30000,400.0,90.0,0,0,null,"adsb_icao",30500,null,null,null])");
  const std::string gnd = trace("a00009", "C172", R"([0.5,41.0,-100.0,"ground",5.0,90.0,0,null,null,"adsb_icao",null,null,null,null])");
  std::string tar;
  auto add = [&](const std::string& name, const std::string& data) {
    std::string h(512, '\0');
    std::memcpy(h.data(), name.data(), name.size());
    std::snprintf(h.data() + 100, 8, "%07o", 0644);
    std::snprintf(h.data() + 124, 12, "%011llo", (unsigned long long)data.size());
    h[156] = '0';
    std::memcpy(h.data() + 257, "ustar", 5);
    unsigned sum = 0;
    std::memset(h.data() + 148, ' ', 8);
    for (char c : h) sum += static_cast<unsigned char>(c);
    std::snprintf(h.data() + 148, 8, "%06o", sum);
    tar += h + data + std::string((512 - data.size() % 512) % 512, '\0');
  };
  add("./README.txt", "hello");
  add("./traces/01/trace_full_a00001.json", gzip(c172));
  add("./traces/02/trace_full_a00002.json", gzip(jet));
  add("./traces/09/trace_full_a00009.json", gzip(gnd));
  tar += std::string(1024, '\0');
  const fs::path dir = test::make_temp_dir("tar");
  // Split at an awkward offset, as the release's parts are.
  atomic_write(dir / "day.tar.aa", tar.substr(0, 777));
  atomic_write(dir / "day.tar.ab", tar.substr(777));
  IngestOptions o;
  o.parts = {(dir / "day.tar.aa").string(), (dir / "day.tar.ab").string()};
  auto st = ingest_day("2026-01-01", o);
  EXPECT_EQ(st.members, 3u);
  EXPECT_EQ(st.kept, 1u);
  EXPECT_EQ(st.jets, 1u);
  EXPECT_EQ(st.fixes, 4u);
  EXPECT_EQ(st.baro_track, 1u);  // the fix without geometric altitude: per-aircraft offset
  auto tr = load_tracks(1767225600.0, 1767225700.0);
  ASSERT_EQ(tr.size(), 1u);
  EXPECT_EQ(tr[0].type, "C172");
  ASSERT_EQ(tr[0].fixes.size(), 4u);
  EXPECT_NEAR(tr[0].fixes[0].alt_m, 3100 * FT, 0.01);
  EXPECT_NEAR(tr[0].fixes[2].alt_m, 3100 * FT, 0.01);  // 3000 ft baro + 100 ft offset
  EXPECT_NEAR(tr[0].fixes[0].gs, 100 * KT, 1e-3);
  std::error_code ec;
  fs::remove_all(dir, ec);
}

TEST(AircraftTerrain, ReadsFloatGeoTiffWithPredictor) {
  // A 4 x 3 float raster, tiled 4 x 3, deflate + floating-point predictor, point-registered
  // at (-74, 41) with 0.5 deg steps.
  const int w = 4, h = 3;
  std::vector<float> v(w * h);
  for (int i = 0; i < w * h; ++i) v[std::size_t(i)] = 10.0f * float(i) - 3.25f;
  std::string raw(std::size_t(w * h * 4), '\0');
  for (int y = 0; y < h; ++y) {
    std::vector<unsigned char> planes(std::size_t(w) * 4);
    for (int x = 0; x < w; ++x) {
      std::uint32_t u;
      std::memcpy(&u, &v[std::size_t(y * w + x)], 4);
      for (int b = 0; b < 4; ++b) planes[std::size_t(b * w + x)] = static_cast<unsigned char>(u >> (24 - 8 * b));
    }
    for (std::size_t i = planes.size(); i-- > 1;) planes[i] = static_cast<unsigned char>(planes[i] - planes[i - 1]);
    std::memcpy(raw.data() + std::size_t(y) * w * 4, planes.data(), planes.size());
  }
  uLongf clen = compressBound(uLong(raw.size()));
  std::string comp(clen, '\0');
  compress2(reinterpret_cast<Bytef*>(comp.data()), &clen, reinterpret_cast<const Bytef*>(raw.data()), uLong(raw.size()), 6);
  comp.resize(clen);
  // Little-endian TIFF: header, one IFD of 15 entries, then the values and the tile.
  std::string t = "II";
  auto u16 = [&](std::uint16_t x) { t.append(reinterpret_cast<const char*>(&x), 2); };
  auto u32 = [&](std::uint32_t x) { t.append(reinterpret_cast<const char*>(&x), 4); };
  auto f64 = [&](double x) { t.append(reinterpret_cast<const char*>(&x), 8); };
  const std::uint32_t n = 15, scale_at = 8 + 2 + n * 12 + 4, tie_at = scale_at + 24, keys_at = tie_at + 48, data_at = keys_at + 32;
  struct E {
    std::uint16_t tag, type;
    std::uint32_t count, value;
  };
  const std::vector<E> es = {{256, 3, 1, w},       {257, 3, 1, h},         {258, 3, 1, 32},        {259, 3, 1, 8},      {262, 3, 1, 1},
                             {277, 3, 1, 1},       {317, 3, 1, 3},         {322, 3, 1, w},         {323, 3, 1, h},      {324, 4, 1, data_at},
                             {325, 4, 1, std::uint32_t(comp.size())},       {339, 3, 1, 3},         {33550, 12, 3, scale_at},
                             {33922, 12, 6, tie_at}, {34735, 3, 16, keys_at}};
  u16(42), u32(8), u16(std::uint16_t(n));
  for (auto& e : es) u16(e.tag), u16(e.type), u32(e.count), u32(e.value);
  u32(0);
  f64(0.5), f64(0.5), f64(0);
  f64(0), f64(0), f64(0), f64(-74), f64(41), f64(0);
  for (std::uint16_t k : {1, 1, 0, 3, 1024, 0, 1, 2, 1025, 0, 1, 2, 2048, 0, 1, 4326}) u16(k);
  ASSERT_EQ(t.size(), data_at);
  t += comp;
  auto r = read_geotiff(t);
  EXPECT_EQ(r.w, w);
  EXPECT_EQ(r.h, h);
  EXPECT_FALSE(r.area);
  EXPECT_FLOAT_EQ(float(r.sample(-74.0, 41.0)), v[0]);
  EXPECT_FLOAT_EQ(float(r.sample(-73.5, 40.5)), v[5]);
  EXPECT_NEAR(r.sample(-73.75, 41.0), 0.5 * (v[0] + v[1]), 1e-4);
}

TEST(AircraftSearch, FindsTheListenerFromTwoFlyovers) {
  // Listener at 40.0 N, 100.0 W (ground at the geoid: the test cache has no DEM, and
  // the search runs offline). Two passes 600 s apart, plus decoys: one aircraft at the
  // first pass's speed and time 25 km away, one at the second's speed 40 s late.
  const double lat0 = 40.0, lon0 = -100.0, start = 1767261600.0;  // 2026-01-01 10:00 UTC
  const Enu f(lat0, lon0, 0.0);
  const double tA = start + 300, tB = start + 900;  // CPA reception times
  Straight A{f, {0, 0, 0}, {120, 0, 0}, 0};
  A.p0 = {0, 1800, 1200};  // passes 1.8 km north at 1200 m
  A.t0 = tA - std::hypot(1800.0, 1200.0 - 2.0) / SOUND_SPEED;
  Straight B{f, {900, 0, 800}, {0, -90, 0}, 0};
  B.t0 = tB - std::hypot(900.0, 800.0 - 2.0) / SOUND_SPEED;
  Straight C{f, {-5000, 25000, 1200}, {120, 0, 0}, tA - 6};
  Straight D{f, {-15000, 8000, 800}, {0, -90, 0}, tB + 40};
  std::vector<Track> tracks = {track_of(A, 0xa00010, "C172", start - 400, start + 1600), track_of(B, 0xa00011, "C172", start - 400, start + 1600),
                               track_of(C, 0xa00012, "C172", start - 400, start + 1600), track_of(D, 0xa00013, "C172", start - 400, start + 1600)};
  const Vec3 o = ecef(lat0, lon0, 2.0);
  auto x = render({{ecef_path(tracks[0]), {105.0, 210.0}}, {ecef_path(tracks[1]), {92.0}}}, o, start, 1200, 0.5, 1.0, 3);
  auto spec = whitened_spectrogram(x);
  auto det = detect_events(spec);
  ASSERT_GE(det.events.size(), 2u);
  Terrain terrain(false);
  SearchOptions so;
  so.start_min = start - 1800, so.start_max = start + 1800;
  so.box = {lon0 - 0.4, lat0 - 0.3, lon0 + 0.4, lat0 + 0.3};
  auto r = search(spec, det.events, split_tracks(tracks), terrain, so);
  ASSERT_FALSE(r.candidates.empty());
  const auto& c = r.candidates[0];
  EXPECT_LT(haversine_m(c.lat, c.lon, lat0, lon0), 150.0) << c.lat << ", " << c.lon;
  EXPECT_NEAR(c.start_utc, start, 0.5);
  ASSERT_EQ(c.matches.size(), 2u);
  EXPECT_EQ(c.matches[0].flight, "a00010");
  EXPECT_EQ(c.matches[1].flight, "a00011");
  EXPECT_GE(r.level, AirLevel::Moderate);
  // The same with the start known to a few seconds and one flyover allowed.
  so.start_min = start - 3, so.start_max = start + 3, so.min_events = 1;
  auto r1 = search(spec, det.events, split_tracks(tracks), terrain, so);
  ASSERT_FALSE(r1.candidates.empty());
  EXPECT_LT(haversine_m(r1.candidates[0].lat, r1.candidates[0].lon, lat0, lon0), 150.0);
}
