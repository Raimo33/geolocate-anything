// Source file formats -> 1 s series. Pure functions over bytes/lines (no I/O), fed
// incrementally so multi-GB files never sit in memory. Timestamp repairs found by
// cross-checking sources are documented in docs/grids/architecture.md.
#pragma once

#include <cmath>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "store/store.hpp"
#include "util/time.hpp"

namespace geo::refs {

inline constexpr i64 SPILL_S = 86400;  // local-time files reach at most this far outside their month
inline constexpr i64 NEM_UTC_OFFSET_S = 10 * 3600;
inline constexpr int NTP_SHIFT_S = -2;

struct Spill {
  i64 start;
  std::vector<float> values;
};
struct MonthSeries {
  std::vector<float> own;
  std::vector<Spill> spill;
};
// Cut a series starting at second a into the UTC month and the parts before/after it
// that hold data (to be written into the neighbouring months).
MonthSeries split_spill(const std::vector<float>& vals, i64 a, const std::string& month);
std::vector<float> shift(const std::vector<float>& x, int lag);  // out[t] = x[t - lag]

// Feeds complete lines (without "\r\n") from arbitrary chunks.
class LineSplitter {
 public:
  template <class F>
  void feed(std::string_view chunk, F&& on_line) {
    std::size_t a = 0;
    if (!carry_.empty()) {
      auto nl = chunk.find('\n');
      if (nl == std::string_view::npos) {
        carry_.append(chunk);
        return;
      }
      carry_.append(chunk.substr(0, nl));
      emit(carry_, on_line);
      carry_.clear();
      a = nl + 1;
    }
    for (;;) {
      auto nl = chunk.find('\n', a);
      if (nl == std::string_view::npos) break;
      emit(chunk.substr(a, nl - a), on_line);
      a = nl + 1;
    }
    carry_.append(chunk.substr(a));
  }
  template <class F>
  void finish(F&& on_line) {
    if (!carry_.empty()) emit(carry_, on_line);
    carry_.clear();
  }

 private:
  template <class F>
  static void emit(std::string_view l, F& on_line) {
    if (!l.empty() && l.back() == '\r') l.remove_suffix(1);
    on_line(l);
  }
  std::string carry_;
};

// (offset, scale) turning raw values into deviation in Hz: absolute Hz sit inside
// 40..70; mHz deviations are signed and span more than 2.
std::pair<double, double> detect_units(const std::vector<double>& sample);

// ---- NESO (Great Britain): "dtm,f" with dd/mm/yyyy UK wall-clock time (2014), ISO
// times with an explicit offset (2015-2019), then plain ISO times, which are UTC except in
// the files of 2025-04..2025-10: those are UTC+1 (BST) throughout, even after the
// 2025-10-26 change (see neso_bst_stamped).
class NesoParser {
 public:
  explicit NesoParser(const std::string& month);
  void feed(std::string_view chunk);
  MonthSeries finish();
  std::string zone;  // what the timestamps were, for the manifest
 private:
  void line(std::string_view l);
  std::string month_;
  i64 t0_, t1_;
  LineSplitter ls_;
  int tcol_ = -1, fcol_ = -1;
  int style_ = 0;  // 0 unknown, 1 ISO, 2 dmy local
  std::optional<store::RegularAccumulator> acc_;
  std::optional<LocalToUtc> tz_;
  i64 plain_offset_s_ = 0;  // subtracted from plain ISO times
};
// NESO's monthly files whose plain times are UTC+1 for the whole month.
bool neso_bst_stamped(const std::string& month);

// ---- Power Grid Frequency DB month CSV ("t,v"), any size. local_tz: wall-clock zone
// converted to UTC (rows outside the month are spilled), then moved by shift_s.
class PgfParser {
 public:
  PgfParser(const std::string& month, std::optional<std::string> local_tz, int shift_s);
  void feed(std::string_view chunk);
  MonthSeries finish();
  std::size_t rows() const { return acc_.n_samples(); }
  std::size_t repaired() const { return repaired_; }

