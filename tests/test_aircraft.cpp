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
#include <thread>
#include <chrono>

#include "air/adsb.hpp"
#include "air/doppler.hpp"
#include "air/events.hpp"
#include "air/fusion.hpp"
#include "air/geodesy.hpp"
#include "air/opensky.hpp"
#include "air/ridge.hpp"
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

// A pass along x at constant acceleration a: speed v and slant d at closest approach,
// reached at epoch tcpa (emission), sampled every 2 s.
Path accelerating_pass(double v, double a, double d, double tcpa, double t0, double t1) {
  std::vector<PathPoint> pts;
  for (double t = t0; t <= t1 + 1e-9; t += 2) {
    const double u = t - tcpa;
    if (v + a * u < 8) continue;
    pts.push_back({t, {v * u + 0.5 * a * u * u, d * 0.6, d * 0.8}, {v + a * u, 0, 0}});
  }
  return Path(std::move(pts));
}

// Speech-like clutter: a harmonic stack (10 harmonics, 1/k) whose pitch glides 110-260 Hz,
// switched on and off at the syllable rate (bursts of 0.12-0.3 s, gaps of 0.05-0.2 s,
// pauses of 0.5-1.5 s between phrases).
void add_speech(std::vector<double>& x, double t0, double t1, double amp, unsigned seed) {
  const int rate = SPEC_RATE;
  std::mt19937_64 rng(seed);
  std::uniform_real_distribution<double> U(0, 1);
  double t = t0, ph = 0;
  while (t < t1) {
    const int syll = 3 + int(U(rng) * 6);
    for (int k = 0; k < syll && t < t1; ++k) {
      const double len = 0.12 + 0.18 * U(rng), p0 = 110 + 150 * U(rng), p1 = 110 + 150 * U(rng);
      for (std::size_t i = std::size_t(t * rate); i < std::min(x.size(), std::size_t((t + len) * rate)); ++i) {
        const double u = (double(i) / rate - t) / len, pitch = p0 + (p1 - p0) * u, env = std::sin(std::numbers::pi * u);
        ph += 2 * std::numbers::pi * pitch / rate;
        double y = 0;
        for (int h = 1; h <= 10; ++h) y += std::sin(h * ph) / h;
        x[i] += amp * env * y;
      }
      t += len + 0.05 + 0.15 * U(rng);
    }
    t += 0.5 + U(rng);
  }
}

std::string describe(const Detection& det) {
  std::string m = "threshold " + std::to_string(det.threshold);
  for (auto* L : {&det.events, &det.rejected, &det.duplicates})
    for (auto& e : *L)
      m += std::string(L == &det.events ? "\n kept" : L == &det.rejected ? "\n rejected" : "\n duplicate") + " tc " + std::to_string(e.tc) +
           " v " + std::to_string(e.v) + " d " + std::to_string(e.d) + " ridge " + std::to_string(e.ridge_hz) + " Hz, detect " +
           std::to_string(e.detect_score) + " cov " + std::to_string(e.ridge_coverage) + " rmse " + std::to_string(e.ridge_rmse_hz) +
           " acc " + std::to_string(e.accel);
  return m;
}

}  // namespace

TEST(AircraftDoppler, AcceleratingPassModel) {
  // a = 0 is the closed form; a != 0 matches brute-force emission times of the same motion.
  const double v = 60, d = 900, tc = 100, c = SOUND_SPEED;
  for (double t : {40.0, 90.0, 100.0, 103.0, 160.0}) EXPECT_NEAR(ca_ratio(t, tc, v, d, 0.0), cv_ratio(t, tc, v, d), 1e-12);
  for (double a : {-0.8, 0.6})
    for (double t : {50.0, 95.0, 100.0, 108.0, 150.0}) {
      // reception t = tc + tau + (r(tau) - d)/c: bisection on tau.
      double lo = -200, hi = 200;
      for (int i = 0; i < 200; ++i) {
        const double m = 0.5 * (lo + hi), s = v * m + 0.5 * a * m * m;
        (tc + m + (std::hypot(s, d) - d) / c < t ? lo : hi) = m;
      }
      const double tau = 0.5 * (lo + hi), s = v * tau + 0.5 * a * tau * tau, u = v + a * tau;
      EXPECT_NEAR(ca_ratio(t, tc, v, d, a), c / (c + s * u / std::hypot(s, d)), 1e-9) << "a " << a << " t " << t;
    }
}

TEST(AircraftEvents, ShortClipWithOnePass) {
  // A 100 s clip filled by one pass (the Livermore recordings): the threshold comes from
  // rising templates, which the pass does not feed; before, the pass lifted its own threshold.
  const double v = 80, d = 1500, tc = 50;
  auto path = accelerating_pass(v, 0.0, d, tc - d / SOUND_SPEED, -100, 200);
  auto x = render({{path, {121.0, 242.0}}}, {0, 0, 0}, 0, 100, 1.0, 1.0, 11);
  auto det = detect_events(whitened_spectrogram(x));
  ASSERT_EQ(det.events.size(), 1u) << describe(det);
  const Event& e = det.events[0];
  EXPECT_NEAR(e.tc, tc, 1.5);
  EXPECT_LE(e.v_lo, v + 3);
  EXPECT_GE(e.v_hi, v - 3);
  EXPECT_LE(e.d_lo, d * 1.15);
  EXPECT_GE(e.d_hi, d / 1.15);
  EXPECT_GT(e.ridge_coverage, 0.5);
  EXPECT_LT(e.ridge_rmse_hz, 0.6);
}

TEST(AircraftEvents, RidgeFitMeasuresASlowingPass) {
  // Slowing at 0.8 m/s^2 through closest approach (an aircraft on approach): the template is
  // a straight constant-speed pass, the ridge fit recovers the speed at CPA and the deceleration.
  const double v = 55, a = -0.8, d = 600, tc = 90;
  auto path = accelerating_pass(v, a, d, tc - d / SOUND_SPEED, -60, 250);
  auto x = render({{path, {140.0, 280.0}}}, {0, 0, 0}, 0, 180, 0.6, 1.0, 13);
  auto spec = whitened_spectrogram(x);
  auto det = detect_events(spec);
  ASSERT_GE(det.events.size(), 1u) << describe(det);
  const Event& e = det.events[0];
  EXPECT_LE(e.tc_lo, tc + 1.0) << describe(det);
  EXPECT_GE(e.tc_hi, tc - 1.0) << describe(det);
  EXPECT_NEAR(e.accel, a, 0.35);
  EXPECT_LE(e.v_lo, v + 3);
  EXPECT_GE(e.v_hi, v - 3);
  const Ridge r = trace_ridge(spec, e.tc, e.v, e.d, e.ridge_hz, e.half_window_s);
  const PassFit f = fit_pass(r, e.tc, e.v, e.d, e.ridge_hz);
  ASSERT_TRUE(f.ok);
  EXPECT_NEAR(f.tc, tc, 1.5);
  EXPECT_NEAR(f.v, v, 6.0);
  EXPECT_NEAR(f.d, d, 0.3 * d);
}

