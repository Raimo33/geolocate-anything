#include "dsp/filters.hpp"
#include "enf/match.hpp"
#include "helpers.hpp"

using namespace geo;

static double brute(const std::vector<double>& q, const std::vector<double>& y, std::size_t lag, double min_overlap = 0.7) {
  double n = 0, nq = 0, sq = 0, sy = 0, sqq = 0, syy = 0, sqy = 0;
  for (std::size_t j = 0; j < q.size(); ++j) {
    nq += std::isfinite(q[j]);
    double a = q[j], b = y[lag + j];
    if (std::isfinite(a) && std::isfinite(b)) n++, sq += a, sy += b, sqq += a * a, syy += b * b, sqy += a * b;
  }
  if (n < min_overlap * nq) return NAN;
  return (sqy - sq * sy / n) / std::sqrt((sqq - sq * sq / n) * (syy - sy * sy / n));
}

class CorrPaths : public ::testing::TestWithParam<std::pair<bool, bool>> {};

TEST_P(CorrPaths, MatchBruteForce) {
  auto [q_gaps, y_gaps] = GetParam();
  auto y = test::normal(5000, 0);
  if (y_gaps)
    for (int i = 100; i < 130; ++i) y[std::size_t(i)] = NAN;
  auto noise = test::normal(300, 1, 0.1);
  std::vector<double> q(y.begin() + 2000, y.begin() + 2300);
  for (std::size_t i = 0; i < q.size(); ++i) q[i] += noise[i];
  if (q_gaps)
    for (int i = 10; i < 20; ++i) q[std::size_t(i)] = NAN;
  auto r = match::sliding_corr(q, y);
  for (std::size_t lag : {0, 90, 1234, 2000, 4700}) {
    double b = brute(q, y, lag);
    // Gap-free inputs take the single-precision fast path; the masked path is double.
    if (std::isnan(b)) EXPECT_TRUE(std::isnan(r[lag]));
    else EXPECT_NEAR(r[lag], b, q_gaps ? 3e-7 : 2e-5) << lag;  // r is stored as float
  }
  std::size_t best = 0;
  for (std::size_t i = 0; i < r.size(); ++i)
    if (std::isfinite(r[i]) && (!std::isfinite(r[best]) || r[i] > r[best])) best = i;
  EXPECT_EQ(best, 2000u);
}

INSTANTIATE_TEST_SUITE_P(Gaps, CorrPaths, ::testing::Values(std::pair{false, false}, std::pair{true, false}, std::pair{false, true}, std::pair{true, true}));

TEST(Filters, MovingMeanMatchesNaive) {
  auto x = test::normal(500, 1);
  for (int i = 50; i < 60; ++i) x[std::size_t(i)] = NAN;
  std::vector<double> mm(x.size());
  dsp::moving_mean<double>(x, 21, mm);
  for (int i : {0, 5, 55, 100, 499}) {
    double s = 0;
    int n = 0;
    for (int k = std::max(0, i - 10); k <= std::min(499, i + 10); ++k)
      if (std::isfinite(x[std::size_t(k)])) s += x[std::size_t(k)], ++n;
    double expect = n >= 0.3 * 21 ? s / n : NAN;
    if (std::isnan(expect)) EXPECT_TRUE(std::isnan(mm[std::size_t(i)]));
    else EXPECT_NEAR(mm[std::size_t(i)], expect, 1e-12);
  }
  std::vector<double> ramp(600);
  for (int i = 0; i < 600; ++i) ramp[std::size_t(i)] = i / 599.0;
  auto hp = dsp::highpass(ramp, 61);
  for (int i = 40; i < 560; ++i) EXPECT_NEAR(hp[std::size_t(i)], 0.0, 1e-9);
}

TEST(Peaks, MatchNaiveLocalMaxima) {
  auto x = test::normal(3000, 7);
  std::vector<float> f(x.begin(), x.end());
  for (int i = 400; i < 450; ++i) f[std::size_t(i)] = NAN;
  const int excl = 37;
  std::vector<std::size_t> naive;
  for (std::ptrdiff_t i = 0; i < std::ptrdiff_t(f.size()); ++i) {
    if (std::isnan(f[std::size_t(i)])) continue;
    bool peak = true;
    for (std::ptrdiff_t k = std::max<std::ptrdiff_t>(0, i - excl); k <= std::min<std::ptrdiff_t>(2999, i + excl) && peak; ++k)
      peak = !(f[std::size_t(k)] > f[std::size_t(i)]);
    if (peak) naive.push_back(std::size_t(i));
  }
  std::sort(naive.begin(), naive.end(), [&](auto a, auto b) { return f[a] > f[b]; });
  naive.resize(10);
  EXPECT_EQ(match::local_peaks(f, 10, excl), naive);
}

