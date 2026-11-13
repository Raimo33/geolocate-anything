// Grid frequency from the mains hum in seismometer recordings.
//
// Accelerometers and seismometers installed in buildings pick up the mains field (60 Hz
// in the US Eastern Interconnection, 50 Hz in IPS/UPS and China),
// and their digitisers are GPS-timed. The hum's frequency is the grid's frequency, so a
// station's recording is a grid frequency recorder that nobody meant to run.
//
// track_hum: the line nearest the nominal (folded below Nyquist when the sample rate is
// under twice it) is mixed down to 0 Hz, low-passed and decimated to 10 samples/s
// (linear phase: no delay), then each UTC second's frequency is the peak of a 12 s Hann
// frame centred on the middle of that second, searched within +-0.25 Hz and refined by
// a parabola through the log power. Output is mHz deviation from nominal, NaN where the
// frame is not fully covered by data.
//
// fuse: several stations' tracks become one. In 10-minute windows each station's noise
// follows from its median SNR (C / sqrt(SNR), C calibrated on real stations); the stations that
// agree (r >= 0.9 with another station after 12 s smoothing, as the matcher smooths)
// form the accepted set, a lone station is accepted when its noise is under a limit;
// each second is the inverse-variance weighted mean of accepted stations whose frame
// SNR passes and that lie near the others.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "refs/mseed.hpp"

namespace geo::seismic {

struct Track {
  std::int64_t t0 = 0;           // UTC second of element 0
  std::vector<float> f_mhz, snr_db;  // per second
};

struct TrackOptions {
  double nominal_hz = 60;  // the line tracked
  double mains_hz = 60;    // the grid's nominal: the deviation is scaled by mains_hz / nominal_hz
  double frame_s = 12;
  double base_rate = 10;   // baseband samples per second
  double search_hz = 0.25; // peak search half-width
  // Noise floor from the median of the probes (scaled to the mean of noise) instead of
  // their mean: a fixed tone beside the hum (seismic_brazil's recorder has one 0.65 Hz
  // away, stronger than the hum) then leaves the SNR as it is.
  bool robust_floor = false;
};

// Segments of one channel (any gaps); seconds [t0, t1).
Track track_hum(const std::vector<mseed::Segment>& segs, std::int64_t t0, std::int64_t t1, const TrackOptions& o = {});

// track_nyquist: a 50 Hz line in a 100 samples/s channel sits exactly on the Nyquist
// frequency. Sampling folds 50 + d and 50 - d onto each other, so the line arrives as a
// real beat a cos(theta), theta' = 2 pi d, whose spectrum cannot tell +d from -d. Two
// things still give the sign. The decimation filter of the digitiser (Raspberry Shake)
// rolls off through 50 Hz, so the line is weaker when the grid runs fast: its amplitude
// falls by `kappa` per mHz. And the phase turns smoothly, so a reversal of d away from
// an extremum of the beat shows. A forward-backward pass over the state (theta, d),
// with observation a exp(-kappa d) cos(theta) + noise, turns the beat into the
// deviation per second. Output as track_hum's: mHz and an SNR in dB (the line's power
// over the noise beside it, capped: the error does not keep falling with SNR). Seconds
// where the line is weak, where the model with the roll-off explains the beat no
// better than one without it (the sign is then a guess), or where the beat is faster
// than the range allows (the grid beyond +-dmax_mhz), are NaN.
struct NyquistOptions {
  double mains_hz = 50;
  double kappa = 0.0073;      // 1/mHz, measured against Continental Europe's frequency
  double dmax_mhz = 80, dstep_mhz = 2;
  int n_theta = 32;
  int amp_win_s = 1200;       // the station's hum level may drift no faster than this
  double sigma_floor = 0.13;  // noise never taken below this share of the amplitude
  double min_line_db = 7;
  double max_snr_db = 20;
  double min_evidence = 30;   // nats per hour block in favour of the roll-off model
};
// Segments must reach `pad_s` beyond [t0, t1) where data exists (NYQUIST_PAD_S).
constexpr std::int64_t NYQUIST_PAD_S = 600;
Track track_nyquist(const std::vector<mseed::Segment>& segs, std::int64_t t0, std::int64_t t1, const NyquistOptions& o = {});

struct FuseOptions {
  int window_s = 600;
  double min_snr_db = 8;
  double noise_c_mhz = 40;          // a station's error ~ this / sqrt(SNR) (12 s frames)
  double agree_r = 0.9;
  double lone_max_noise_mhz = 1.5;  // a station with nobody to check it must be this quiet (0: never alone)
  double max_abs_mhz = 400;         // beyond this a value is not grid frequency
  // Mixed networks (seismic_ips_shake): which tracks come from track_nyquist (empty:
  // none, or all when the network is a Shake network and says so through agree_r). A
  // direct line has no sign ambiguity, so where a direct station agrees with anyone, the
  // direct stations decide the window: a Nyquist track is accepted only if it agrees
  // with one of them, and one that mirrors a direct station (r <= -agree_r_nyquist: the
  // decoder chose the wrong sign for the stretch, the same way at every Shake) confirms
  // the direct station but is itself left out.
  std::vector<char> nyquist;
  double agree_r_nyquist = 0.7;      // a pair with a Nyquist track agrees from this r
  double lone_max_noise_nyquist_mhz = 0;  // a lone Nyquist track must be this quiet (0: never alone)
  double lone_min_sd_mhz = 0;        // and a lone track of either kind must wander this much in the window (an inverter's fixed 50 Hz does not)
};

struct FuseStats {
  std::vector<std::string> ids;
  std::vector<std::int64_t> used_s;  // seconds each station contributed
  std::int64_t multi_s = 0, single_s = 0;  // fused seconds backed by >= 2 stations / by one
  std::vector<std::uint8_t> n_used;         // per second: stations in the fused value
};

// Tracks must share t0 and length. Returns mHz per second (NaN = none).
std::vector<float> fuse(const std::vector<Track>& tracks, const std::vector<std::string>& ids, FuseStats* stats = nullptr,
                        const FuseOptions& o = {});

}  // namespace geo::seismic