TEST(AircraftEvents, SpeechIsMaskedAndNotAFlyover) {
  // Ten minutes of speech-like clutter over noise: flagged as speech, no flyover. With a
  // pass under the same speech, the pass is still found.
  auto x = render({}, {0, 0, 0}, 0, 600, 0, 1.0, 17);
  add_speech(x, 0, 600, 10.0, 19);
  auto spec = whitened_spectrogram(x);
  EXPECT_GT(spec.speech_fraction, 0.5);
  {
    auto d0 = detect_events(spec);
    EXPECT_TRUE(d0.events.empty()) << describe(d0);
  }
  const double v = 70, d = 1200, tc = 300;
  auto path = accelerating_pass(v, 0.0, d, tc - d / SOUND_SPEED, -200, 800);
  auto y = render({{path, {118.0, 236.0}}}, {0, 0, 0}, 0, 600, 1.5, 1.0, 17);
  add_speech(y, 0, 600, 10.0, 19);
  auto det = detect_events(whitened_spectrogram(y));
  ASSERT_GE(det.events.size(), 1u) << describe(det);
  bool found = false;
  for (auto& e : det.events) found |= std::abs(e.tc - tc) < 3 && e.v_lo <= v + 5 && e.v_hi >= v - 5;
  EXPECT_TRUE(found) << describe(det);
}

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
  // A PC-12 at 70 m/s: 200 s straight, a 3 deg/s turn through 180 deg, 200 s straight
  // climbing 3 m/s; a new leg after a 300 s gap. Fixes every 2 s with 3 m of jitter
  // and 0.5 m/s on the reported velocity, some velocities missing.
  Track a;
  a.icao = 0xa1dfeb, a.type = "PC12", a.reg = "N22NY";
  a.info = {'L', 1, EngineType::Turboprop};
  const double t0 = 1788898649.25, v = 70, w = 3 * DEG, lat0 = 40.8, lon0 = -73.5;
  const Enu enu(lat0, lon0);
  struct Truth {
    Vec3 p, v;  // ENU
  };
  auto truth = [&](double t) {
    if (t <= 200) return Truth{{0, v * t, 1500}, {0, v, 0}};
    const double tt = 60.0, r = v / w;  // turn: 60 s, centred east of the end of leg 1
    if (t <= 200 + tt) {
      const double th = w * (t - 200);
      return Truth{{r - r * std::cos(th), v * 200 + r * std::sin(th), 1500}, {v * std::sin(th), v * std::cos(th), 0}};
    }
    const double s = t - 200 - tt;
    return Truth{{2 * r, v * 200 - v * s, 1500 + 3 * s}, {0, -v, 3}};
  };
  std::mt19937 rng(7);
  std::normal_distribution<double> n3(0, 3), nv(0, 0.5);
  std::vector<double> times;
  for (double t = 0; t <= 460; t += 2) times.push_back(t);
  for (double t = 760; t <= 860; t += 2) times.push_back(t);
  for (double t : times) {
    const double tt = t >= 760 ? t - 760 + 460 : t;  // the second leg flies on
    const Truth tr = truth(tt);
    const Geodetic g = geodetic(enu.o + enu.unrot(tr.p + Vec3{n3(rng), n3(rng), n3(rng)}));
    Fix f;
    f.t = t0 + t, f.lat = g.lat, f.lon = g.lon, f.alt_m = float(g.h);
    const double ve = tr.v.x + nv(rng), vn = tr.v.y + nv(rng);
    f.gs = float(std::hypot(ve, vn)), f.track = float(std::fmod(std::atan2(ve, vn) / DEG + 360, 360)), f.vrate = float(tr.v.z + nv(rng));
    if (int(t) % 34 == 0) f.gs = f.track = NAN;
    f.alt_src = AltSource::BaroTrack;
    f.leg_start = t == 760;
    a.fixes.push_back(f);
  }
  auto back = decode_tracks(encode_tracks({a}));
  ASSERT_EQ(back.size(), 1u);
  EXPECT_EQ(back[0].id(), "a1dfeb");
  EXPECT_EQ(back[0].type, "PC12");
  EXPECT_EQ(back[0].reg, "N22NY");
  EXPECT_EQ(back[0].info.engine, EngineType::Turboprop);
  // A few nodes, not 283 fixes: the straight legs need hardly any.
  EXPECT_LT(back[0].fixes.size(), 40u);
  EXPECT_GE(back[0].fixes.size(), 6u);
  for (auto& f : back[0].fixes) EXPECT_EQ(f.alt_src, AltSource::BaroTrack);
  EXPECT_NEAR(back[0].fixes.front().t, t0, 0.5);
  EXPECT_NEAR(back[0].fixes.back().t, t0 + 860, 0.5);
  // Leg starts and gaps split a track into segments; the curve follows the true
  // trajectory (better than the jittered fixes did).
  auto segs = split_tracks(back, 120.0);
  ASSERT_EQ(segs.size(), 2u);
  EXPECT_TRUE(segs[1].fixes.front().leg_start);
  double worst_p = 0, worst_v = 0;
  std::vector<double> ev;
  for (auto& sg : segs) {
    const Path path = path_of(sg);
    for (double t = path.t0(); t <= path.t1(); t += 1) {
      const double tt = t - t0 >= 760 ? t - t0 - 760 + 460 : t - t0;
      if (tt > 458 && tt < 460) continue;  // the last fix of leg 1 is at 460: no extrapolation
      Vec3 p, vel;
      path.state(t, p, vel);
      const Truth tr = truth(tt);
      worst_p = std::max(worst_p, (enu.pos(p) - tr.p).norm());
      worst_v = std::max(worst_v, (enu.rot(vel) - tr.v).norm());
      ev.push_back((enu.rot(vel) - tr.v).norm());
    }
  }
  std::sort(ev.begin(), ev.end());
  EXPECT_LT(worst_p, 25.0);
  EXPECT_LT(ev[ev.size() / 2], 0.7);
  EXPECT_LT(worst_v, 8.0);  // at the turn's instant roll-in and roll-out (no aircraft does that)
}

// A bounded store (GEOLOC_AIRPLANE_KEEP_DAYS) never evicts a day a search in progress
// reads. Regression: a whole-day window needs three days; with KEEP_DAYS=2, pulling the
// third evicted the window's own day (used last by the previous analysis), and the search
// saw only the tracks after midnight.
TEST(AircraftStore, BoundedStoreKeepsTheDaysASearchReads) {
  const auto dir = test::make_temp_dir("adsb-pin");
  test::use_cache(dir);
  const char* bucket_env = std::getenv("GEOLOC_AIRPLANE_BUCKET");  // no pulls: local days only
  const std::string bucket = bucket_env ? bucket_env : "";
  ::unsetenv("GEOLOC_AIRPLANE_BUCKET");
  ::setenv("GEOLOC_AIRPLANE_KEEP_DAYS", "2", 1);
  fs::create_directories(adsb_dir());
  auto stub = [](const std::string& day) {  // has_day() reads only the store's magic
    std::ofstream(day_path(day), std::ios::binary) << "GLADSB03";
    mark_used(day);
  };
  auto stored = [](const std::string& day) { return fs::exists(day_path(day)); };
  stub("2024-04-07");  // the window's day, used last by an earlier analysis
  std::this_thread::sleep_for(std::chrono::milliseconds(20));
  {
    DayPinScope pins;
    EXPECT_TRUE(ensure_day("2024-04-07"));
    EXPECT_FALSE(ensure_day("2024-04-06"));  // pinned, then "pulled" (no bucket here):
    stub("2024-04-06");
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    EXPECT_FALSE(ensure_day("2024-04-08"));
    stub("2024-04-08");
    evict_days();  // three stored, two allowed: all three pinned, all three stay
    EXPECT_TRUE(stored("2024-04-07"));
    EXPECT_TRUE(stored("2024-04-06"));
    EXPECT_TRUE(stored("2024-04-08"));
  }
  // Unpinned again: the bound holds, keeping the days used last.
  evict_days();
  EXPECT_FALSE(stored("2024-04-07"));
  EXPECT_TRUE(stored("2024-04-06"));
  EXPECT_TRUE(stored("2024-04-08"));
  ::unsetenv("GEOLOC_AIRPLANE_KEEP_DAYS");
  if (!bucket.empty()) ::setenv("GEOLOC_AIRPLANE_BUCKET", bucket.c_str(), 1);
  std::error_code ec;
  fs::remove_all(dir, ec);
}

