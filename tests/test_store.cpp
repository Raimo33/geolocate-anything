#include <fstream>
#include <random>

#include "helpers.hpp"
#include "refs/parsers.hpp"
#include "refs/sources.hpp"
#include "store/codec.hpp"
#include "util/files.hpp"

using namespace geo;
using geo::test::TempStore;

static void write_manifest_months(const fs::path& cache, const std::string& months_json) {
  atomic_write(cache / "refs" / "g" / "s" / "manifest.json", R"({"months": )" + months_json + "}");
  store::invalidate_caches();
}

TEST(Codec, RoundtripAndMissing) {
  const double v[] = {0.0, 0.1234, -0.25, NAN, 3.5, -3.5};
  float out[6];
  for (int i = 0; i < 6; ++i) out[i] = codec::decode(codec::encode(v[i]));
  for (int i = 0; i < 3; ++i) EXPECT_NEAR(out[i], v[i], 0.5 / codec::SCALE + 1e-7);
  EXPECT_TRUE(std::isnan(out[3]));
  EXPECT_NEAR(out[4], codec::MAX_ABS_HZ, 1e-4);  // clipped, not wrapped
  EXPECT_NEAR(out[5], -codec::MAX_ABS_HZ, 1e-4);
}

TEST_F(TempStore, ReadsMatchWrittenDataAtAnyOffset) {
  auto [a, b] = month_bounds("2020-02");
  std::mt19937_64 rng(1);
  std::normal_distribution<double> nd;
  std::vector<float> vals(std::size_t(b - a + 3 * 86400));  // into March
  double acc = 0;
  for (auto& v : vals) v = float(std::clamp(acc += nd(rng) * 1e-4, -3.0, 3.0));
  for (int i = 50000; i < 60000; ++i) vals[std::size_t(i)] = NAN;
  store::write_range("g", "s", a, vals);
  write_manifest_months(dir_, R"({"2020-02": {}, "2020-03": {}})");
  std::vector<float> expect(vals.size());
  for (std::size_t i = 0; i < vals.size(); ++i)
    expect[i] = std::isfinite(vals[i]) ? codec::decode(codec::encode(std::nearbyint(double(vals[i]) / 1e-3) * 1e-3)) : NAN;
  for (auto [s0, n] : std::vector<std::pair<i64, i64>>{{0, 10}, {86399, 2}, {86000, 3 * 86400}, {b - a - 7, 20}, {0, i64(vals.size())}}) {
    auto got = store::read("g", a + s0, a + s0 + n);
    for (i64 k = 0; k < n; ++k) {
      float e = expect[std::size_t(s0 + k)], g = got[std::size_t(k)];
      ASSERT_TRUE((std::isnan(e) && std::isnan(g)) || e == g) << s0 << "+" << k;
    }
  }
}

TEST_F(TempStore, WriteDoesNotEraseExistingDataInGaps) {
  auto [a, b] = month_bounds("2020-03");
  store::write_range("g", "s", a, std::vector<float>(100, 0.01f));
  std::vector<float> patch(100, NAN);
  for (int i = 40; i < 60; ++i) patch[std::size_t(i)] = 0.02f;
  store::write_range("g", "s", a, patch);
  write_manifest_months(dir_, R"({"2020-03": {}})");
  auto got = store::read("g", a, a + 100);
  for (int i = 0; i < 40; ++i) EXPECT_NEAR(got[std::size_t(i)], 0.01, 1e-4);
  for (int i = 40; i < 60; ++i) EXPECT_NEAR(got[std::size_t(i)], 0.02, 1e-4);
}

TEST_F(TempStore, SourcePriority) {
  auto [a, b] = month_bounds("2020-04");
  std::vector<float> hi(std::size_t(b - a), NAN);
  for (int i = 0; i < 10; ++i) hi[std::size_t(i)] = 0.05f;
  store::save_month("g", "neso", "2020-04", hi, {});
  store::save_month("g", "local", "2020-04", std::vector<float>(std::size_t(b - a), -0.05f), {});
  auto got = store::read("g", a, a + 20);
  for (int i = 0; i < 10; ++i) EXPECT_NEAR(got[std::size_t(i)], 0.05, 1e-4);   // official source wins
  for (int i = 10; i < 20; ++i) EXPECT_NEAR(got[std::size_t(i)], -0.05, 1e-4);  // local fills its gaps
}