 private:
  void line(std::string_view l);
  void flush();
  std::string month_;
  int year_, mon_;
  std::optional<LocalToUtc> tz_;
  int shift_;
  i64 a_;
  store::RegularAccumulator acc_;
  LineSplitter ls_;
  // Header: skip the first line. Sniffing: hold rows (up to 2M) until there are enough
  // to tell the units. Streaming: units known, rows flushed in 64k batches.
  enum class Phase { Header, Sniffing, Streaming } phase_ = Phase::Header;
  std::vector<std::pair<WallTime, double>> pending_;
  std::pair<double, double> units_{0.0, 1.0};  // (offset, scale), set when Streaming starts
  std::size_t repaired_ = 0;
};

// ---- Fingrid daily 10 Hz CSV (Finnish wall-clock time) into an accumulator.
void parse_fingrid_day(std::string_view csv, store::RegularAccumulator& acc);
std::map<std::string, std::string> parse_fingrid_index(std::string_view html);

// ---- KIT EDR campaign CSV ("Time;f50_<site>;QI_<site>", mHz, UTC), streamed onto the
// 1 s grid of the given months. Only QI 0 is kept. Also reads the Zenodo release's files
// (";" or "," separated, "f50"/"f60"/"freq" column, QI optional, "+00:00" honoured).
class EdrParser {
 public:
  // Most files carry a time column and mHz deviations. A few (Mallorca's 2026 eclipse
  // file) hold only "freq,QI" rows in Hz, one per second from a start KIT gives in its
  // metadata: pass that start and the nominal frequency for them.
  struct Implied {
    i64 t0;
    double nominal_hz;
  };
  // A recorder whose clock wandered: its offset (stamp minus UTC, seconds) at known
  // stamps, linear between them and held after the last. Rows stamped before the first
  // knot are dropped (the offset there is not known). Each row is moved to its stamp
  // minus the offset; a second the move leaves empty takes the mean of its neighbours.
  struct ClockKnot {
    i64 stamp;
    double offset_s;
  };
  explicit EdrParser(const std::vector<std::string>& months, std::optional<Implied> implied = {}, std::vector<ClockKnot> clock = {});
  void feed(std::string_view chunk);
  std::map<std::string, std::vector<float>> finish();
  std::size_t rows = 0, kept = 0;

 private:
  void line(std::string_view l);
  std::vector<std::string> months_;
  i64 a_, b_;
  std::vector<float> out_;
  LineSplitter ls_;
  int fcol_ = -1, qcol_ = -1;
  char sep_ = ';';
  bool header_ = true;
  std::optional<Implied> implied_;
  std::vector<ClockKnot> clock_;
  double offset_at(double stamp) const;
};

// ---- Plain time/frequency CSVs (AESO, the Grid-India PMU release, PMU and DTU files
// on Zenodo/figshare), one parser configured per source. Columns are picked by header
// name (lower case, quotes stripped; any later header line re-picks them, and lines that
// are neither header nor row, such as stack traces in the Grid-India files, are skipped);
// with no header, time is column 0 and frequency column 1. Times are ISO (an explicit
// offset is honoured) or UNIX seconds/milliseconds. Frequency is absolute Hz; rows more
// than 3 Hz off nominal or with a quality flag other than 0 are dropped. Rows go to a
// RegularAccumulator per requested UTC month (any rate; coarse sources interpolated
// across short gaps), finished once the stream is two days past the month.
struct SeriesFormat {
  std::vector<std::string> time_cols, value_cols, quality_cols;
  double nominal_hz = 50;
  int utc_offset_s = 0;  // naive times: local = UTC + offset ...
  std::string tz;        // ... or a wall-clock zone with DST (rows must be chronological)
  int shift_s = 0;       // added after conversion (a measured logging delay)
  double scale = 1.0;    // values times this give Hz (1e-3 for mHz; with nominal_hz 0 for deviations)
  double keep_from = -INFINITY, keep_to = INFINITY;  // UTC window kept (after shift_s), [from, to)
};
class SeriesParser {
 public:
  SeriesParser(const std::vector<std::string>& months, SeriesFormat f);
  void feed(std::string_view chunk);
  std::map<std::string, std::vector<float>> finish();  // every requested month (NaN where no data)
  std::size_t rows = 0, kept = 0;