TEST(AircraftStore, IngestsADayFromLocalTarParts) {
  seed_tables();
  // A tar of three traces: a Cessna with geometric and barometric altitude, a jet
  // (dropped by its registry type although the trace has none), one only on the ground.
  auto trace = [](const std::string& icao, const std::string& t, const std::string& pts) {
    return R"({"icao":")" + icao + R"(",)" + (t.empty() ? "" : R"("t":")" + t + R"(",)") + R"("r":"N1","timestamp": 1767225600.000,"trace":[)" + pts + "]}";
  };
  const std::string c172 = trace("a00001", "C172",
                                 R"([0.5,40.0,-100.0,3000,165.8,90.0,0,0,null,"adsb_icao",3100,null,null,null],)"
                                 R"([10.5,40.0,-99.99,3000,165.8,90.0,0,0,null,"adsb_icao",3100,null,null,null],)"
                                 R"([20.5,40.0,-99.98,3000,165.8,90.0,0,0,null,"adsb_icao",null,null,null,null],)"
                                 R"([30.5,40.0,-99.97,3000,165.8,90.0,0,0,null,"adsb_icao",3100,null,null,null])");
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
  // A straight, level, steady flight: its two ends are all the store keeps.
  ASSERT_EQ(tr[0].fixes.size(), 2u);
  EXPECT_NEAR(tr[0].fixes[0].t, 1767225600.5, 1e-6);
  EXPECT_NEAR(tr[0].fixes[1].t, 1767225630.5, 1e-6);
  EXPECT_NEAR(tr[0].fixes[1].lon, -99.97, 5e-5);
  // 3100 ft throughout: the barometric fix (3000 ft + the aircraft's 100 ft offset) too.
  for (auto& f : tr[0].fixes) {
    EXPECT_NEAR(f.alt_m, 3100 * FT, 2.5);
    EXPECT_NEAR(f.gs, 165.8 * KT, 0.5);
  }
  // The chunk index (built at ingest) picks aircraft by box, window and speed.
  EXPECT_TRUE(fs::exists(index_path("2026-01-01")));
  EXPECT_EQ(load_tracks(1767225600.0, 1767225700.0, Bbox{-100.1, 39.9, -99.9, 40.1}).size(), 1u);
  EXPECT_TRUE(load_tracks(1767225600.0, 1767225700.0, Bbox{-99.0, 39.9, -98.5, 40.1}).empty());
  EXPECT_TRUE(load_tracks(1767225600.0 + 3600, 1767225700.0 + 3600).empty());                        // an hour later
  EXPECT_EQ(load_tracks(1767225600.0, 1767225700.0, std::nullopt, std::pair{80.0, 90.0}).size(), 1u);  // 85 m/s
  EXPECT_TRUE(load_tracks(1767225600.0, 1767225700.0, std::nullopt, std::pair{100.0, 150.0}).empty());
  {
    const auto st = ensure_index("2026-01-01");
    EXPECT_EQ(st.aircraft, 1u);
    EXPECT_EQ(st.chunks, 1u);
    // A stale or damaged sidecar is rebuilt from the store.
    atomic_write(index_path("2026-01-01"), "GLADXI01 junk");
    EXPECT_EQ(load_tracks(1767225600.0, 1767225700.0).size(), 1u);
    EXPECT_EQ(ensure_index("2026-01-01").chunks, 1u);
  }
  // Coverage: the Cessna's 4 fixes span one minute and one 0.5 deg cell (40.0 N, 100.0 W).
  auto cov = parse_json(coverage_json());
  ASSERT_EQ(cov.at("days").as_array().size(), 1u);
  auto& cells = cov.at("cells").as_array();
  ASSERT_EQ(cells.size(), 1u);
  EXPECT_DOUBLE_EQ(as_number(cells[0].as_array()[0]), 40.0);
  EXPECT_DOUBLE_EQ(as_number(cells[0].as_array()[1]), -100.0);
  EXPECT_DOUBLE_EQ(as_number(cells[0].as_array()[2]), 1.0);
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

TEST(AircraftSearch, PathBoundsHoldTheWholeCurve) {
  // Erratic nodes (turns, climbs, velocities disagreeing with positions, up to 100 s
  // apart): every point and speed of the Hermite curve, sampled densely, lies within.
  std::mt19937_64 rng(11);
  std::uniform_real_distribution<double> u(-1, 1);
  for (int trial = 0; trial < 200; ++trial) {
    const double lat0 = 60 * u(rng), lon0 = 170 * u(rng);
    const Enu f(lat0, lon0, 0.0);
    std::vector<PathPoint> pts;
    Vec3 p{0, 0, 1000 + 500 * u(rng)};
    double t = 0;
    for (int i = 0; i < 12; ++i) {
      const Vec3 v{150 * u(rng), 150 * u(rng), 10 * u(rng)};
      pts.push_back({t, f.unrot(p) + f.o, f.unrot(v)});
      const double h = 2 + 98 * std::abs(u(rng));
      p = p + Vec3{v.x * h + 3000 * u(rng), v.y * h + 3000 * u(rng), 100 * u(rng)};
      t += h;
    }
    const Path path(std::move(pts));
    const PathBounds b = path_bounds(path);
    for (double x = path.t0(); x <= path.t1(); x += 0.05) {
      Vec3 q, v;
      path.state(x, q, v);
      const Geodetic g = geodetic(q);
      ASSERT_TRUE(b.box.contains(g.lat, g.lon)) << trial << " t " << x;
      ASSERT_GE(v.norm(), b.vlo) << trial << " t " << x;
      ASSERT_LE(v.norm(), b.vhi) << trial << " t " << x;
    }
  }
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
  // Types and tones as the blade-pass table has them: A a PC-12 (4 blades at 1,575 rpm),
  // B a Cessna 172 (2 blades at 2,550 rpm); the decoys are types that cannot make the
  // tone of the event they mimic (a C152 cannot sing at 105 Hz, a PC-12 at 85 Hz).
  std::vector<Track> tracks = {track_of(A, 0xa00010, "PC12", start - 400, start + 1600), track_of(B, 0xa00011, "C172", start - 400, start + 1600),
                               track_of(C, 0xa00012, "C152", start - 400, start + 1600), track_of(D, 0xa00013, "PC12", start - 400, start + 1600)};
  const Vec3 o = ecef(lat0, lon0, 2.0);
  auto x = render({{ecef_path(tracks[0]), {105.0, 210.0}}, {ecef_path(tracks[1]), {85.0}}}, o, start, 1200, 0.5, 1.0, 3);
  auto spec = whitened_spectrogram(x);
  auto det = detect_events(spec);
  ASSERT_GE(det.events.size(), 2u);
  Terrain terrain(false);
  SearchOptions so;
  so.start_min = start - 1800, so.start_max = start + 1800;
  so.box = {lon0 - 0.4, lat0 - 0.3, lon0 + 0.4, lat0 + 0.3};
  const BladePassTable table = BladePassTable::builtin();  // not the cache's learned file
  so.blade_pass = &table;
  so.type_prior = false;
  auto r0 = search(spec, det.events, split_tracks(tracks), terrain, so);
  so.type_prior = true;
  auto r = search(spec, det.events, split_tracks(tracks), terrain, so);
  std::printf("without the type prior:%s", render_result(r0, det.events).c_str());
  std::printf("with the type prior:%s", render_result(r, det.events).c_str());
  // The prior drops each decoy for the event it mimics, and the answer is unchanged.
  EXPECT_EQ(r0.type_pairs, 0u);
  EXPECT_GE(r.type_pruned_pairs, 2u);
  EXPECT_LT(r.corridor_cells, r0.corridor_cells);
  ASSERT_FALSE(r0.candidates.empty());
  ASSERT_FALSE(r.candidates.empty());
  EXPECT_EQ(r0.candidates[0].combo, r.candidates[0].combo);
  const auto& c = r.candidates[0];
  EXPECT_LT(haversine_m(c.lat, c.lon, lat0, lon0), 150.0) << c.lat << ", " << c.lon;
  EXPECT_NEAR(c.start_utc, start, 0.5);
  ASSERT_EQ(c.matches.size(), 2u);
  EXPECT_EQ(c.matches[0].flight, "a00010");
  EXPECT_EQ(c.matches[1].flight, "a00011");
  EXPECT_GE(r.level, AirLevel::Moderate);
  // Pair-first and the segment bounds rasterize fewer cells and keep exactly the same
  // survivors and answer.
  EXPECT_GT(r.pair_pieces, r.pair_kept);
  {
    SearchOptions all = so;
    all.pair_first = false, all.segment_bounds = false;
    auto r0 = search(spec, det.events, split_tracks(tracks), terrain, all);
    EXPECT_EQ(r0.pair_pieces, 0u);
    EXPECT_LT(r.corridor_cells, r0.corridor_cells);
    EXPECT_EQ(r.consistent_cells, r0.consistent_cells);
    EXPECT_EQ(r.silent_cells, r0.silent_cells);
    EXPECT_EQ(r.scored, r0.scored);
    ASSERT_EQ(r.candidates.size(), r0.candidates.size());
    for (std::size_t i = 0; i < r.candidates.size(); ++i) {
      EXPECT_EQ(r.candidates[i].combo, r0.candidates[i].combo);
      EXPECT_DOUBLE_EQ(r.candidates[i].lat, r0.candidates[i].lat);
      EXPECT_DOUBLE_EQ(r.candidates[i].lon, r0.candidates[i].lon);
      EXPECT_DOUBLE_EQ(r.candidates[i].start_utc, r0.candidates[i].start_utc);
      EXPECT_DOUBLE_EQ(r.candidates[i].score, r0.candidates[i].score);
    }
  }
  // The same with the start known to a few seconds and one flyover allowed.
  so.start_min = start - 3, so.start_max = start + 3, so.min_events = 1;
  auto r1 = search(spec, det.events, split_tracks(tracks), terrain, so);
  ASSERT_FALSE(r1.candidates.empty());
  EXPECT_LT(haversine_m(r1.candidates[0].lat, r1.candidates[0].lon, lat0, lon0), 150.0);
}

TEST(AircraftSearch, EditedRecordingClipsShareOnePlace) {
  // The recording of FindsTheListenerFromTwoFlyovers, edited: its second half first, then
  // its first half. Each clip holds one flyover and has its own clock (known to +-3 s, as
  // from each clip's own hum match); the place is shared. One flyover alone leaves a strip
  // on both sides of its track: the other clip decides.
  const double lat0 = 40.0, lon0 = -100.0, start = 1767261600.0;
  const Enu f(lat0, lon0, 0.0);
  const double tA = start + 300, tB = start + 900;
  Straight A{f, {0, 1800, 1200}, {120, 0, 0}, tA - std::hypot(1800.0, 1200.0 - 2.0) / SOUND_SPEED};
  Straight B{f, {900, 0, 800}, {0, -90, 0}, tB - std::hypot(900.0, 800.0 - 2.0) / SOUND_SPEED};
  Straight C{f, {-5000, 25000, 1200}, {120, 0, 0}, tA - 6};
  Straight D{f, {-15000, 8000, 800}, {0, -90, 0}, tB + 40};
  std::vector<Track> tracks = {track_of(A, 0xa00010, "C172", start - 400, start + 1600), track_of(B, 0xa00011, "C172", start - 400, start + 1600),
                               track_of(C, 0xa00012, "C172", start - 400, start + 1600), track_of(D, 0xa00013, "C172", start - 400, start + 1600)};
  const Vec3 o = ecef(lat0, lon0, 2.0);
  auto x = render({{ecef_path(tracks[0]), {105.0, 210.0}}, {ecef_path(tracks[1]), {92.0}}}, o, start, 1200, 0.5, 1.0, 3);
  const std::size_t half = 600 * SPEC_RATE;
  std::vector<double> edited(x.begin() + std::ptrdiff_t(half), x.end());
  edited.insert(edited.end(), x.begin(), x.begin() + std::ptrdiff_t(half));
  // Per clip, as run_aircraft_clips does: its own spectrogram and flyovers.
  const double clip_start[2] = {start + 600, start};  // clip 1 is the original 600-1200 s
  std::vector<Spectrogram> specs;
  for (int i = 0; i < 2; ++i) specs.push_back(whitened_spectrogram(std::span<const double>(edited.data() + std::size_t(i) * half, half)));
  std::vector<ClipInput> in(2);
  const Bbox box{lon0 - 0.4, lat0 - 0.3, lon0 + 0.4, lat0 + 0.3};
  for (int i = 0; i < 2; ++i) {
    in[std::size_t(i)].spec = &specs[std::size_t(i)];
    in[std::size_t(i)].events = detect_events(specs[std::size_t(i)]).events;
    ASSERT_GE(in[std::size_t(i)].events.size(), 1u) << i;
    // The strongest flyover of each clip is its own pass, CPA 300 s into the clip.
    auto& ev = in[std::size_t(i)].events;
    auto strongest = *std::max_element(ev.begin(), ev.end(), [](auto& a, auto& b) { return a.score < b.score; });
    EXPECT_NEAR(strongest.tc, 300.0, 3.0) << i;
    in[std::size_t(i)].hyps.push_back({"clip hum", std::nullopt, clip_start[i] - 3, clip_start[i] + 3, 1, box});
  }
  Terrain terrain(false);
  auto j = search_clips(in, [&](const Bbox&, double, double) { return split_tracks(tracks); }, terrain);
  ASSERT_TRUE(j.shared);
  EXPECT_LT(haversine_m(j.shared->lat, j.shared->lon, lat0, lon0), 150.0) << j.shared->lat << ", " << j.shared->lon;
  EXPECT_EQ(j.shared->clips_matched, 2);
  EXPECT_FALSE(j.shared->disagree);
  EXPECT_GE(j.shared->level, AirLevel::Moderate);
  const char* flight[2] = {"a00011", "a00010"};
  for (int i = 0; i < 2; ++i) {
    const auto& oc = j.clips[std::size_t(i)];
    ASSERT_TRUE(oc.at_place) << i;
    EXPECT_NEAR(oc.start_utc, clip_start[i], 0.5) << i;
    ASSERT_EQ(oc.at_place->candidates[0].matches.size(), 1u) << i;
    EXPECT_EQ(oc.at_place->candidates[0].matches[0].flight, flight[i]) << i;
  }
  // As one stage (the report's `aircraft`): flyovers in file time, per-clip clocks.
  seed_tables();
  std::vector<ClipSpan> spans = {{0, 0.0, 600.0, {}}, {1, 600.0, 1200.0, {}}};
  const auto st = clips_stage(spans, in, j);
  const auto js = parse_json(json::serialize(stage_json(st)));
  ASSERT_EQ(js.at("clips").as_array().size(), 2u);
  for (int i = 0; i < 2; ++i) {
    const auto& c = js.at("clips").as_array()[std::size_t(i)];
    EXPECT_NEAR(as_number(c.at("start_epoch")), clip_start[i], 0.5);
    EXPECT_EQ(c.at("matches").as_array()[0].at("flight").as_string(), flight[i]);
  }
  EXPECT_TRUE(js.at("best").at("clips").as_bool());
  ASSERT_EQ(js.at("flights").as_array().size(), 2u);
  for (auto& fl : js.at("flights").as_array()) {
    const int clip = int(as_number(fl.at("clip")));
    const auto& e = js.at("events").as_array()[std::size_t(as_number(fl.at("event")))];
    EXPECT_EQ(int(as_number(e.at("clip"))), clip);
    EXPECT_NEAR(as_number(e.at("tc_s")), 300.0 + 600.0 * clip, 3.0);  // file time
    EXPECT_EQ(fl.at("flight").as_string(), flight[clip]);
  }
  for (auto& sp : js.at("spectra").as_array())
    if (sp.at("matched").as_bool()) {
      // UTC at file time t: the clip's start + (t - the clip's offset in the file).
      const int clip = int(as_number(sp.at("clip")));
      EXPECT_NEAR(as_number(sp.at("utc0")) + 600.0 * clip, clip_start[clip], 0.5);
    }
}
TEST(AircraftBladePass, TypeTableAndHarmonics) {
  const BladePassTable t = BladePassTable::builtin();
  // PC-12: 4 blades at 1,700 rpm = 113.3 Hz (the reference flyover measured 113.8 Hz),
  // and the detector locking onto half, twice or three times that.
  EXPECT_TRUE(t.compatible("PC12", {113.8}));
  EXPECT_TRUE(t.compatible("pc12", {56.9}));
  EXPECT_TRUE(t.compatible("PC12", {227.6}));
  EXPECT_TRUE(t.compatible("PC12", {341.4}));
  EXPECT_FALSE(t.compatible("PC12", {80.0}));
  // A weaker line at the fundamental is enough.
  EXPECT_TRUE(t.compatible("PC12", {80.0, 113.0}));
  // King Air 350 with the five-blade STC propeller at 1,562 rpm (the reference's 130.2 Hz).
  EXPECT_TRUE(t.compatible("B350", {130.2}));
  // Cessna 152: 2 blades and 4 cylinders, at most 2,750 rpm (92 Hz).
  EXPECT_TRUE(t.compatible("C152", {80.0}));
  EXPECT_FALSE(t.compatible("C152", {105.0, 210.0}));
  // Unknown or empty types, and types left out on purpose (helicopters), never constrain.
  EXPECT_TRUE(t.compatible("ZZZZ", {105.0}));
  EXPECT_TRUE(t.compatible("", {105.0}));
  EXPECT_TRUE(t.compatible("R44", {300.0}));
  EXPECT_FALSE(t.known("R44"));
  // Every seeded range is a plausible propeller tone.
  for (auto& [type, p] : t.types())
    for (auto& s : p.sources) {
      EXPECT_GT(s.f_lo(), 40.0) << type;
      EXPECT_LT(s.f_hi(), 250.0) << type;
      EXPECT_LT(s.rpm_lo, s.rpm_hi) << type;
    }
}

TEST(AircraftBladePass, LearnsFromConfirmedMatches) {
  BladePassTable t = BladePassTable::builtin();
  // A type absent from the table constrains once it has a few observations.
  t.learn("XYZ1", 150.0);
  t.learn("XYZ1", 151.0);
  EXPECT_TRUE(t.compatible("XYZ1", {100.0}));
  t.learn("XYZ1", 149.0);
  EXPECT_TRUE(t.compatible("XYZ1", {150.5}));
  EXPECT_TRUE(t.compatible("XYZ1", {75.0}));  // half
  EXPECT_FALSE(t.compatible("XYZ1", {100.0}));
  // An observation outside a seeded type's book range widens it.
  EXPECT_FALSE(t.compatible("C152", {105.0}));
  t.learn("C152", 105.0);
  EXPECT_TRUE(t.compatible("C152", {105.0}));
  // Round trip through the learned file's JSON.
  BladePassTable u = BladePassTable::builtin();
  u.merge_learned(parse_json(dump(t.learned_json())));
  EXPECT_TRUE(u.compatible("C152", {105.0}));
  EXPECT_FALSE(u.compatible("XYZ1", {100.0}));
}

TEST(AircraftBladePass, SameAircraftTonesAgree) {
  EXPECT_TRUE(f0_consistent({113.8}, {114.9}));
  EXPECT_TRUE(f0_consistent({113.8}, {227.0}));        // harmonic
  EXPECT_TRUE(f0_consistent({113.8, 227.6}, {75.9}));  // a third of its harmonic
  EXPECT_FALSE(f0_consistent({113.8}, {130.2}));       // the reference's two aircraft
  EXPECT_TRUE(f0_consistent({}, {130.2}));
}

// A drawn area: "LON,LAT;..." parsed, its bounding box, and the even-odd rule on a concave loop.
TEST(Aircraft, AreaPolygon) {
  const Area a = parse_area("0,0;10,0;10,10;5,4;0,10");  // a "V" notch cut from the top
  ASSERT_EQ(a.size(), 5u);
  const Bbox b = area_box(a);
  EXPECT_DOUBLE_EQ(b.west, 0);
  EXPECT_DOUBLE_EQ(b.north, 10);
  EXPECT_TRUE(area_contains(a, 2, 5));    // lat, lon: below the notch
  EXPECT_FALSE(area_contains(a, 8, 5));   // inside the notch
  EXPECT_TRUE(area_contains(a, 8, 1));
  EXPECT_FALSE(area_contains(a, 5, 11));
  EXPECT_THROW(parse_area("0,0;1,1"), std::invalid_argument);
  EXPECT_THROW(parse_area("0,0;1,1;2,x"), std::invalid_argument);
  EXPECT_THROW(parse_area("0,0;1,1;200,2"), std::invalid_argument);
}

// One flyover with a poor straight-pass fit (speech in a council chamber matched to a
// helicopter 664 km away at fit 0.61) stays weak however far it leads; a clean one is moderate.
TEST(AircraftJudge, SingleFlyoverNeedsAGoodFitForModerate) {
  auto one = [](double fit) {
    SearchResult r;
    Candidate c;
    c.score = 7;
    Match m;
    m.fit = fit;
    c.matches.push_back(m);
    r.candidates.push_back(c);
    judge(r);
    return r.level;
  };
  EXPECT_EQ(one(0.61), AirLevel::Weak);
  EXPECT_EQ(one(0.95), AirLevel::Moderate);
}

// ---------------------------------------------------------------- OpenSky (no network)

TEST(AircraftOpenSky, WindowsAndQueriesFollowTheRules) {
  // 20:50 .. 22:03:20 UTC on 2026-09-08 over a small box: one query per hour partition,
  // the window snapped to 10 minutes inside it, the box to 0.1 degrees.
  const auto ws = opensky::windows(1788900600, 1788905000, Bbox{-73.55, 40.83, -73.47, 40.91});
  ASSERT_EQ(ws.size(), 3u);
  EXPECT_EQ(ws[0].hour, 1788897600);
  EXPECT_EQ(ws[0].t0, 1788900600);
  EXPECT_EQ(ws[0].t1, 1788901199);
  EXPECT_EQ(ws[1].hour, 1788901200);
  EXPECT_EQ(ws[1].t0, 1788901200);
  EXPECT_EQ(ws[1].t1, 1788904799);
  EXPECT_EQ(ws[2].hour, 1788904800);
  EXPECT_EQ(ws[2].t1, 1788905400);
  EXPECT_NEAR(ws[0].box.west, -73.6, 1e-9);
  EXPECT_NEAR(ws[0].box.east, -73.4, 1e-9);
  EXPECT_NEAR(ws[0].box.south, 40.8, 1e-9);
  EXPECT_NEAR(ws[0].box.north, 41.0, 1e-9);
  const std::string q = opensky::sql(ws[0]);
  EXPECT_NE(q.find("FROM state_vectors_data4"), std::string::npos);
  EXPECT_NE(q.find("WHERE hour = 1788897600"), std::string::npos) << q;
  EXPECT_NE(q.find("time BETWEEN 1788900600 AND 1788901199"), std::string::npos) << q;
  EXPECT_NE(q.find("lat BETWEEN 40.8 AND 41.0"), std::string::npos) << q;
  EXPECT_NE(q.find("lon BETWEEN -73.6 AND -73.4"), std::string::npos) << q;
  EXPECT_NE(q.find("time - lastcontact <= 15"), std::string::npos);
  EXPECT_NE(q.find("LIMIT"), std::string::npos);
  EXPECT_EQ(q.find('*'), std::string::npos) << "never every column";
  EXPECT_EQ(q.find("serials"), std::string::npos);
  // One key per question, the same for the same question.
  EXPECT_EQ(opensky::cache_key(ws[0]).size(), 32u);
  EXPECT_EQ(opensky::cache_key(ws[0]), opensky::cache_key(opensky::windows(1788900600, 1788901100, Bbox{-73.55, 40.83, -73.47, 40.91})[0]));
  EXPECT_NE(opensky::cache_key(ws[0]), opensky::cache_key(ws[1]));
  // A whole day: 24 hours of twelve 15-degree bands; a split box keeps its exact edges.
  const auto day = opensky::day_windows("2026-09-08");
  ASSERT_EQ(day.size(), 24u * 12u);
  EXPECT_EQ(day.front().hour, 1788825600);
  EXPECT_EQ(day.front().t1, 1788825600 + 3599);
  EXPECT_DOUBLE_EQ(day.front().box.south, -90);
  EXPECT_DOUBLE_EQ(day[11].box.north, 90);
  opensky::Window split = day[0];
  split.box = {-180, 11.25, -157.5, 15};
  EXPECT_NE(opensky::sql(split, 1000).find("lat BETWEEN 11.25 AND 15.0"), std::string::npos) << opensky::sql(split, 1000);
  EXPECT_NE(opensky::sql(split, 1000).find("LIMIT 1000"), std::string::npos);
  EXPECT_EQ(opensky::chunk_name(split), "00_11.25_-180.0_15.0_-157.5.trk");
  // A window inside one hour is one query.
  EXPECT_EQ(opensky::windows(1788901300, 1788902000, Bbox{8.5, 50.0, 8.6, 50.1}).size(), 1u);
}

TEST(AircraftOpenSky, TokenUserIsTheTokens) {
  EXPECT_EQ(opensky::token_user("eyJhbGciOiJub25lIn0.eyJwcmVmZXJyZWRfdXNlcm5hbWUiOiJyZXNlYXJjaGVyIiwiZXhwIjoxfQ.sig"), "researcher");
  EXPECT_EQ(opensky::token_user("not a token"), "");
  EXPECT_EQ(opensky::token_user("a.b.c"), "");
}

TEST(AircraftOpenSky, RowsBecomeTracksWithCorrectedAltitudes) {
  seed_tables();  // registry: A00001 C172, A00002 E55P (a jet), A00003 PC12
  const std::vector<std::string> cols = {"time", "icao24", "lat", "lon", "velocity", "heading", "vertrate", "baroaltitude", "geoaltitude"};
  auto rows = parse_json(R"([
    [1000, "a00001", 40.00, -100.00, 50.0, 90.0, 0.0, 600.0, 650.0],
    [1005, "a00001", 40.00, -99.997, 50.0, 90.0, 0.0, 600.0, 650.0],
    [1005, "a00001", 40.00, -99.997, 50.0, 90.0, 0.0, 600.0, 650.0],
    [1010, "a00001", 40.00, -99.994, 50.0, 90.0, 0.0, 610.0, 660.0],
    [1015, "a00001", 40.00, -99.991, 50.0, 90.0, 0.0, 610.0, 660.0],
    [1020, "a00001", 40.00, -99.988, 50.0, 90.0, null, 620.0, null],
    [1000, "a00003", 40.10, -100.00, 80.0, 0.0, 1.0, 900.0, null],
    [1005, "a00003", 40.1036, -100.00, 80.0, 0.0, 1.0, 905.0, null],
    [1010, "a00003", null, -100.00, 80.0, 0.0, 1.0, 910.0, null],
    [1000, "a00002", 40.20, -100.00, 90.0, 0.0, 0.0, 800.0, 850.0],
    [1000, "zzzzzz", 40.30, -100.00, 90.0, 0.0, 0.0, 800.0, 850.0],
    [1000, "a00004", 40.40, -100.00, 90.0, 0.0, 0.0, null, null]
  ])").as_array();
  auto tr = opensky::tracks_from_rows(cols, rows);
  ASSERT_EQ(tr.size(), 2u);  // the jet, the bad address and the altitude-less fix gone
  const Track& a = tr[0].icao == 0xa00001 ? tr[0] : tr[1];
  const Track& c = tr[0].icao == 0xa00001 ? tr[1] : tr[0];
  EXPECT_EQ(a.type, "C172");
  EXPECT_EQ(a.info.engine, EngineType::Piston);
  ASSERT_EQ(a.fixes.size(), 5u);  // the repeated second once
  EXPECT_FLOAT_EQ(a.fixes[0].alt_m, 650.0f);
  EXPECT_EQ(a.fixes[0].alt_src, AltSource::Geometric);
  EXPECT_FLOAT_EQ(a.fixes[4].alt_m, 670.0f);  // barometric + its own median offset (50 m)
  EXPECT_EQ(a.fixes[4].alt_src, AltSource::BaroTrack);
  EXPECT_TRUE(std::isnan(a.fixes[4].vrate));
  EXPECT_FLOAT_EQ(a.fixes[0].gs, 50.0f);
  EXPECT_FLOAT_EQ(a.fixes[0].track, 90.0f);
  EXPECT_EQ(c.type, "PC12");
  ASSERT_EQ(c.fixes.size(), 2u);  // the fix without a position dropped
  EXPECT_FLOAT_EQ(c.fixes[0].alt_m, 950.0f);  // + the median offset of every aircraft here
  EXPECT_EQ(c.fixes[0].alt_src, AltSource::BaroRegion);
  EXPECT_EQ(opensky::tracks_from_rows(cols, rows, true).size(), 3u);  // jets on request
  EXPECT_THROW(opensky::tracks_from_rows({"time", "lat"}, rows), std::runtime_error);
}

