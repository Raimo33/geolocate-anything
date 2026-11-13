#include <cmath>
#include <numbers>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <random>
#include <stdexcept>
#include <thread>

#include "helpers.hpp"
#include "mseed_fixture.hpp"
#include "net/offload.hpp"
#include "refs/mseed.hpp"
#include "refs/sources.hpp"
#include "refs/seismic.hpp"
#include "refs/seismic_fetch.hpp"

using namespace geo;
namespace asio = boost::asio;

static double utc_s(const char* s) { return geo::test::utc(s); }

TEST(Mseed, DecodesEveryEncodingLikeObspy) {
  auto segs = mseed::decode(mseed_fixture::BYTES);
  std::map<std::string, std::vector<const mseed::Segment*>> by;
  for (auto& s : segs) by[s.id].push_back(&s);
  ASSERT_EQ(by["XX.STM2.00.HNZ"].size(), 1u);
  ASSERT_EQ(by["XX.STM1.00.HNZ"].size(), 1u);
  ASSERT_EQ(by["XX.I32.00.HNZ"].size(), 2u);  // a 1 s gap splits it
  ASSERT_EQ(by["XX.F32.00.HNZ"].size(), 1u);
  auto same = [](const mseed::Segment& s, const auto& ref) {
    ASSERT_EQ(s.x.size(), ref.size());
    for (std::size_t i = 0; i < ref.size(); ++i) ASSERT_EQ(s.x[i], float(ref[i])) << s.id << " sample " << i;
  };
  same(*by["XX.STM2.00.HNZ"][0], mseed_fixture::STM2);
  same(*by["XX.STM1.00.HNZ"][0], mseed_fixture::STM1);
  same(*by["XX.F32.00.HNZ"][0], mseed_fixture::F32);
  auto& i32 = by["XX.I32.00.HNZ"];
  std::vector<std::int32_t> a(mseed_fixture::I32.begin(), mseed_fixture::I32.begin() + 150), b(mseed_fixture::I32.begin() + 150, mseed_fixture::I32.end());
  same(*i32[0], a);
  same(*i32[1], b);
  EXPECT_NEAR(by["XX.STM2.00.HNZ"][0]->t0, utc_s("2023-06-25 03:00:00") + 0.1234, 1e-6);
  EXPECT_NEAR(i32[1]->t0, utc_s("2023-06-25 03:00:01") + 0.75, 1e-6);
  EXPECT_DOUBLE_EQ(i32[0]->rate, 200.0);
}

namespace {
// Grid-like deviation (mHz) at time t: slow and fast wander.
double dev_mhz(double t) {
  return 25 * std::sin(2 * std::numbers::pi * t / 410) + 8 * std::sin(2 * std::numbers::pi * t / 53 + 1) + 3 * std::sin(2 * std::numbers::pi * t / 17);
}
// A segment carrying mains hum at 60 Hz (or `mains`) + dev, a large microseism and noise.
mseed::Segment hum_segment(double rate, double t0, double dur, double hum_amp, double noise, unsigned seed, double mains = 60.0) {
  std::mt19937 rng(seed);
  std::normal_distribution<double> nd;
  mseed::Segment s{"XX.SYN..HNZ", t0, rate, {}};
  const std::size_t n = std::size_t(dur * rate);
  s.x.resize(n);
  double ph = 0;
  for (std::size_t i = 0; i < n; ++i) {
    const double t = t0 + double(i) / rate;
    ph += 2 * std::numbers::pi * (mains + dev_mhz(t) * 1e-3) / rate;
    s.x[i] = float(std::round(hum_amp * std::sin(ph) + 3000 * std::sin(2 * std::numbers::pi * 0.2 * t) + noise * nd(rng)));
  }
  return s;
}
}  // namespace

// A Steim record whose last sample is not its reverse integration constant is corrupt:
// it is dropped, and the rest of the request still decodes.
TEST(Mseed, DropsARecordThatFailsSteimsIntegrityCheck) {
  auto bytes = mseed_fixture::BYTES;
  const std::size_t doff = std::size_t(bytes[44]) << 8 | bytes[45];  // the first record (STM2)
  bytes[doff + 8 + 3] ^= 0x5a;                                       // frame 0, word 2: Xn
  std::map<std::string, std::size_t> by;
  for (auto& s : mseed::decode(bytes)) by[s.id] += s.x.size();
  EXPECT_LT(by["XX.STM2.00.HNZ"], mseed_fixture::STM2.size());  // its first record is gone
  EXPECT_EQ(by["XX.STM1.00.HNZ"], mseed_fixture::STM1.size());
  EXPECT_EQ(by["XX.F32.00.HNZ"], mseed_fixture::F32.size());
}

