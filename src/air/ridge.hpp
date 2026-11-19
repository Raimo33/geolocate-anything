// Ridge tracking and an accelerating-pass fit: checking a detected flyover against the
// tone that is actually in the spectrogram.
//
// The detector's templates are straight constant-speed passes. Real aircraft slow down
// on approach, climb, turn and change power, so the true curve drifts away from the best
// template. The ridge is the tone followed frame by frame (a Viterbi path of the offset
// from the template's curve, with a cost per bin of change), so it can bend where the
// aircraft did. A pass model with constant along-track acceleration is then fitted to the
// ridge (least squares in Hz), giving the speed and slant distance at closest approach,
// the acceleration, and how well a single moving source explains the tone (RMS error and
// the fraction of frames that hold it). Speech, birds and clutter seldom keep a tone for
// tens of seconds on a pass-shaped curve, so these two numbers also reject them.
#pragma once

#include <vector>

#include "air/doppler.hpp"
#include "air/spectrogram.hpp"

namespace geo::air {

// f / f0 at reception time t for a pass at constant acceleration a along a straight line:
// speed v and slant distance d at closest approach, tc the reception time of the sound
// emitted there (as cv_ratio, which this equals for a = 0). NaN where the modelled speed
// would fall below 5 m/s (outside the pass the model describes).
double ca_ratio(double t, double tc, double v, double d, double a, double c = SOUND_SPEED);

struct Ridge {
  std::vector<double> t, f, z;  // frame centre (s), traced frequency (Hz), whitened level there
  std::vector<unsigned char> held;  // the tone is present there (z >= the frame's held level)
  double coverage = 0;          // fraction of frames within the core window holding the tone
};

// The tone near f_line * cv_ratio(t; tc, v, d), within +-max(4%, 3 bins), over
// tc +- half_window_s * 1.5 (within the clip).
Ridge trace_ridge(const Spectrogram& s, double tc, double v, double d, double f_line, double half_window_s);

struct PassFit {
  bool ok = false;
  double tc = 0, v = 0, d = 0, f0 = 0, a = 0;  // a in m/s^2 (negative: slowing down)
  double rmse_hz = INFINITY;                   // weighted RMS of the kept ridge points
  int n = 0;                                   // ridge points used (z >= 2, outliers dropped)
};

// Least squares of ca_ratio to the held ridge points, from the template's
// (tc, v, d, f0); a constrained to +-3 m/s^2.
PassFit fit_pass(const Ridge& r, double tc, double v, double d, double f0);

}  // namespace geo::air
