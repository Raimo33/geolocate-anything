// Synthesise mains hum that follows a real grid's recorded frequency (tests, demos,
// evaluation): pick a moment of a reference series, produce audio whose hum carries
// exactly that wobble, optionally buried in noise or mixed under real audio.
#pragma once

#include <cstdint>
#include <map>
#include <optional>
#include <vector>

#include "store/store.hpp"

namespace geo::synth {

std::vector<double> hum_from_reference(const store::RefSeries& ref, i64 start_utc, double duration_s, int nominal, int rate = 1000,
                                       const std::map<int, double>& harmonics = {{1, 0.3}, {2, 1.0}, {3, 0.4}, {4, 0.2}},
                                       double clock_ppm = 0.0, std::uint64_t seed = 0);
std::vector<double> bury(const std::vector<double>& signal, double snr_db, std::uint64_t seed = 0,
                         const std::vector<double>* background = nullptr);

}  // namespace geo::synth
