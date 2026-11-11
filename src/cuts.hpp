// Edit points (cuts) from jumps in the hum's phase (Rodriguez, Apolinario & Biscainho,
// IEEE TIFS 2010), with a noise-floor jump (Pan, Zhang & Lyu, ICASSP 2012) as support.
// Thresholds favour few false cuts: an unedited recording must stay in one piece.
#pragma once

#include <span>
#include <vector>

#include "enf/extract.hpp"

namespace geo::cuts {

inline constexpr double WIN_S = 1.0;
inline constexpr int SIDE_FREQ = 15, SIDE_PHASE = 5, SPREAD_WIN = 60;
inline constexpr double Z_CUT = 7.0, Z_SUPPORTED = 5.0, FLOOR_JUMP_DB = 6.0, MIN_SEGMENT_S = 5.0, EDGE_S = 10.0, MIN_SPREAD_RAD = 0.05;

struct Cut {
  double time_s = 0, phase_z = 0, floor_jump_db = 0;
  std::vector<int> harmonics;
};
struct Segment {
  double start_s = 0, end_s = 0;
  double duration_s() const { return end_s - start_s; }
};

std::vector<Cut> detect_cuts(std::span<const double> x, double fs, const enf::HumDetection& hum, std::size_t max_harmonics = 3,
                             double win_s = WIN_S);
std::vector<Cut> clean_cuts(double duration_s, std::vector<Cut> cuts, double min_gap_s = 1.0);
std::vector<Segment> segments_from_cuts(double duration_s, const std::vector<Cut>& cuts);

// Building blocks (tests).
std::vector<double> phase_steps(std::span<const fft::cd> X, double T, double centre_hz, int harmonic, double win_s = WIN_S);
std::vector<double> noise_floor_db(std::span<const double> x, double fs, double win_s, const std::vector<double>& hum_lines);

}  // namespace geo::cuts
