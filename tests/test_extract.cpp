#include <random>
#include <set>

#include "dsp/filters.hpp"
#include "enf/extract.hpp"
#include "helpers.hpp"
#include "synth.hpp"

using namespace geo;

static std::vector<double> sines(std::size_t n, double fs, const std::vector<std::pair<double, double>>& parts, double noise, unsigned seed) {
  auto x = test::normal(n, seed, noise);
  for (std::size_t i = 0; i < n; ++i)
    for (auto [f, a] : parts) x[i] += a * std::sin(2 * M_PI * f * double(i) / fs);
  return x;
}

TEST(Detect, Finds60HzFamily) {
  auto x = sines(30000, 1000, {{120, 1.0}, {180, 0.5}}, 3.0, 0);
  EXPECT_EQ(enf::detect_hum(x, 1000).nominal, 60);
}

TEST(Detect, NoHumInNoise) {
  for (unsigned seed = 0; seed < 5; ++seed) EXPECT_FALSE(enf::detect_hum(test::normal(30000, seed), 1000).nominal) << seed;
}

// Smoothed truth at each frame of a trace.
static std::vector<double> truth_at(const store::RefSeries& ref, i64 start, const enf::ENFTrace& tr) {
  const i64 i0 = start - ref.start - 1;
  std::vector<double> raw(ref.values.begin() + i0, ref.values.begin() + i0 + i64(tr.times.back()) + 11);
  auto sm = dsp::smooth_like_frames(raw, tr.frame_s);
  std::vector<double> out;
  for (double t : tr.times) out.push_back(sm[std::size_t(std::lrint(t - 0.5) + 1)]);
  return out;
}

TEST(Extract, RidgeTracksReferenceAtLowSnr) {
  const i64 start = test::use_refstore() + 20000;
  auto ref = *store::load_grid("nordic");
  auto hum = synth::hum_from_reference(ref, start, 300, 50, 1000, {{1, 0.3}, {2, 1.0}, {3, 0.4}, {4, 0.2}}, 20, 0);
  for (auto [snr, min_r] : {std::pair{-10.0, 0.97}, std::pair{-22.0, 0.85}}) {
    auto x = synth::bury(hum, snr, 1);
    auto spec = enf::Spectrum::of(x, 1000);
    auto tr = enf::extract_enf(spec, enf::detect_hum(x, 1000, &spec, {}));
    auto tru = truth_at(ref, start, tr);
    auto dev = tr.deviation();
    EXPECT_EQ(tr.nominal, 50);
    EXPECT_GT(test::corrcoef(dev, tru), min_r) << snr;
    // The clock error shows up as a constant offset of 50 Hz * 20 ppm = 1 mHz.
    double s = 0;
    int n = 0;
    for (std::size_t i = 0; i < dev.size(); ++i)
      if (std::isfinite(dev[i])) s += dev[i] - tru[i], ++n;
    EXPECT_NEAR(s / n, 0.001, 0.0007) << snr;
  }
}

// Grid-like deviation per second: smoothed random walk, ~20 mHz.
static std::vector<double> wobble(std::size_t n, unsigned seed) {
  auto e = test::normal(n, seed);
  std::vector<double> w(n);
  double acc = 0, mean = 0;
  for (std::size_t i = 0; i < n; ++i) mean += (w[i] = acc += e[i] * 0.003);
  mean /= double(n);
  std::vector<double> s(n, 0.0);
  for (std::size_t i = 0; i < n; ++i) {  // numpy.convolve(w - mean, ones(20)/20, "same")
    for (int k = -10; k < 10; ++k) {
      std::ptrdiff_t j = std::ptrdiff_t(i) + k;
      if (j >= 0 && j < std::ptrdiff_t(n)) s[i] += (w[std::size_t(j)] - mean) / 20;
    }
  }
  double var = 0, ms = 0;
  for (double v : s) ms += v;
  ms /= double(n);
  for (double v : s) var += (v - ms) * (v - ms);
  double sd = std::sqrt(var / double(n));
  for (auto& v : s) v = 0.02 * v / sd;
  return s;
}

static std::vector<double> tone(const std::vector<double>& dev_per_s, double f0, int fs) {
  std::vector<double> out(dev_per_s.size() * std::size_t(fs));
  double ph = 0;
  for (std::size_t i = 0; i < out.size(); ++i) {
    ph += 2 * M_PI * (f0 + dev_per_s[i / std::size_t(fs)]) / fs;
    out[i] = std::sin(ph);
  }
  return out;
}

TEST(Consensus, IgnoresLoudDeviceTones) {
  // Hum at 100 and 200 Hz; much louder steady tones inside the 3rd and 5th harmonics'
  // bands, like the 250.81 Hz whine in real phone recordings. SNR ranking takes the
  // tones first; agreement does not.
  const int fs = 1000;
  const std::size_t n = 400;
  auto enf_dev = wobble(n, 1);
  std::vector<double> d2(n), d4(n);
  for (std::size_t i = 0; i < n; ++i) d2[i] = 2 * enf_dev[i], d4[i] = 4 * enf_dev[i];
  auto a = tone(d2, 100, fs), b = tone(d4, 200, fs);
  auto noise = test::normal(n * fs, 0, 0.3);
  std::vector<double> x(n * fs);
  for (std::size_t i = 0; i < x.size(); ++i)
    x[i] = 0.05 * a[i] + 0.05 * b[i] + 0.5 * std::sin(2 * M_PI * 150.4 * double(i) / fs) + 0.5 * std::sin(2 * M_PI * 250.81 * double(i) / fs) +
           noise[i];
  auto r = [&](const enf::ENFTrace& tr) {
    std::vector<double> v, t;
    for (std::size_t k = 0; k + 11 < n; ++k) {
      double at = double(k) + 6;
      std::size_t j = std::size_t(std::clamp(at - tr.times[0], 0.0, double(tr.times.size() - 1)));
      v.push_back(tr.freq[j] - tr.nominal);
      t.push_back(enf_dev[std::size_t(at)]);
    }
    return test::corrcoef(v, t);
  };
  auto spec = enf::Spectrum::of(x, fs);
  auto det = enf::detect_hum(x, fs, &spec, {});
  enf::ExtractOptions o;
  o.nominal = 50;
  o.consensus = false;
  auto old_tr = enf::extract_enf(spec, det, o);
  o.consensus = true;
  auto new_tr = enf::extract_enf(spec, det, o);
  std::set<int> oh(old_tr.harmonics.begin(), old_tr.harmonics.end());
  EXPECT_TRUE(oh.count(3) && oh.count(5));
  EXPECT_LT(r(old_tr), 0.8);
  auto nh = new_tr.harmonics;
  std::sort(nh.begin(), nh.end());
  EXPECT_EQ(nh, (std::vector<int>{2, 4}));
  EXPECT_GT(r(new_tr), 0.95);
}