TEST(AircraftOpenSky, StateVectorsBecomeTheStoresNodes) {
  // A steady leg at one vector per 5 s: a node every ~100 s, on the same curve.
  const Enu f(40.0, -100.0, 0.0);
  Straight L{f, {0, 0, 900}, {70, 20, 0}, 1000};
  const Track raw = track_of(L, 0xa00001, "C172", 1000, 1600, 5.0);
  const auto nodes = opensky::to_nodes({raw});
  ASSERT_EQ(nodes.size(), 1u);
  EXPECT_LT(nodes[0].fixes.size(), 12u);
  EXPECT_GE(nodes[0].fixes.size(), 2u);
  EXPECT_EQ(nodes[0].icao, 0xa00001u);
  EXPECT_EQ(nodes[0].type, "C172");
  const Path p = path_of(nodes[0]);
  for (auto& x : raw.fixes) {
    if (x.t < p.t0() || x.t > p.t1()) continue;
    Vec3 q, v;
    p.state(x.t, q, v);
    EXPECT_LT((q - ecef(x.lat, x.lon, x.alt_m)).norm(), 30.0) << x.t;
  }
  EXPECT_TRUE(opensky::to_nodes({}).empty());
}

TEST(AircraftOpenSky, MergeCountsAFlightOnce) {
  const Enu f(40.0, -100.0, 0.0);
  Straight L{f, {0, 0, 900}, {70, 0, 0}, 0};
  Straight M{f, {0, 5000, 700}, {0, 60, 0}, 0};
  // ADSB.lol has aircraft 1 for [0, 600]; OpenSky has it for [0, 1200] and aircraft 2.
  const auto adsb = split_tracks({track_of(L, 1, "C172", 0, 600, 10.0)});
  const std::vector<Track> os = {track_of(L, 1, "", 0, 1200, 5.0), track_of(M, 2, "PC12", 0, 300, 5.0)};
  const auto m = opensky::merge(adsb, os);
  EXPECT_EQ(m.from_adsb, 1u);
  EXPECT_EQ(m.only_opensky, (std::set<std::uint32_t>{2}));
  EXPECT_EQ(m.gap_filled, (std::set<std::uint32_t>{1}));
  ASSERT_EQ(m.segments.size(), 3u);
  for (std::size_t i = 1; i < m.segments.size(); ++i) {
    const Track& s = m.segments[i];
    if (s.icao != 1) continue;
    EXPECT_GT(s.fixes.front().t, 600 + opensky::GAP_S) << "no overlap with ADSB.lol's own segment";
    EXPECT_EQ(s.type, "C172") << "the gap takes ADSB.lol's identity";
  }
  // OpenSky's copy of a flight ADSB.lol covers adds nothing.
  const auto same = opensky::merge(adsb, {track_of(L, 1, "C172", 0, 600, 5.0)});
  EXPECT_EQ(same.segments.size(), 1u);
  EXPECT_TRUE(same.only_opensky.empty() && same.gap_filled.empty());
}