TEST(SeismicTrack, FollowsTheHumToTheSecond) {
  const double t0 = utc_s("2023-06-25 03:00:00") - 30.123;  // starts off the second grid
  for (double rate : {200.0, 100.0}) {                     // 100: folded to 40 Hz, sign flips
    auto seg = hum_segment(rate, t0, 1260, 40, 20, 7);  // ~32 dB SNR, like a good station
    const i64 a = i64(utc_s("2023-06-25 03:00:00")), b = a + 1200;
    auto tr = seismic::track_hum({seg}, a, b);
    std::vector<float> truth(1200);
    for (int i = 0; i < 1200; ++i) truth[std::size_t(i)] = float(dev_mhz(double(a + i) + 0.5));
    // The frame smooths like the matcher does: compare at the best lag, expect lag 0.
    int best_lag = 99;
    double best_rmse = 1e9;
    for (int lag = -3; lag <= 3; ++lag) {
      double se = 0;
      int k = 0;
      for (int i = 20; i < 1180; ++i) {
        float e = tr.f_mhz[std::size_t(i)];
        if (!std::isfinite(e)) continue;
        // 12 s Hann-smoothed truth
        double s = 0, w = 0;
        for (int j = -6; j <= 5; ++j) {
          double h = std::pow(std::sin(std::numbers::pi * (j + 6.5) / 12), 2);
          s += h * truth[std::size_t(i + j + lag)];
          w += h;
        }
        se += (e - s / w) * (e - s / w);
        ++k;
      }
      ASSERT_GT(k, 1000) << rate;
      if (std::sqrt(se / k) < best_rmse) best_rmse = std::sqrt(se / k), best_lag = lag;
    }
    EXPECT_EQ(best_lag, 0) << rate;
    EXPECT_LT(best_rmse, 2.0) << rate;  // ~C/sqrt(SNR) plus the frame smoothing
    EXPECT_GT(tr.snr_db[600], 25) << rate;
  }
}

TEST(SeismicFuse, WeightsAgreeingStationsAndDropsGarbage) {
  const std::size_t n = 3600;
  std::mt19937 rng(5);
  std::normal_distribution<double> nd;
  std::vector<float> truth(n);
  for (std::size_t i = 0; i < n; ++i) truth[i] = float(dev_mhz(double(i)));
  auto make = [&](double sigma, bool garbage) {
    seismic::Track t;
    // the SNR that goes with this noise (noise = 40 mHz / sqrt(SNR)); junk claims 20 dB
    t.f_mhz.resize(n), t.snr_db.assign(n, garbage ? 20.f : float(10 * std::log10(std::pow(40.0 / sigma, 2))));
    for (std::size_t i = 0; i < n; ++i) t.f_mhz[i] = float(garbage ? 60 * nd(rng) : truth[i] + sigma * nd(rng));
    return t;
  };
  std::vector<seismic::Track> tr = {make(0.5, false), make(3.0, false), make(0, true)};
  seismic::FuseStats st;
  auto f = seismic::fuse(tr, {"good", "fair", "junk"}, &st);
  double se = 0;
  int k = 0;
  for (std::size_t i = 0; i < n; ++i)
    if (std::isfinite(f[i])) se += (f[i] - truth[i]) * (f[i] - truth[i]), ++k;
  EXPECT_GT(k, int(n * 0.95));
  EXPECT_LT(std::sqrt(se / k), 0.6);  // close to the best station, not the mean of all
  EXPECT_LT(st.used_s[2], 100);       // junk barely used
  EXPECT_GT(st.multi_s, i64(n / 2));
  // A lone noisy station is not trusted; a lone quiet one is.
  auto lone = seismic::fuse({make(9.0, false)}, {"noisy"});
  int fin = 0;
  for (float v : lone) fin += std::isfinite(v);
  EXPECT_LT(fin, int(n / 10));
  auto lone2 = seismic::fuse({make(0.8, false)}, {"quiet"});
  fin = 0;
  for (float v : lone2) fin += std::isfinite(v);
  EXPECT_GT(fin, int(n * 0.9));
}

