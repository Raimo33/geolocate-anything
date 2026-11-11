#include "audio.hpp"
#include "cuts.hpp"
#include "helpers.hpp"
#include "locate.hpp"
#include "synth.hpp"

using namespace geo;

static constexpr int FS = 1000;

static std::vector<double> clip(const store::RefSeries& ref, i64 start, double dur, double snr_db, double gain, unsigned seed) {
  auto h = synth::hum_from_reference(ref, start, dur, 50, FS, {{1, 0.3}, {2, 1.0}, {3, 0.4}, {4, 0.2}}, 0.0, seed);
  double p = 0;
  for (double v : h) p += v * v;
  p /= double(h.size());
  auto n = test::normal(h.size(), seed + 1000, std::sqrt(p / std::pow(10.0, snr_db / 10)));
  for (std::size_t i = 0; i < h.size(); ++i) h[i] = gain * (h[i] + n[i]);
  return h;
}

static std::vector<cuts::Cut> find(const std::vector<double>& x) { return cuts::detect_cuts(x, FS, enf::detect_hum(x, FS)); }

TEST(Cuts, UneditedRecordingIsOneSegment) {
  const i64 t0 = test::use_refstore();
  auto ref = *store::load_grid("continental_europe");
  for (unsigned seed = 0; seed < 4; ++seed) EXPECT_TRUE(find(clip(ref, t0 + 200000 + seed * 5000, 600, -10, 1.0, seed)).empty()) << seed;
}

TEST(Cuts, FindsSplicesBetweenClips) {
  const i64 t0 = test::use_refstore();
  auto ref = *store::load_grid("continental_europe");
  const double durs[] = {70, 95, 60, 110}, gains[] = {1.0, 3.0, 0.5, 1.5};
  std::vector<double> x, truth;
  double t = 0;
  for (int i = 0; i < 4; ++i) {
    auto p = clip(ref, t0 + 300000 + i * 40000, durs[i], -10, gains[i], 10 + unsigned(i));
    x.insert(x.end(), p.begin(), p.end());
    t += durs[i];
    if (i < 3) truth.push_back(t);
  }
  auto cs = find(x);
  int hit = 0;
  for (auto& c : cs) EXPECT_TRUE(std::any_of(truth.begin(), truth.end(), [&](double tt) { return std::fabs(c.time_s - tt) <= 1.5; }));  // nothing spurious
  for (double tt : truth) hit += std::any_of(cs.begin(), cs.end(), [&](auto& c) { return std::fabs(c.time_s - tt) <= 1.5; });
  EXPECT_GE(hit, 2);
  auto segs = cuts::segments_from_cuts(double(x.size()) / FS, cs);
  EXPECT_EQ(segs.front().start_s, 0.0);
  EXPECT_NEAR(segs.back().end_s, double(x.size()) / FS, 1e-9);
}

