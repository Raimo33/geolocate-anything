// Search engine: find where (which grid) and when (which second) an ENF trace occurred.
//
// Exact sliding Pearson correlation of drift-removed signals (the matched filter),
// scored jointly with the level evidence (clock-error prior vs the grid's own level
// spread). The reference streams from the compressed store in jobs of 2^20 lags; each
// job is decoded and filtered once and correlated against every query of the batch
// (all segments and hum hypotheses of a file) by overlap-save FFTs sized to the query.
#pragma once

#include <optional>
#include <span>
#include <string>
#include <vector>

#include "enf/extract.hpp"
#include "store/store.hpp"

namespace geo::match {

using enf::ENFTrace;

struct Candidate {
  std::string grid;
  double start_utc = NAN;  // epoch seconds of the recording's first sample
  double r = NAN;          // correlation of the drift-removed wobble
  double score = NAN;      // ranking score (joint log-likelihood ratio, or r)
  double r_raw = NAN, offset_hz = NAN, rmse_hz = NAN;
  // Hum measured from audio is the grid's frequency itself, in Hz, so at the true time
  // the clip's wobble has the grid's size (gain: slope of the clip's wobble on the
  // reference's, ~1) and the clip rarely strays from the reference (mismatch: share of
  // frames more than MISMATCH_HZ off, after the constant offset). A noise trace can
  // correlate with a grid whose wobble is 5x its own, or follow a spiky grid's shape
  // while sitting at the band edges: NaN = not measured.
  double gain = NAN, mismatch = NAN;
  int origin = 0;  // index of the query (hum hypothesis) that produced it
  std::string start_iso() const { return iso(start_utc); }
  bool consistent() const;  // gain and mismatch within physical bounds (unmeasured passes)
};
constexpr double GAIN_MIN = 0.5, GAIN_MAX = 2.0, MISMATCH_HZ = 0.1, MISMATCH_MAX = 0.1;

struct GridResult {
  std::string grid;
  double searched_from = NAN, searched_to = NAN, coverage = 0;
  i64 n_lags = 0;
  std::vector<Candidate> candidates;
  double seconds = 0;
  std::string note;
  std::optional<int> nominal;
  int trace_index = 0;
  // Candidates rank consistent ones first; a grid whose best is not consistent has no
  // match (its scores still count as wrong matches when judging the others).
  const Candidate* best() const { return candidates.empty() || !candidates[0].consistent() ? nullptr : &candidates[0]; }
  double margin() const { return candidates.size() < 2 ? NAN : candidates[0].score - candidates[1].score; }
};

struct SearchOptions {
  std::optional<i64> from, to;
  int top_k = 30;
  i64 block_s = 1 << 20;
  int hp_window_s = 61;
  double min_overlap = 0.7;
  bool joint = true;  // false: rank by Pearson r alone
  // Two-pass search: correlate block means of `decimate` samples first, then score the
  // lags around the best `shortlist` bumps per block exactly (same r, same scores as
  // the single pass). 1 = single exact pass. Queries shorter than
  // decimate * min_coarse_len samples use a smaller factor (60 s clips: none).
  int decimate = 4;
  int shortlist = 512;
  int min_coarse_len = 32;
  // Threads the blocks are spread over: 0 = $GEOLOC_THREADS, else every core. The
  // result does not depend on it.
  int threads = 0;
};

int search_threads(int requested = 0);

int hp_window_for(int m, int hp_window_s = 61);

// One result per trace (same order). Traces whose query has too few valid samples get
// a result with n_lags == 0 and a note.
std::vector<GridResult> search_grid(std::span<const ENFTrace* const> traces, const std::string& grid, const SearchOptions& o = {});
GridResult search_grid(const ENFTrace& trace, const std::string& grid, const SearchOptions& o = {});

// Pearson r of q against every window of y (NaN = invalid). Exposed for tests.
std::vector<double> sliding_corr(std::span<const double> q, std::span<const double> y, double min_overlap = 0.7);
std::vector<std::size_t> local_peaks(std::span<const float> score, std::size_t k, int exclusion);
std::vector<float> joint_score(std::span<const float> r, std::span<const double> level_ref, double q_mean, int m);
void joint_score_into(std::span<const float> r, std::span<const float> level_ref, double q_mean, int m, std::vector<float>& out);
std::vector<Candidate> merge_topk(std::vector<Candidate> c, std::size_t k, double exclusion);

// How sure a verdict is, weakest first (so levels compare with < and >=).
enum class Level { None, Weak, Moderate, Strong };
const char* level_name(Level l);                       // "none" | "weak" | "moderate" | "strong"
inline bool located(Level l) { return l >= Level::Moderate; }  // a confident match

struct Verdict {
  Level level = Level::None;
  std::optional<std::string> grid;
  std::optional<Candidate> candidate;
  std::vector<std::string> reasons;
  double p_false = NAN;
};
double false_match_probability(std::vector<double> scores, std::size_t k_max = 30);
// Wobble of a 1 s series with its outliers clipped at k robust standard deviations
// (median +- k * 1.4826 MAD), so a few spikes can't make a correlation on their own.
void clip_outliers(std::span<double> x, double k = 4.0);
void clip_outliers(std::span<float> x, double k = 4.0);
Level confidence_level(double p, double r);
Verdict judge(const std::vector<GridResult>& results, double duration_s);

std::string pyf(double x, const char* spec);  // printf, but "nan"/"inf" like Python

}  // namespace geo::match