TEST(AircraftOpenSky, AnswersComeFromTheCacheWithoutCredentials) {
  const std::string old_cache = env("GEOLOC_CACHE");
  const fs::path dir = test::make_temp_dir("opensky");
  test::use_cache(dir);
  ::unsetenv("OPENSKY_USERNAME");
  ::unsetenv("OPENSKY_PASSWORD");
  ::unsetenv("GEOLOC_AIRPLANE_BUCKET");
  const auto w = opensky::windows(1788900600, 1788901100, Bbox{8.5, 50.0, 8.6, 50.1})[0];
  EXPECT_THROW(opensky::fetch(w), opensky::NotConfigured);
  const Enu f(50.05, 8.55, 0.0);
  Straight L{f, {0, 0, 900}, {70, 0, 0}, double(w.t0)};
  const fs::path p = store::cache_dir() / "opensky" / (opensky::cache_key(w) + ".trk");
  fs::create_directories(p.parent_path());
  atomic_write(p, encode_tracks({track_of(L, 7, "C172", double(w.t0), double(w.t0) + 300, 5.0)}));
  std::string src;
  const auto got = opensky::fetch(w, &src);
  EXPECT_EQ(src, "cache");
  ASSERT_EQ(got.size(), 1u);
  EXPECT_EQ(got[0].icao, 7u);
  atomic_write(p, "");  // a window that held no aircraft
  EXPECT_TRUE(opensky::fetch(w).empty());
  if (old_cache.empty()) ::unsetenv("GEOLOC_CACHE");
  else test::use_cache(old_cache);
  std::error_code ec;
  fs::remove_all(dir, ec);
}