// A 50 Hz grid: seen directly at 200 samples/s, folded to 30 Hz (sign flipped) at 80, and
// not at all at 100, where the line sits on Nyquist.
TEST(SeismicTrack, FiftyHertzMains) {
  const i64 a = i64(utc_s("2025-04-15 05:00:00")), b = a + 600;
  seismic::TrackOptions o;
  o.nominal_hz = o.mains_hz = 50;
  for (double rate : {200.0, 80.0, 100.0}) {
    auto tr = seismic::track_hum({hum_segment(rate, double(a) - 30, 660, 40, 20, 3, 50.0)}, a, b, o);
    double se = 0;
    int k = 0;
    for (int i = 20; i < 580; ++i)
      if (float e = tr.f_mhz[std::size_t(i)]; std::isfinite(e)) se += std::pow(e - dev_mhz(double(a + i) + 0.5), 2), ++k;
    if (rate == 100.0) {
      EXPECT_EQ(k, 0);
      continue;
    }
    ASSERT_GT(k, 500) << rate;
    EXPECT_LT(std::sqrt(se / k), 4.0) << rate;  // against the unsmoothed truth
  }
}

// A 50 Hz line in a 100 samples/s channel is a real beat on Nyquist: only the roll-off
// of the digitiser's filter (weaker when the grid runs fast) carries the sign.
namespace {
mseed::Segment nyquist_segment(double t0, double dur, double kappa, double noise, unsigned seed, double sign = 1) {
  std::mt19937 rng(seed);
  std::normal_distribution<double> nd;
  mseed::Segment s{"AM.SYN.00.EHZ", t0, 100.0, {}};
  const std::size_t n = std::size_t(dur * 100);
  s.x.resize(n);
  double ph = 0.7;
  for (std::size_t i = 0; i < n; ++i) {
    const double d = sign * dev_mhz(t0 + double(i) / 100);
    ph += 2 * std::numbers::pi * (50 + d * 1e-3) / 100;
    s.x[i] = float(std::round(2000 * std::exp(-kappa * d) * std::cos(ph) + 500 * std::sin(2 * std::numbers::pi * 0.2 * double(i) / 100) + noise * nd(rng)));
  }
  return s;
}
double track_r(const seismic::Track& tr, double t0, int* n_out, double sign = 1) {
  double sa = 0, sb = 0, saa = 0, sbb = 0, sab = 0;
  int n = 0;
  for (std::size_t i = 0; i < tr.f_mhz.size(); ++i) {
    if (!std::isfinite(tr.f_mhz[i])) continue;
    const double a = tr.f_mhz[i], b = sign * dev_mhz(t0 + double(i) + 0.5);
    sa += a, sb += b, saa += a * a, sbb += b * b, sab += a * b, ++n;
  }
  *n_out = n;
  return n < 10 ? 0 : (sab - sa * sb / n) / std::sqrt((saa - sa * sa / n) * (sbb - sb * sb / n));
}
}  // namespace

TEST(SeismicNyquist, RecoversTheSignedDeviation) {
  const double t0 = utc_s("2025-03-15 04:00:00");
  const double pad = double(seismic::NYQUIST_PAD_S);
  for (double sign : {1.0, -1.0}) {  // the mirrored grid must come out mirrored: the sign is measured, not assumed
    auto sg = nyquist_segment(t0 - pad - 5.0037, 1800 + 2 * pad + 10, 0.0073, 3000, 7, sign);
    auto tr = seismic::track_nyquist({sg}, std::int64_t(t0), std::int64_t(t0) + 1800);
    int n = 0;
    const double r = track_r(tr, t0, &n, sign);
    EXPECT_GT(n, 1700);
    EXPECT_GT(r, 0.9) << "sign " << sign;
  }
}

