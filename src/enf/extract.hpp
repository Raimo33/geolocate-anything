// Electrical Network Frequency (ENF) extraction from audio.
//
// detect_hum: how strongly each mains harmonic (50/60 Hz family) stands out of its
// noise floor (whitened segment-averaged spectrum), plus a time-frequency ridge test
// when that fails. extract_enf: each usable harmonic's spectrum evaluated by exact DFT
// on a shared grid of fundamental offsets, noise-normalised, combined (GLRT) and
// tracked with Viterbi; harmonics are chosen by agreement of their wander.
#pragma once

#include <map>
#include <optional>
#include <span>
#include <vector>

#include "dsp/fft.hpp"

namespace geo::enf {

inline constexpr int NOMINALS[2] = {50, 60};
// Chosen on real data (identification benchmark): 12 s frames.
inline constexpr double DEFAULT_FRAME_S = 12.0;
inline constexpr double RIDGE_LOG10P = 3.0;
inline constexpr double RIDGE_MIN_S = 45.0;

struct HarmonicStat {
  int nominal = 0, harmonic = 0;
  double centre_hz = 0, peak_hz = 0, snr_db = 0;
  bool shared = false;  // a harmonic of both 50 and 60 (300, 600 Hz ...)
};

struct HumDetection {
  std::optional<int> nominal;
  std::map<int, double> score_db;
  std::vector<HarmonicStat> harmonics;
  double min_snr_db = 6.0;
  std::vector<std::pair<int, double>> ridge_log10p;  // in test order (50, 60)
  bool detected() const { return nominal.has_value(); }
  std::vector<HarmonicStat> usable(std::size_t max_harmonics = 4) const;
};

struct ENFTrace {
  std::vector<double> times, freq, snr_db;  // frame centres (s), Hz (NaN unreliable), dB
  int nominal = 50;
  std::vector<int> harmonics;
  double frame_s = DEFAULT_FRAME_S, hop_s = 1.0;
  std::vector<std::vector<int>> alternatives;
  std::vector<double> deviation() const;
  double valid_fraction() const;
};

// Mean-removed real spectrum of a recording, shared by every analysis of it.
struct Spectrum {
  fft::Buf<fft::cd> X;
  double T = 0;  // duration (s)
  double fs = 0;
  std::size_t n = 0;
  static Spectrum of(std::span<const double> x, double fs);
};

struct DetectOptions {
  int max_harmonic = 8;
  double band_hz = 0.25, min_snr_db = 6.0, detect_snr_db = 10.0;
  std::optional<double> max_hz;
  bool ridge = true;
};
HumDetection detect_hum(std::span<const double> x, double fs, const DetectOptions& o = {});
// Same, reusing a spectrum already computed for the recording (for the ridge test).
HumDetection detect_hum(std::span<const double> x, double fs, const Spectrum* spec, const DetectOptions& o);

std::optional<double> nyquist_limit(std::optional<int> source_rate);
std::vector<int> plausible_nominals(const HumDetection& d, double floor_db = 3.0);

struct ExtractOptions {
  std::optional<int> nominal;
  double frame_s = DEFAULT_FRAME_S, hop_s = 1.0, band_hz = 0.25, step_hz = 0.001;
  std::size_t max_harmonics = 6;
  double min_snr_db = 6.0, drift_mhz = 2.0, evidence_scale = 1.0, max_jump_hz = 0.02, min_frame_snr_db = 0.0;
  bool consensus = true;       // false: loudest harmonics ("snr" selection)
  bool snr_weighting = false;  // weight harmonics by long-term SNR instead of equally (GLRT)
  double consensus_r = 0.5, pool_snr_db = 3.0;
  std::optional<std::vector<int>> harmonics;  // caller's choice (e.g. an alternative group)
};
// Throws std::invalid_argument when the recording is too short or has no hum.
ENFTrace extract_enf(const Spectrum& spec, const HumDetection& det, const ExtractOptions& o = {});

// Building blocks (exposed for tests and the ridge detector).
std::vector<float> harmonic_spectrogram(const Spectrum& s, double nominal, int harmonic, std::span<const double> offsets,
                                        double frame_s, double hop_s, std::size_t n_frames, double band_hz, int n_noise = 48);
std::vector<int> viterbi_ridge(std::span<const float> score, std::size_t K, double step_hz, double lam, double max_jump_hz);
struct Consensus {
  std::vector<std::vector<int>> groups;
};
Consensus consensus_harmonics(const std::map<int, std::vector<double>>& tracks, double smooth_frames, double min_r = 0.5,
                              int hp_frames = 61, const std::vector<int>& lenient = {}, double admit_r = 0.25);
// Fake fundamentals whose harmonics stay clear of every 50/60 Hz line (the ridge null).
std::vector<double> null_nominals(const std::vector<int>& harmonics, double band_hz, double frame_s, double lo = 40.0, double hi = 60.5,
                                  std::size_t n = 24);
std::vector<std::pair<int, double>> ridge_detect(std::span<const double> x, double fs, const std::vector<HarmonicStat>& stats,
                                                 const Spectrum* spec, double band_hz = 0.25);

}  // namespace geo::enf
