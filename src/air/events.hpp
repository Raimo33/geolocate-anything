// Flyover detection: finding Doppler curves in a recording and measuring them.
//
// A straight constant-speed pass gives f(t) = f0 * g(t - tc; v, d) (doppler.hpp). In
// log frequency that is one fixed shape per (v, d), shifted up or down by log f0, so a
// matched filter over every rest frequency at once is a sum of shifted spectrogram rows.
// The detector slides (v, d) templates over a pooled log-frequency copy of the whitened
// spectrogram, scores single lines and harmonic combs, keeps the separated peaks above
// a threshold set by the recording's own score distribution, then refines each on the
// full-resolution spectrogram and reports the parameter ranges that score nearly as
// well (real aircraft turn and change speed, so ranges, not points, go to the search).
#pragma once

#include <cmath>
#include <string>
#include <utility>
#include <vector>

#include "air/spectrogram.hpp"
#include "util/json.hpp"

namespace geo::air {

struct EventOptions {
  double fmin = 35.0, fmax = 600.0;   // band searched for lines (rest frequency)
  double vmin = 30.0, vmax = 180.0;   // m/s
  double dmin = 120.0, dmax = 9000.0; // closest slant distance, m
  double pool_s = 0.5;                // detection time step
  double logf_step = 0.0025;          // detection frequency step (relative)
  double min_swing = 0.04;            // a template must sweep at least this much in log f
  double threshold = 0;               // detection score; 0 = from the recording's rising-template null
  double null_k = 6.0;                // threshold = null median + null_k * null MAD
  int max_events = 12;
  double min_separation_s = 30.0;
  std::vector<std::pair<double, double>> windows;  // only look for CPAs in these spans (s from start)
  // Verification: a detection is kept only if its tone is followed through enough of the
  // half window and one accelerating pass explains it (relative RMS misfit).
  bool verify = true;
  double min_coverage = 0.25;
  double max_rel_rmse = 0.008;
};

struct Line {
  double f0 = 0;     // rest frequency, Hz
  double score = 0;  // z-like curve score
};

struct Event {
  double tc = 0, tc_lo = 0, tc_hi = 0;  // reception time of closest approach, s from start
  double v = 0, v_lo = 0, v_hi = 0;     // speed, m/s
  double d = 0, d_lo = 0, d_hi = 0;     // closest slant distance, m
  double f0 = 0;                        // strongest line's rest frequency, Hz
  std::vector<Line> lines;              // every line of this source (f0 included), strongest first
  double score = 0;                     // refined curve score of the strongest line
  double detect_score = 0;              // detection statistic
  double significance = 0;              // (detect_score - null median) / null MAD
  double half_window_s = 0;             // the curve is measured over tc +- this
  // The tone followed in the spectrogram (ridge.hpp) and the accelerating-pass fit to it.
  double ridge_hz = 0;                  // rest frequency of the line traced (the strongest)
  double ridge_coverage = 0;            // fraction of frames within the half window holding the tone
  double ridge_rmse_hz = INFINITY;      // RMS misfit of the accelerating pass (Hz, at ridge_hz)
  int ridge_points = 0;
  double accel = 0;                     // fitted along-track acceleration, m/s^2
  double fit_tc = NAN, fit_v = NAN, fit_d = NAN;  // the accelerating fit's CPA time, speed, slant
};

// Mean z along f0 * cv_ratio(t) over frames with t in [tc - hw, tc + hw], times sqrt(n):
// ~N(0,1) on noise, grows with sqrt(duration) on a matching tone.
double cv_score(const Spectrogram& s, double tc, double v, double d, double f0, double hw);

struct Detection {
  std::vector<Event> events;   // in time order
  std::vector<Event> rejected; // above the threshold but failed verification (strongest first)
  std::vector<Event> duplicates;  // a stronger kept event's source heard again (its tail, a harmonic)
  double threshold = NAN;      // detection score used
  double noise_median = NAN, noise_mad = NAN;  // of the per-instant best rising-template score (the null)
};
Detection detect_events(const Spectrogram& s, const EventOptions& o = {});

json::object event_json(const Event& e);
Event event_from_json(const json::value& v);

}  // namespace geo::air