// A small grid leaves the decoder's +-80 mHz now and then. The decoder cannot follow
// the beat there and would report values near zero: those seconds must come out empty.
TEST(SeismicNyquist, SilentBeyondItsRange) {
  const double t0 = utc_s("2025-03-15 04:00:00");
  const double pad = double(seismic::NYQUIST_PAD_S);
  auto excursion = [&](double t) {  // +130 mHz for five minutes, ramps of one minute
    const double u = t - (t0 + 900);
    return u < 0 || u > 420 ? 0.0 : 130.0 * std::min({1.0, u / 60, (420 - u) / 60});
  };
  std::mt19937 rng(11);
  std::normal_distribution<double> nd;
  mseed::Segment sg{"AM.SYN.00.EHZ", t0 - pad - 5.0037, 100.0, {}};
  sg.x.resize(std::size_t((1800 + 2 * pad + 10) * 100));
  double ph = 0.7;
  for (std::size_t i = 0; i < sg.x.size(); ++i) {
    const double t = sg.t0 + double(i) / 100, d = dev_mhz(t) + excursion(t);
    ph += 2 * std::numbers::pi * (50 + d * 1e-3) / 100;
    sg.x[i] = float(std::round(2000 * std::exp(-0.0073 * d) * std::cos(ph) + 3000 * nd(rng)));
  }
  auto tr = seismic::track_nyquist({sg}, std::int64_t(t0), std::int64_t(t0) + 1800);
  int inside = 0, far = 0, kept_far = 0, wrong = 0;
  for (std::size_t i = 0; i < tr.f_mhz.size(); ++i) {
    const double truth = dev_mhz(t0 + double(i) + 0.5) + excursion(t0 + double(i) + 0.5);
    const bool kept = std::isfinite(tr.f_mhz[i]);
    if (excursion(t0 + double(i) + 0.5) > 100) ++far, kept_far += kept;
    else if (excursion(t0 + double(i) + 0.5) == 0) inside += kept;
    if (kept && std::fabs(tr.f_mhz[i] - truth) > 60) ++wrong;
  }
  EXPECT_GT(far, 250);
  EXPECT_EQ(kept_far, 0);  // nothing stored while the grid is beyond the range
  EXPECT_LT(wrong, 20);
  EXPECT_GT(inside, 1000);  // and the rest of the half hour is still there
}

TEST(SeismicNyquist, SilentWithoutALineOrWithoutTheRollOff) {
  const double t0 = utc_s("2025-03-15 04:00:00");
  const double pad = double(seismic::NYQUIST_PAD_S);
  // Noise only: no line, nothing stored.
  auto quiet = nyquist_segment(t0 - pad - 5, 1800 + 2 * pad + 10, 0.0073, 3000, 3);
  for (auto& v : quiet.x) v = float(std::remainder(v, 7.0) * 300);
  int n = 0;
  track_r(seismic::track_nyquist({quiet}, std::int64_t(t0), std::int64_t(t0) + 1800), t0, &n);
  EXPECT_EQ(n, 0);
  // A line whose amplitude does not depend on the deviation: the sign cannot be known.
  auto flat = nyquist_segment(t0 - pad - 5, 1800 + 2 * pad + 10, 0.0, 3000, 5);
  track_r(seismic::track_nyquist({flat}, std::int64_t(t0), std::int64_t(t0) + 1800), t0, &n);
  EXPECT_EQ(n, 0);
  // Other sample rates are not this tracker's business.
  auto other = nyquist_segment(t0 - pad - 5, 1800 + 2 * pad + 10, 0.0073, 3000, 5);
  other.rate = 200;
  track_r(seismic::track_nyquist({other}, std::int64_t(t0), std::int64_t(t0) + 1800), t0, &n);
  EXPECT_EQ(n, 0);
}

// Where a station may run on a generator (seismic_ips), nobody stands alone.
TEST(SeismicFuse, NeverAloneWhenTheNetworkSaysSo) {
  const std::size_t n = 1800;
  seismic::Track t;
  t.f_mhz.resize(n), t.snr_db.assign(n, 35.f);
  for (std::size_t i = 0; i < n; ++i) t.f_mhz[i] = float(dev_mhz(double(i)));
  seismic::FuseOptions o;
  o.lone_max_noise_mhz = 0;
  for (float v : seismic::fuse({t}, {"alone"}, nullptr, o)) ASSERT_FALSE(std::isfinite(v));
  int fin = 0;
  for (float v : seismic::fuse({t, t}, {"a", "b"}, nullptr, o)) fin += std::isfinite(v);
  EXPECT_GT(fin, int(n * 0.9));
}