TEST(AircraftOpenSky, OffOrUnconfiguredLeavesTheAnswerAlone) {
  ::unsetenv("OPENSKY_USERNAME");
  ::unsetenv("OPENSKY_PASSWORD");
  ::unsetenv("GEOLOC_AIRPLANE_BUCKET");
  SearchResult r;
  Candidate c;
  c.lat = 40, c.lon = -100, c.start_utc = 1767261600, c.score = 9, c.combo = "0:a00010";
  r.candidates.push_back(c);
  judge(r);
  const auto before = r.reasons;
  Spectrogram spec;
  Terrain terrain(false);
  const TrackSource none = [](const Bbox&, double, double) { return std::vector<Track>{}; };
  SearchOptions so;
  opensky::Options o;  // off
  opensky::check(r, spec, {}, so, none, terrain, o);
  EXPECT_FALSE(r.opensky);
  o.enabled = true;
  o.source = [](const opensky::Window&) -> std::vector<Track> { throw opensky::NotConfigured("no credentials"); };
  opensky::check(r, spec, {}, so, none, terrain, o);
  ASSERT_TRUE(r.opensky);
  EXPECT_EQ(r.opensky->status, "not configured");
  EXPECT_EQ(r.reasons, before);
  ASSERT_EQ(r.candidates.size(), 1u);
  EXPECT_EQ(r.candidates[0].combo, "0:a00010");
  o.source = [](const opensky::Window&) -> std::vector<Track> { throw std::runtime_error("connection reset"); };
  opensky::check(r, spec, {}, so, none, terrain, o);
  EXPECT_EQ(r.opensky->status, "unreachable");
  EXPECT_NE(r.opensky->note.find("connection reset"), std::string::npos);
  EXPECT_EQ(r.reasons, before);
  EXPECT_EQ(r.candidates.size(), 1u);
}

