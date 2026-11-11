// Shared test fixtures: a throwaway reference store per test, and one synthetic
// four-grid store (built once) for the tests that only read it.
#pragma once

#include <gtest/gtest.h>
#include <stdlib.h>

#include <cmath>
#include <filesystem>
#include <random>
#include <string>
#include <vector>

#include "store/store.hpp"
#include "util/time.hpp"

namespace geo::test {

namespace fs = std::filesystem;

inline constexpr const char* MONTH = "2020-02";

inline fs::path make_temp_dir(const std::string& tag) {
  std::string tmpl = (fs::temp_directory_path() / ("geoloc-test-" + tag + "-XXXXXX")).string();
  char* p = ::mkdtemp(tmpl.data());
  return p ? fs::path(p) : fs::path(tmpl);
}

inline void use_cache(const fs::path& dir) {
  ::setenv("GEOLOC_CACHE", dir.c_str(), 1);
  store::invalidate_caches();
}

// A fresh, empty store for one test (removed afterwards).
class TempStore : public ::testing::Test {
 protected:
  void SetUp() override {
    dir_ = make_temp_dir("store");
    use_cache(dir_);
  }
  void TearDown() override {
    store::invalidate_caches();
    std::error_code ec;
    fs::remove_all(dir_, ec);
  }
  fs::path dir_;
};

// Grid-like frequency deviation in Hz: slow drift (~30 min) plus the faster wander
// (~1 min) that real grids show.
inline std::vector<float> fake_grid(unsigned seed, std::size_t n) {
  std::mt19937_64 rng(seed);
  std::normal_distribution<double> nd;
  auto ar1 = [&](double phi, double sd) {
    std::vector<double> o(n);
    double s = sd * std::sqrt(1 - phi * phi), x = 0;
    for (std::size_t i = 0; i < n; ++i) o[i] = x = phi * x + nd(rng) * s;
    return o;
  };
  auto a = ar1(0.9995, 0.03), b = ar1(0.98, 0.01);
  std::vector<float> out(n);
  for (std::size_t i = 0; i < n; ++i) out[i] = static_cast<float>(a[i] + b[i]);
  return out;
}

// Four synthetic grids, one month each; continental_europe has a gap at 1000..1600.
// Built once per test binary; returns the month start.
inline i64 use_refstore() {
  static fs::path dir;
  auto [a, b] = month_bounds(MONTH);
  if (dir.empty()) {
    dir = make_temp_dir("refstore");
    use_cache(dir);
    const std::size_t n = std::size_t(b - a);
    auto v1 = fake_grid(1, n);
    for (int i = 1000; i < 1600; ++i) v1[std::size_t(i)] = NAN;
    store::save_month("continental_europe", "test", MONTH, v1, {{"source", "test"}});
    store::save_month("nordic", "test", MONTH, fake_grid(2, n), {{"source", "test"}});
    store::save_month("great_britain", "test", MONTH, fake_grid(3, n), {{"source", "test"}});
    store::save_month("us_eastern", "test", MONTH, fake_grid(4, n), {{"source", "test"}});
  }
  use_cache(dir);
  return a;
}

inline double utc(const std::string& s) { return parse_iso_datetime(s)->utc(); }

inline double corrcoef(const std::vector<double>& a, const std::vector<double>& b) {
  double ma = 0, mb = 0;
  std::size_t n = 0;
  for (std::size_t i = 0; i < a.size(); ++i)
    if (std::isfinite(a[i]) && std::isfinite(b[i])) ma += a[i], mb += b[i], ++n;
  ma /= double(n);
  mb /= double(n);
  double ab = 0, aa = 0, bb = 0;
  for (std::size_t i = 0; i < a.size(); ++i)
    if (std::isfinite(a[i]) && std::isfinite(b[i])) {
      ab += (a[i] - ma) * (b[i] - mb);
      aa += (a[i] - ma) * (a[i] - ma);
      bb += (b[i] - mb) * (b[i] - mb);
    }
  return ab / std::sqrt(aa * bb);
}

inline std::vector<double> normal(std::size_t n, unsigned seed, double sd = 1.0) {
  std::mt19937_64 rng(seed);
  std::normal_distribution<double> nd(0.0, sd);
  std::vector<double> v(n);
  for (auto& x : v) x = nd(rng);
  return v;
}

}  // namespace geo::test