// A fixed tone 0.65 Hz from the hum and stronger than it (seismic_brazil's recorder) sits
// among the noise probes: the mean floor takes it for noise, the median floor does not.
TEST(SeismicTrack, RobustFloorIgnoresAToneBesideTheHum) {
  const double fs = 100;
  const std::size_t n = std::size_t(fs * 300);
  mseed::Segment sg;
  sg.id = "XX.T..EHZ", sg.t0 = 1'100'000'000, sg.rate = fs, sg.x.resize(n);
  std::uint32_t lcg = 12345;
  for (std::size_t i = 0; i < n; ++i) {
    const double t = double(i) / fs;
    lcg = lcg * 1664525u + 1013904223u;
    const double noise = (double(lcg >> 8) / double(1u << 24) - 0.5) * 2.0;
    // 60.010 Hz folds to 39.990 Hz at 100 samples/s; the stray tone is at 40.65 Hz
    sg.x[i] = float(100 * std::sin(2 * std::numbers::pi * 60.010 * t) + 180 * std::sin(2 * std::numbers::pi * 40.65 * t) + noise);
  }
  seismic::TrackOptions o;
  const std::int64_t t0 = std::int64_t(sg.t0) + 20, t1 = t0 + 240;
  auto plain = seismic::track_hum({sg}, t0, t1, o);
  o.robust_floor = true;
  auto robust = seismic::track_hum({sg}, t0, t1, o);
  for (std::size_t i = 0; i < plain.f_mhz.size(); ++i) {
    ASSERT_NEAR(robust.f_mhz[i], 10.0, 0.5);
    ASSERT_EQ(robust.f_mhz[i], plain.f_mhz[i]);  // only the SNR differs
    ASSERT_LT(plain.snr_db[i], 8);
    ASSERT_GT(robust.snr_db[i], 30);
  }
}

TEST(SeismicNetworks, BrazilIsAClosedPeriodFromItsOwnArchive) {
  const auto& b = seismic::brazil();
  ASSERT_EQ(seismic::network("seismic_brazil"), &b);
  EXPECT_EQ(b.mains_hz, 60);
  EXPECT_TRUE(b.robust_floor);
  EXPECT_TRUE(std::string(b.dataselect).find("usp.br") != std::string::npos);
  const auto m = seismic::months(b);
  EXPECT_EQ(m.front(), "2004-02");
  EXPECT_EQ(m.back(), "2008-08");
  EXPECT_FALSE(seismic::east().dataselect);  // EarthScope, with its account
}

// Western Australia in 2017: a closed deployment read directly from AusPass.
TEST(SeismicNetworks, PerthBasinArrayIsAClosedPeriodFromAusPass) {
  const auto& p = seismic::perth_basin();
  ASSERT_EQ(seismic::network("seismic_perth_basin"), &p);
  EXPECT_EQ(p.mains_hz, 50);
  EXPECT_FALSE(p.nyquist);  // 250 samples/s: the line is seen directly
  EXPECT_FALSE(p.lone);     // a field station may hum at its own inverter
  EXPECT_TRUE(std::string(p.dataselect).find("auspass.edu.au") != std::string::npos);
  const auto m = seismic::months(p);
  EXPECT_EQ(m.front(), "2017-01");
  EXPECT_EQ(m.back(), "2017-06");
  for (auto& i : refs::placement_issues("seismic_perth_basin", "australia_swis", "2017-03", nullptr)) ADD_FAILURE() << i.message;
  ::unsetenv("GEOLOC_AUSPASS_REFRESH");
  ::unsetenv("GEOLOC_SHAKE_REFRESH");
  EXPECT_TRUE(refs::refresh_plan("australia_swis").empty());  // not from the dashboard
}