TEST(Verdict, FalseMatchProbabilityBehaviour) {
  // numpy.random.default_rng(2).gumbel(0.5, 0.02, 30), sorted: the Python test's draws.
  std::vector<double> null{0.5573906199601631, 0.5467811790199077, 0.5440689721060148, 0.5435365237189982, 0.5363302031453255,
                           0.5314684001518947, 0.5313915485207484, 0.523861578566852,  0.5226925810703842, 0.5207397113744251,
                           0.519197507803719,  0.5171327299700438, 0.5139847724572808, 0.5119734779680847, 0.5113569153703589,
                           0.5090323848431132, 0.5066946989428952, 0.5038197457930197, 0.5017429460145767, 0.4999421635563001,
                           0.49862276555884494, 0.4979753209140219, 0.4972211502518791, 0.496632464593414,  0.49469102380947355,
                           0.49196865712756155, 0.48958579182433737, 0.4840649028880777, 0.4810451428938238, 0.47538061928760517};
  EXPECT_NEAR(match::false_match_probability(null), 0.7611215581881567, 1e-12);  // a lucky wrong one is not significant
  auto strong = null;
  strong[0] = 0.95;
  EXPECT_NEAR(match::false_match_probability(strong), 3.123070109301614e-05, 1e-15);
}

TEST(Search, StreamingEqualsInMemoryAcrossBlockSeams) {
  test::use_refstore();
  auto [t0, t1] = month_bounds(test::MONTH);
  auto ref = store::read("great_britain", t0, t1);
  const std::size_t start = 500000;
  enf::ENFTrace tr;
  tr.nominal = 50;
  tr.frame_s = 1.0;
  for (std::size_t i = 0; i < 400; ++i) {
    tr.times.push_back(double(i));
    tr.freq.push_back(50.0 + ref[start + i]);
    tr.snr_db.push_back(0);
  }
  match::SearchOptions o;
  o.block_s = 499850;  // the true lag straddles a block seam
  o.top_k = 5;
  auto res = match::search_grid(tr, "great_britain", o);
  ASSERT_TRUE(res.best());
  EXPECT_LT(std::fabs(res.best()->start_utc - double(t0 + i64(start))), 0.05);  // sub-second refined
  EXPECT_GT(res.best()->r, 0.99);  // < 1: the query is high-passed without outside context (Python: same value)
  EXPECT_EQ(res.n_lags, (t1 - t0) - 400 + 1);
}

TEST(Search, JointScoreRewardsConsistentLevel) {
  const std::size_t n = 20000, m = 300;
  auto z = test::normal(n, 5, 0.1);
  std::vector<float> r(n);
  for (std::size_t i = 0; i < n; ++i) r[i] = float(std::tanh(z[i]));  // null-like correlations
  r[5000] = r[15000] = 0.6f;                                            // two equally well-shaped candidates
  auto noise = test::normal(n + m - 1, 6, 0.02);
  std::vector<double> level(n + m - 1, 0.0);
  for (std::size_t i = 5000; i < 5000 + m; ++i) level[i] = 0.020;    // window mean 20 mHz
  for (std::size_t i = 15000; i < 15000 + m; ++i) level[i] = -0.030;  // window mean -30 mHz
  for (std::size_t i = 0; i < level.size(); ++i) level[i] += noise[i];
  auto sc = match::joint_score(r, level, 0.021, int(m));  // clip at +21 mHz (1 mHz clock error)
  EXPECT_GT(sc[5000], sc[15000] + 3);
  EXPECT_EQ(std::max_element(sc.begin(), sc.end()) - sc.begin(), 5000);
}

// The two-pass search (decimated correlation, then exact scores around a shortlist)
// finds what the single exact pass finds, with the same scores (all top_k of them), on
// clean data, with the query over a reference gap, and with gaps among the other lags.
TEST(Search, TwoPassMatchesExactPass) {
  test::use_refstore();
  auto [t0, t1] = month_bounds(test::MONTH);
  for (auto [grid, start] : {std::pair{"great_britain", std::size_t(700000)}, std::pair{"continental_europe", std::size_t(1500)}, std::pair{"continental_europe", std::size_t(2000)}}) {
    auto ref = store::read(grid, t0, t1);
    auto noise = test::normal(600, 9, 0.004);
    enf::ENFTrace tr;
    tr.nominal = 50;
    tr.frame_s = 12.0;
    for (std::size_t i = 0; i < 600; ++i) {
      const double v = ref[start + i];
      tr.times.push_back(double(i));
      tr.freq.push_back(std::isfinite(v) ? 50.0 + v + noise[i] : NAN);
      tr.snr_db.push_back(0);
    }
    match::SearchOptions exact, fast;
    exact.decimate = 1;
    fast.decimate = 4;
    auto a = match::search_grid(tr, grid, exact), b = match::search_grid(tr, grid, fast);
    ASSERT_TRUE(a.best() && b.best()) << grid;
    EXPECT_LT(std::fabs(b.best()->start_utc - double(t0 + i64(start))), 1.5) << grid;
    EXPECT_NEAR(b.best()->start_utc, a.best()->start_utc, 1e-3) << grid;
    EXPECT_NEAR(b.best()->r, a.best()->r, 1e-4) << grid;
    EXPECT_NEAR(b.best()->score, a.best()->score, 1e-3 * std::fabs(a.best()->score)) << grid;
    EXPECT_EQ(a.n_lags, b.n_lags);
    ASSERT_EQ(a.candidates.size(), b.candidates.size());
    for (std::size_t k = 1; k < a.candidates.size(); ++k) EXPECT_NEAR(b.candidates[k].score, a.candidates[k].score, 1e-3 * std::fabs(a.candidates[k].score) + 1e-3) << grid << " #" << k;
  }
}