TEST(AircraftOpenSky, AnUnheardAircraftOnlyOpenSkySawDropsThePlace) {
  // The scene of FindsTheListenerFromTwoFlyovers. ADSB.lol has the two heard aircraft;
  // OpenSky also has a third passing 0.9 km from the listener between the flyovers,
  // when nothing was heard, and its own copy of one heard aircraft.
  const double lat0 = 40.0, lon0 = -100.0, start = 1767261600.0;
  const Enu f(lat0, lon0, 0.0);
  const double tA = start + 300, tB = start + 900;
  Straight A{f, {0, 1800, 1200}, {120, 0, 0}, tA - std::hypot(1800.0, 1200.0 - 2.0) / SOUND_SPEED};
  Straight B{f, {900, 0, 800}, {0, -90, 0}, tB - std::hypot(900.0, 800.0 - 2.0) / SOUND_SPEED};
  Straight E{f, {800, 0, 500}, {0, 60, 0}, start + 600};
  const std::vector<Track> adsb_tracks = {track_of(A, 0xa00010, "PC12", start - 400, start + 1600), track_of(B, 0xa00011, "C172", start - 400, start + 1600)};
  const Vec3 o = ecef(lat0, lon0, 2.0);
  auto x = render({{ecef_path(adsb_tracks[0]), {105.0, 210.0}}, {ecef_path(adsb_tracks[1]), {85.0}}}, o, start, 1200, 0.5, 1.0, 3);
  auto spec = whitened_spectrogram(x);
  auto det = detect_events(spec);
  ASSERT_GE(det.events.size(), 2u);
  Terrain terrain(false);
  SearchOptions so;
  so.start_min = start - 600, so.start_max = start + 600;
  so.box = {lon0 - 0.1, lat0 - 0.08, lon0 + 0.1, lat0 + 0.08};
  const BladePassTable table = BladePassTable::builtin();
  so.blade_pass = &table;
  const TrackSource adsb = [&](const Bbox&, double, double) { return split_tracks(adsb_tracks); };
  const auto r0 = search(spec, det.events, split_tracks(adsb_tracks), terrain, so);
  ASSERT_FALSE(r0.candidates.empty());
  ASSERT_LT(haversine_m(r0.candidates[0].lat, r0.candidates[0].lon, lat0, lon0), 150.0);
  const std::string combo = r0.candidates[0].combo;

  opensky::Options oo;
  oo.enabled = true, oo.places = 1;
  int asked = 0;
  // Nothing new in OpenSky: its copy of a heard aircraft, and an aircraft passing the
  // listener 50 min in, inside the query's hour but outside the window searched (the
  // flyovers' span +- 10 min), which ADSB.lol is not read for either. The answer stands.
  Straight F{f, {500, 0, 400}, {0, 60, 0}, start + 3000};
  oo.source = [&](const opensky::Window& w) {
    ++asked;
    EXPECT_LE(w.t1 - w.t0, 3599);
    return std::vector<Track>{track_of(A, 0xa00010, "PC12", start - 400, start + 1600), track_of(F, 0xa00030, "C172", start + 2700, start + 3300)};
  };
  auto r1 = r0;
  opensky::check(r1, spec, det.events, so, adsb, terrain, oo);
  ASSERT_TRUE(r1.opensky);
  std::printf("%s", opensky::render_report(*r1.opensky).c_str());
  EXPECT_GE(asked, 1);
  EXPECT_EQ(r1.opensky->status, "checked");
  ASSERT_EQ(r1.opensky->places.size(), 1u);
  EXPECT_EQ(r1.opensky->places[0].outcome, "unchanged");
  EXPECT_EQ(r1.opensky->places[0].opensky_segments, 0u);
  EXPECT_EQ(r1.candidates[0].combo, combo);

  // The unheard aircraft: the place goes.
  oo.source = [&](const opensky::Window&) {
    return opensky::to_nodes({track_of(A, 0xa00010, "PC12", start - 400, start + 1600), track_of(E, 0xa00020, "C172", start - 400, start + 1600)});
  };
  auto r2 = r0;
  opensky::check(r2, spec, det.events, so, adsb, terrain, oo);
  ASSERT_TRUE(r2.opensky);
  std::printf("%s", opensky::render_report(*r2.opensky).c_str());
  ASSERT_EQ(r2.opensky->places.size(), 1u);
  const auto& pc = r2.opensky->places[0];
  EXPECT_EQ(pc.outcome, "dropped") << pc.detail;
  EXPECT_EQ(r2.opensky->dropped, 1);
  ASSERT_FALSE(pc.aircraft.empty());
  EXPECT_EQ(pc.aircraft[0].flight, "a00020");
  EXPECT_TRUE(pc.aircraft[0].only_opensky);
  EXPECT_TRUE(pc.aircraft[0].unheard);
  EXPECT_NEAR(pc.aircraft[0].closest_m, std::hypot(800.0, 498.0), 60.0);
  for (auto& a : pc.aircraft) EXPECT_NE(a.flight, "a00010") << "a flight ADSB.lol has is not added again";
  for (auto& c : r2.candidates) EXPECT_NE(c.combo, combo);
  const auto j = result_json(r2, det.events);
  ASSERT_TRUE(j.contains("opensky"));
  EXPECT_EQ(j.at("opensky").at("places").as_array().at(0).at("outcome").as_string(), "dropped");
}