// IPS/UPS from Estonia: a closed period that ends when the Baltic states left the grid.
TEST(SeismicNetworks, EstoniaIsIpsUntilTheBalticsLeft) {
  const auto& e = seismic::estonia();
  ASSERT_EQ(seismic::network("seismic_estonia"), &e);
  EXPECT_EQ(e.mains_hz, 50);
  EXPECT_FALSE(e.nyquist);  // 250 samples/s: the line is seen directly
  EXPECT_FALSE(e.lone);
  EXPECT_TRUE(std::string(e.dataselect).find("geofon.gfz.de") != std::string::npos);
  const auto m = seismic::months(e);
  EXPECT_EQ(m.front(), "2015-05");
  EXPECT_EQ(m.back(), "2025-02");  // stations still record; the grid moved on 2025-02-08
  for (auto& i : refs::placement_issues("seismic_estonia", "ips_ups", "2019-04", nullptr)) ADD_FAILURE() << i.message;
  ::unsetenv("GEOLOC_GEOFON_REFRESH");
  for (auto& [src, months] : refs::refresh_plan("ips_ups")) EXPECT_NE(src, "seismic_estonia");  // not from the dashboard
}

TEST(SeismicNetworks, EastAndIps) {
  ASSERT_TRUE(seismic::network("seismic_east") && seismic::network("seismic_ips"));
  EXPECT_FALSE(seismic::network("osf_edr"));
  EXPECT_EQ(seismic::ips().mains_hz, 50);
  EXPECT_EQ(seismic::months(seismic::ips()).front(), "2022-10");
  EXPECT_LT(seismic::months(seismic::east()).front(), "2010");
  for (auto& s : seismic::ips().stations()) EXPECT_FALSE(s.score.empty()) << s.id;
}

// The Western reconstruction leaves the months AESO measured to AESO; ERCOT believes a
// lone station only while there is one site.
TEST(SeismicNetworks, WestAndErcot) {
  const auto& w = seismic::west();
  const auto& e = seismic::ercot();
  ASSERT_EQ(seismic::network("seismic_west"), &w);
  ASSERT_EQ(seismic::network("seismic_ercot"), &e);
  EXPECT_EQ(w.mains_hz, 60);
  EXPECT_EQ(e.mains_hz, 60);
  const auto wm = seismic::months(w);
  ASSERT_FALSE(wm.empty());
  EXPECT_EQ(wm.front(), "2001-09");
  EXPECT_FALSE(w.lone);
  for (auto& m : wm) EXPECT_TRUE(m < "2019-01" || m > "2025-07") << m;
  EXPECT_TRUE(std::find(wm.begin(), wm.end(), "2018-12") != wm.end());
  EXPECT_TRUE(std::find(wm.begin(), wm.end(), "2025-08") != wm.end());
  const auto em = seismic::months(e);
  ASSERT_FALSE(em.empty());
  EXPECT_EQ(em.front(), "2009-06");
  EXPECT_TRUE(std::find(em.begin(), em.end(), "2021-02") != em.end());
  // Arrays and re-named stations share a building under different codes: one site.
  EXPECT_FLOAT_EQ(w.same_site_km, 0.5f);
  EXPECT_FLOAT_EQ(e.same_site_km, 0.5f);
  EXPECT_EQ(seismic::east().same_site_km, 0);
  ASSERT_TRUE(e.lone && e.lone_before);
  EXPECT_STREQ(e.lone_before, "2015-02");
  for (auto* n : {&w, &e})
    for (auto& s : n->stations()) {
      EXPECT_FALSE(s.score.empty()) << s.id;
      float best = 0;
      for (auto [y, r] : s.score) best = std::max(best, r);
      EXPECT_GE(best, 0.97f) << s.id;
    }
}

TEST(SeismicNetworks, ChinaFromRaspberryShakes) {
  for (const char* src : {"seismic_cn_south", "seismic_cn_north", "seismic_cn_east", "seismic_cn_southwest"}) {
    const seismic::Network* n = seismic::network(src);
    ASSERT_TRUE(n) << src;
    EXPECT_TRUE(n->nyquist) << src;
    EXPECT_EQ(n->mains_hz, 50);
    ASSERT_FALSE(n->stations().empty()) << src;
    for (auto& s : n->stations()) {
      EXPECT_EQ(std::string(s.id).rfind("AM.", 0), 0u) << s.id;  // the Raspberry Shake network
      EXPECT_FALSE(s.score.empty()) << s.id;
    }
  }
  EXPECT_FALSE(seismic::east().nyquist);
}