TEST(Accumulator, FineAndCoarse) {
  auto [a, b] = month_bounds("2021-03");
  store::RegularAccumulator acc(a, b);
  for (double t = double(a); t < double(a + 3600); t += 10) acc.add(t, std::sin(t / 100.0));  // 10 s source -> interpolated
  auto v = acc.finish();
  for (int i = 0; i < 3591; ++i) ASSERT_TRUE(std::isfinite(v[std::size_t(i)]));
  for (int i = 4000; i < 5000; ++i) ASSERT_TRUE(std::isnan(v[std::size_t(i)]));
  EXPECT_NEAR(v[5], std::sin(double(a + 5) / 100.0), 2e-3);
  store::RegularAccumulator fine(a, b);  // 10 Hz source -> per-second means, any chunking
  for (int k = 0; k < 600; ++k) fine.add(double(a) + k * 0.1, 1.0);
  auto v2 = fine.finish();
  for (int i = 0; i < 60; ++i) EXPECT_NEAR(v2[std::size_t(i)], 1.0, 1e-6);
}

TEST(Time, IsoParsingAndMonths) {
  EXPECT_EQ(parse_iso_utc("2019-04-17"), 1555459200);
  EXPECT_EQ(parse_iso_utc("2019-04-17T09:41:07Z"), 1555494067);
  EXPECT_EQ(parse_iso_utc("2019-04-17 11:41:07+02:00"), 1555494067);
  EXPECT_FALSE(parse_iso_utc("2019-13-01"));
  EXPECT_EQ(iso(1555494067.9), "2019-04-17 09:41:07 UTC");
  EXPECT_EQ(months_between("2019-11", "2020-02"), (std::vector<std::string>{"2019-11", "2019-12", "2020-01", "2020-02"}));
  EXPECT_EQ(month_bounds("2020-02").second - month_bounds("2020-02").first, 29 * 86400);
}

TEST_F(TempStore, ReplaceRangeOverwritesGapsToo) {
  auto [a, b] = month_bounds("2020-03");
  store::write_range("g", "s", a, std::vector<float>(100, 0.01f));
  std::vector<float> patch(50, NAN);
  for (int i = 10; i < 20; ++i) patch[std::size_t(i)] = 0.02f;
  store::replace_range("g", "s", a + 20, patch);
  write_manifest_months(dir_, R"({"2020-03": {}})");
  auto got = store::read("g", a, a + 100);
  for (int i = 0; i < 20; ++i) EXPECT_NEAR(got[std::size_t(i)], 0.01, 1e-4);
  for (int i = 20; i < 30; ++i) EXPECT_TRUE(std::isnan(got[std::size_t(i)]));
  for (int i = 30; i < 40; ++i) EXPECT_NEAR(got[std::size_t(i)], 0.02, 1e-4);
  for (int i = 40; i < 70; ++i) EXPECT_TRUE(std::isnan(got[std::size_t(i)]));
  for (int i = 70; i < 100; ++i) EXPECT_NEAR(got[std::size_t(i)], 0.01, 1e-4);
}

TEST_F(TempStore, ManifestWritesKeepOtherProcessesEntries) {
  // Two backfill processes of one source: each caches the manifest, then stores its own
  // months. Neither may drop the other's entries (each change is made on the copy on disk).
  auto [a, b] = month_bounds("2020-05");
  const std::vector<float> v(std::size_t(b - a), 0.01f);
  store::save_month("g", "seis", "2020-05", v, {});
  ASSERT_TRUE(store::has_month("g", "seis", "2020-05"));  // this process's cache now holds 2020-05 only
  // The other process's write, behind this one's back.
  const fs::path p = store::refs_dir() / "g" / "seis" / "manifest.json";
  auto other = parse_json(*read_file(p)).get_object();
  other["months"].get_object()["2021-01"] = json::object{{"coverage", 1.0}};
  other["unavailable"] = json::object{{"1999-01", json::object{{"reason", "none"}}}};
  atomic_write(p, dump(other));
  store::mark_unavailable("g", "seis", "2000-01", "none");
  auto [c, d] = month_bounds("2020-06");
  store::save_month("g", "seis", "2020-06", std::vector<float>(std::size_t(d - c), 0.02f), {});
  auto man = parse_json(*read_file(p)).get_object();
  for (const char* m : {"2020-05", "2020-06", "2021-01"}) EXPECT_TRUE(man["months"].get_object().contains(m)) << m;
  for (const char* m : {"1999-01", "2000-01"}) EXPECT_TRUE(man["unavailable"].get_object().contains(m)) << m;
}