 private:
  void line(std::string_view l);
  void close_before(double t);
  SeriesFormat f_;
  std::map<std::string, std::optional<store::RegularAccumulator>> open_;
  std::map<std::string, std::vector<float>> done_;
  std::optional<LocalToUtc> tz_;
  LineSplitter ls_;
  char sep_ = 0;
  int tcol_ = 0, fcol_ = 1, qcol_ = -1;
  double last_close_ = -INFINITY;
};

// Blank runs where the value (to 1 mHz) holds still for at least min_s seconds (gaps inside a run
// don't end it): a historian repeating its last value while the feed was down. Returns
// the seconds blanked.
std::size_t drop_flat_runs(std::vector<float>& v, int min_s);

// ---- LBNL micro-PMU phase angle ("<UTC ns>,<degrees>", 120 samples/s): the voltage
// angle against a GPS-locked 60 Hz reference turns at (f - 60) * 360 degrees per second,
// so each pair of consecutive samples gives the frequency; they are averaged per second.
class UpmuAngleParser {
 public:
  UpmuAngleParser(i64 t0, i64 t1, double nominal_hz = 60.0) : acc_(t0, t1), nominal_(nominal_hz) {}
  void feed(std::string_view chunk);
  std::vector<float> finish();
  std::size_t rows = 0, kept = 0;

 private:
  void line(std::string_view l);
  store::RegularAccumulator acc_;
  double nominal_;
  LineSplitter ls_;
  i64 prev_ns_ = 0;
  double prev_deg_ = NAN;
};

// ---- EirGrid Smart Grid Dashboard frequency JSON (one day of "SYS_FREQUENCY" rows,
// "01-Nov-2026 00:00:05", Irish wall-clock time, 5 s, 0.01 Hz). Returns rows kept;
// `listed` (if given) gets the number of frequency rows, null values included.
std::size_t parse_eirgrid_frequency(std::string_view json_text, LocalToUtc& tz, int shift_s, store::RegularAccumulator& acc,
                                    std::size_t* listed = nullptr);

// ---- Netztransparenz month CSV ("2021-10-31,02:00:00,Frequ,50.0060", German time, 2 s late).
class NtpParser {
 public:
  explicit NtpParser(const std::string& month);
  void feed(std::string_view chunk);
  MonthSeries finish();
  std::size_t rows() const { return acc_.n_samples(); }

 private:
  std::string month_;
  i64 a_;
  store::RegularAccumulator acc_;
  LocalToUtc tz_;
  LineSplitter ls_;
};

// ---- Irish FDR daily text file (10 Hz, UTC): "1015 051314 000001  3 50.0290 50.0289 ...".
std::size_t parse_fdr_day(std::string_view text, store::RegularAccumulator& acc);

// ---- AEMO FPP REGION_FREQ_MEASURE CSV (NEM time) into per-region accumulators ("NSW1",
// "TAS1", ...; regions without one are skipped).
class FppParser {
 public:
  explicit FppParser(std::map<std::string, store::RegularAccumulator*, std::less<>> accs) : accs_(std::move(accs)) {}
  void feed(std::string_view chunk);
  void finish();
  std::size_t rows = 0;

 private:
  void line(std::string_view l);
  std::map<std::string, store::RegularAccumulator*, std::less<>> accs_;
  std::vector<std::string> cols_;
  int i_flag_ = -1, i_region_ = -1, i_time_ = -1, i_dev_ = -1;
  LineSplitter ls_;
};

// ---- AEMO Causer Pays FCAS CSV: only the frequency rows (elements 32001 NEM, 42001 TAS).
std::size_t parse_causer_pays_csv(std::string_view data, std::map<std::string, store::RegularAccumulator*>& accs);

// ---- Energy-Charts frequency JSON ({"unix_seconds": [...], "data": [...]}).
std::size_t parse_energy_charts(std::string_view json_text, store::RegularAccumulator& acc);

// ---- index pages
std::map<std::string, std::string> parse_neso_index(std::string_view package_json);
std::vector<std::string> parse_html_links(std::string_view html, const std::string& base, const std::string& pattern);

}  // namespace geo::refs