// Australia outside the NEM: Perth's Shakes (several of one owner at one rounded
// position, which must not confirm each other) and the one in Darwin.
TEST(SeismicNetworks, AustraliaFromRaspberryShakes) {
  for (const char* src : {"seismic_swis", "seismic_darwin"}) {
    const seismic::Network* n = seismic::network(src);
    ASSERT_TRUE(n) << src;
    EXPECT_TRUE(n->nyquist) << src;
    EXPECT_EQ(n->mains_hz, 50);
    ASSERT_FALSE(n->stations().empty()) << src;
    for (auto& s : n->stations()) {
      EXPECT_EQ(std::string(s.id).rfind("AM.", 0), 0u) << s.id;
      EXPECT_FALSE(s.score.empty()) << s.id;
      EXPECT_EQ(s.lat < -25, std::string(src) == "seismic_swis") << s.id;  // Perth is at 32 S, Darwin at 12 S
    }
  }
  EXPECT_GT(seismic::swis().same_site_km, 0);
  EXPECT_GT(seismic::darwin().same_site_km, 0);  // its two current units share an address
  // AM.RBA56 stood in Darwin, then Perth, then Darwin again: each list holds its own spells.
  auto spells = [](const seismic::Network& n) {
    std::vector<std::pair<std::string, std::string>> v;
    for (auto& s : n.stations())
      if (std::string(s.id) == "AM.RBA56.00.EHZ") v.emplace_back(s.from, s.to);
    return v;
  };
  // Every listed station lies inside the grid it is a reference for.
  for (auto [src, grid] : {std::pair{"seismic_swis", "australia_swis"}, std::pair{"seismic_darwin", "darwin_katherine"}})
    for (auto& i : refs::placement_issues(src, grid, "2026-06", nullptr)) ADD_FAILURE() << i.message;
  auto perth = spells(seismic::swis()), darwin = spells(seismic::darwin());
  ASSERT_EQ(perth.size(), 1u);
  ASSERT_EQ(darwin.size(), 2u);
  for (auto& [from, to] : darwin) EXPECT_TRUE(to <= perth[0].first || from >= perth[0].second) << from;
}

// The Nyquist decoder's chunks run on threads of their own while the loop goes on.
TEST(Offload, RunsInParallelAndRethrows) {
  std::atomic<int> live{0}, peak{0};
  auto job = [&](int k) {
    const int now = ++live;
    for (int p = peak; now > p && !peak.compare_exchange_weak(p, now);) {
    }
    for (auto until = std::chrono::steady_clock::now() + std::chrono::seconds(5); peak < 3 && std::chrono::steady_clock::now() < until;)
      std::this_thread::yield();
    --live;
    if (k == 2) throw std::runtime_error("chunk 2");
    return k * 10;
  };
  std::vector<int> got(3, -1);
  std::string err;
  net::run_sync_void([&](asio::io_context&) -> asio::awaitable<void> {
    std::vector<int> ks{0, 1, 2};
    co_await net::parallel_for(ks, 3, [&](int k) -> asio::awaitable<void> {
      try {
        got[std::size_t(k)] = co_await net::run_in_thread([&job, k] { return job(k); });
      } catch (const std::exception& e) {
        err = e.what();
      }
    });
  });
  EXPECT_EQ(peak, 3);  // all three at once, not one after another
  EXPECT_EQ(got[0], 0);
  EXPECT_EQ(got[1], 10);
  EXPECT_EQ(got[2], -1);
  EXPECT_EQ(err, "chunk 2");
}

// The dashboard never backfills Raspberry Shake sources (minutes of CPU per station-day).
TEST(SeismicNetworks, ShakeSourcesOnlyRefreshedWhenAsked) {
  ::unsetenv("GEOLOC_SHAKE_REFRESH");
  for (const char* grid : {"china_southern", "china_north_central", "china_east", "china_southwest", "australia_swis", "darwin_katherine"})
    EXPECT_TRUE(refs::refresh_plan(grid).empty()) << grid;
}