TEST(AircraftOpenSky, BuildsAWholeDayFromItsBandHours) {
  // Every band-hour already cached (as an interrupted run leaves them): the day is built
  // into an ordinary store without a query, an aircraft crossing an hour kept as one.
  seed_tables();
  (void)AircraftDb::get(), (void)TypeDb::get();
  const std::string old_cache = env("GEOLOC_CACHE");
  const fs::path dir = test::make_temp_dir("opensky-day");
  test::use_cache(dir);
  ::unsetenv("OPENSKY_USERNAME");
  ::unsetenv("OPENSKY_PASSWORD");
  ::unsetenv("GEOLOC_AIRPLANE_BUCKET");
  const std::string day = "2025-06-01";
  EXPECT_THROW(opensky::build_day(day), opensky::NotConfigured);
  const auto ws = opensky::day_windows(day);
  const fs::path cdir = store::cache_dir() / "opensky" / "days" / day;
  fs::create_directories(cdir);
  const double h10 = double(ws[10 * 12].hour);
  const Enu f(40.0, -100.0, 0.0);
  Straight L{f, {0, 0, 900}, {70, 0, 0}, h10 - 300};
  for (auto& w : ws) {
    std::string bytes;
    if (w.box.south <= 40 && w.box.north > 40) {
      const double a = double(w.t0), b = double(w.t1) + 1;
      if (b > h10 - 300 && a < h10 + 300) bytes = encode_tracks({track_of(L, 0xa00001, "C172", std::max(a, h10 - 300), std::min(b - 5, h10 + 300), 5.0)});
    }
    atomic_write(cdir / opensky::chunk_name(w), bytes);
  }
  const auto st = opensky::build_day(day);
  EXPECT_EQ(st.queries, 0);
  EXPECT_EQ(st.cached, int(ws.size()));
  EXPECT_EQ(st.aircraft, 1u);
  EXPECT_TRUE(has_day(day));
  EXPECT_TRUE(fs::exists(opensky::source_path(day)));
  const auto got = split_tracks(load_tracks(h10 - 600, h10 + 600));
  ASSERT_EQ(got.size(), 1u) << "one segment across the hour";
  EXPECT_LT(got[0].fixes.front().t, h10 - 200);
  EXPECT_GT(got[0].fixes.back().t, h10 + 200);
  EXPECT_NO_THROW(opensky::build_day(day)) << "a day OpenSky built is rebuilt (from the cache)";
  fs::remove(opensky::source_path(day));  // now it is as if ADSB.lol had it
  EXPECT_THROW(opensky::build_day(day), std::runtime_error) << "never over an ADSB.lol day without --force";
  if (old_cache.empty()) ::unsetenv("GEOLOC_CACHE");
  else test::use_cache(old_cache);
  std::error_code ec;
  fs::remove_all(dir, ec);
}