// A mixed network (seismic_ips_shake): direct stations decide where they agree with
// anyone, a Shake that mirrors one confirms it but is left out, two Shakes agree on
// their own, and a lone Shake must wander as a grid does.
TEST(SeismicFuse, MixedNetworkDirectStationsDecide) {
  const std::size_t n = 3600;
  std::mt19937 rng(7);
  std::normal_distribution<double> nd;
  auto make = [&](double sign, double sigma, float snr, double offset = 0, bool flat = false) {
    seismic::Track t;
    t.f_mhz.resize(n), t.snr_db.assign(n, snr);
    for (std::size_t i = 0; i < n; ++i)
      t.f_mhz[i] = float(flat ? offset + 0.2 * nd(rng) : sign * dev_mhz(double(i)) + offset + sigma * nd(rng));
    return t;
  };
  auto count = [](const std::vector<float>& v) {
    int k = 0;
    for (float x : v) k += std::isfinite(x);
    return k;
  };
  seismic::FuseOptions o;
  o.lone_max_noise_mhz = 0;  // a direct station never alone
  o.lone_max_noise_nyquist_mhz = 9, o.lone_min_sd_mhz = 2;
  const auto direct = make(1, 0.5, 30), shake = make(1, 4, 15), shake2 = make(1, 4, 15), mirror = make(-1, 4, 15), mirror2 = make(-1, 4, 15);
  {  // a direct station and a Shake that agree: both used
    o.nyquist = {0, 1};
    seismic::FuseStats st;
    auto f = seismic::fuse({direct, shake}, {"d", "s"}, &st, o);
    EXPECT_GT(count(f), int(n * 0.9));
    EXPECT_GT(st.used_s[0], i64(n * 0.9));
    EXPECT_GT(st.used_s[1], i64(n * 0.9));
  }
  {  // two Shakes mirroring the direct station: the direct station stands, the Shakes are out
    o.nyquist = {0, 1, 1};
    seismic::FuseStats st;
    auto f = seismic::fuse({direct, mirror, mirror2}, {"d", "m1", "m2"}, &st, o);
    EXPECT_GT(count(f), int(n * 0.9));
    EXPECT_EQ(st.used_s[1] + st.used_s[2], 0);
    double se = 0;
    int k = 0;
    for (std::size_t i = 0; i < n; ++i)
      if (std::isfinite(f[i])) se += std::pow(f[i] - dev_mhz(double(i)), 2), ++k;
    EXPECT_LT(std::sqrt(se / k), 1.0);
  }
  {  // two Shakes that agree, no direct station humming
    o.nyquist = {1, 1};
    EXPECT_GT(count(seismic::fuse({shake, shake2}, {"s1", "s2"}, nullptr, o)), int(n * 0.9));
  }
  {  // alone: a strong Shake that wanders stands; a flat one (an inverter) or a direct station does not
    o.nyquist = {1};
    EXPECT_GT(count(seismic::fuse({shake}, {"s"}, nullptr, o)), int(n * 0.9));
    EXPECT_EQ(count(seismic::fuse({make(1, 0, 15, 0, true)}, {"flat"}, nullptr, o)), 0);
    EXPECT_EQ(count(seismic::fuse({make(1, 4, 8)}, {"weak"}, nullptr, o)), 0);  // 8 dB: 16 mHz, too noisy alone
    o.nyquist = {0};
    EXPECT_EQ(count(seismic::fuse({direct}, {"d"}, nullptr, o)), 0);
  }
}

// IPS/UPS from Shakes: offline only, and stations of grids that left IPS/UPS end with it.
TEST(SeismicNetworks, IpsShakeIsMixedAndOffline) {
  const auto& n = seismic::ips_shake();
  ASSERT_EQ(seismic::network("seismic_ips_shake"), &n);
  EXPECT_TRUE(n.mixed);
  EXPECT_FALSE(n.nyquist);
  EXPECT_EQ(n.mains_hz, 50);
  int shakes = 0, direct = 0;
  for (auto& s : n.stations()) {
    const std::string id = s.id;
    (id.starts_with("AM.") ? shakes : direct)++;
    EXPECT_FALSE(s.score.empty()) << id;
  }
  EXPECT_EQ(direct, int(seismic::ips().stations().size()));
  EXPECT_GT(shakes, 0);
  ::unsetenv("GEOLOC_SHAKE_REFRESH");
  for (auto& [src, months] : refs::refresh_plan("ips_ups")) EXPECT_NE(src, "seismic_ips_shake");  // not from the dashboard
}
