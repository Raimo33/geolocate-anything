#include "air/commands.hpp"

#include <CLI/CLI.hpp>

#include <cstdio>
#include <cmath>
#include <iostream>
#include <set>

#include "air/adsb.hpp"
#include "air/bucket.hpp"
#include "air/events.hpp"
#include "air/search.hpp"
#include "air/terrain.hpp"
#include "air/spectrogram.hpp"
#include "audio.hpp"
#include "util/files.hpp"
#include "util/json.hpp"
#include "util/text.hpp"
#include "util/time.hpp"

namespace geo::air {

namespace {

std::vector<std::pair<double, double>> parse_windows(const std::vector<std::string>& specs) {
  std::vector<std::pair<double, double>> w;
  for (auto& s : specs) {
    auto c = s.find(':');
    if (c == std::string::npos) throw std::invalid_argument("window must be START:END in seconds: " + s);
    w.emplace_back(std::stod(s.substr(0, c)), std::stod(s.substr(c + 1)));
  }
  return w;
}

void print_events(const std::vector<Event>& ev) {
  std::printf("%zu flyover(s)\n", ev.size());
  for (std::size_t i = 0; i < ev.size(); ++i) {
    auto& e = ev[i];
    std::printf("  #%zu  CPA at %8.2f s [%.2f..%.2f]  speed %5.1f m/s [%.0f..%.0f]  slant %5.0f m [%.0f..%.0f]  f0 %6.1f Hz  score %5.1f (detect %.1f, %.0f MAD)\n",
                i + 1, e.tc, e.tc_lo, e.tc_hi, e.v, e.v_lo, e.v_hi, e.d, e.d_lo, e.d_hi, e.f0, e.score, e.detect_score, e.significance);
    if (e.lines.size() > 1) {
      std::printf("        lines:");
      for (auto& l : e.lines) std::printf(" %.1f Hz (%.1f)", l.f0, l.score);
      std::printf("\n");
    }
  }
}

std::optional<Bbox> parse_bbox(const std::string& s) {
  if (s.empty()) return std::nullopt;
  double w, so, e, n;
  if (std::sscanf(s.c_str(), "%lf,%lf,%lf,%lf", &w, &so, &e, &n) != 4) throw std::invalid_argument("bbox must be WEST,SOUTH,EAST,NORTH: " + s);
  return Bbox{w, so, e, n};
}

double parse_time(const std::string& s, const char* what) {
  if (auto t = parse_iso_utc(s)) return double(*t);
  if (auto t = to_double(s)) return *t;
  throw std::invalid_argument(std::string(what) + ": not an ISO time or epoch seconds: " + s);
}

}  // namespace

void register_commands(CLI::App& app, int& rc) {
  auto* air = app.add_subcommand("aircraft", "aircraft Doppler: flyovers in a recording, ADS-B history, location search");
  air->require_subcommand(1);

  // aircraft events FILE
  auto* ev = air->add_subcommand("events", "find and measure aircraft flyovers (Doppler curves) in a recording");
  static std::string ev_file, ev_json;
  static std::vector<std::string> ev_windows;
  static double ev_fmin = 35, ev_fmax = 600, ev_thr = 0;
  ev->add_option("file", ev_file)->required();
  ev->add_option("--window", ev_windows, "only look for closest approaches in START:END (seconds from the start; repeatable)");
  ev->add_option("--fmin", ev_fmin, "lowest rest frequency searched (Hz)");
  ev->add_option("--fmax", ev_fmax, "highest rest frequency searched (Hz)");
  ev->add_option("--threshold", ev_thr, "detection score threshold (default: from the recording)");
  ev->add_option("--json", ev_json, "write the events here");
  ev->callback([&rc] {
    auto au = audio::load_audio(ev_file, SPEC_RATE);
    auto spec = whitened_spectrogram(au.samples);
    EventOptions o;
    o.windows = parse_windows(ev_windows);
    o.fmin = ev_fmin, o.fmax = ev_fmax, o.threshold = ev_thr;
    auto det = detect_events(spec, o);
    auto& events = det.events;
    std::printf("noise: median %.1f, MAD %.2f; threshold %.1f\n", det.noise_median, det.noise_mad, det.threshold);
    print_events(events);
    if (!ev_json.empty()) {
      json::array a;
      for (auto& e : events) a.push_back(event_json(e));
      atomic_write(ev_json, dump(json::object{{"file", ev_file}, {"duration_s", au.duration()}, {"events", a}}, {.indent = 2}));
    }
    rc = events.empty() ? 2 : 0;
  });

  // aircraft locate FILE
  auto* lo = air->add_subcommand("locate", "find where a recording was made from its aircraft flyovers and stored ADS-B history");
  static std::string lo_file, lo_from, lo_to, lo_bbox, lo_events, lo_json, lo_near;
  static int lo_min_events = 2, lo_max_events = 6;
  static double lo_cell = 200, lo_c = SOUND_SPEED, lo_wf = 1.0;
  static int lo_win = 2048;
  static double lo_region = 3.0;
  static bool lo_silence_whole = false, lo_no_silence = false, lo_known_types = false, lo_offline = false;
  lo->add_option("file", lo_file)->required();
  lo->add_option("--from", lo_from, "earliest start of the recording (ISO 8601 UTC or epoch seconds)")->required();
  lo->add_option("--to", lo_to, "latest start of the recording")->required();
  lo->add_option("--bbox", lo_bbox, "search area WEST,SOUTH,EAST,NORTH")->required();
  lo->add_option("--events", lo_events, "use these events (JSON from `aircraft events --json`) instead of detecting");
  lo->add_option("--window", ev_windows, "only detect closest approaches in START:END (seconds from the start; repeatable)");
  lo->add_option("--min-events", lo_min_events, "flyovers a location must explain (1 with a tight time window)");
  lo->add_option("--max-events", lo_max_events, "strongest flyovers considered");
  lo->add_option("--cell", lo_cell, "corridor grid (m)");
  lo->add_option("--region", lo_region, "process the box in regions of this many degrees (bounds memory)");
  lo->add_option("--fft-window", lo_win, "spectrogram window for scoring, samples at 4 kHz");
  lo->add_option("--window-factor", lo_wf, "scoring window relative to the event's measured half window");
  lo->add_option("--sound-speed", lo_c, "effective speed of sound (m/s)");
  lo->add_flag("--no-silence", lo_no_silence, "do not reject places that would have heard other aircraft between the flyovers");
  lo->add_flag("--silence-whole", lo_silence_whole, "apply the silence test to the whole recording, not only between the matched flyovers");
  lo->add_flag("--known-types", lo_known_types, "only aircraft of known (non-jet) type");
  lo->add_flag("--offline", lo_offline, "no DEM downloads (heights fall back to the geoid)");
  lo->add_option("--json", lo_json, "write a JSON report here");
  lo->callback([&rc] {
    auto au = audio::load_audio(lo_file, SPEC_RATE);
    SpecOptions sopt;
    sopt.win = lo_win, sopt.nfft = std::max(4096, 2 * lo_win);
    auto spec = whitened_spectrogram(au.samples, sopt);
    std::vector<Event> events;
    if (!lo_events.empty()) {
      auto txt = read_file(lo_events);
      if (!txt) throw std::invalid_argument("cannot read " + lo_events);
      for (auto& e : parse_json(*txt).as_object().at("events").as_array()) events.push_back(event_from_json(e));
    } else {
      EventOptions eo;
      eo.windows = parse_windows(ev_windows);
      events = detect_events(spec, eo).events;
    }
    print_events(events);
    SearchOptions so;
    so.start_min = parse_time(lo_from, "--from"), so.start_max = parse_time(lo_to, "--to");
    so.box = *parse_bbox(lo_bbox);
    so.min_events = lo_min_events, so.max_events = lo_max_events, so.cell_m = lo_cell, so.c = lo_c, so.window_factor = lo_wf;
    so.silence = !lo_no_silence, so.silence_whole = lo_silence_whole, so.unknown_types = !lo_known_types;
    const double t0 = so.start_min - 600, t1 = so.start_max + au.duration() + 600;
    bool any_day = false;
    for (i64 d = floor_div(i64(t0), 86400) - 1; d <= floor_div(i64(t1), 86400); ++d) any_day |= ensure_day(iso_date(d * 86400));
    if (!any_day) std::printf("no ADS-B history stored for this window: run `geoloc aircraft fetch DAY`\n");
    Terrain terrain(!lo_offline);
    auto res = search_regions(
        spec, events, [&](const Bbox& b) { return split_tracks(load_tracks(t0, t1, b)); }, terrain, so, lo_region,
        [](const std::string& m) { std::fprintf(stderr, "%s\n", m.c_str()); });
    std::printf("%s", render_result(res, events).c_str());
    if (!lo_json.empty()) {
      json::array ea;
      for (auto& e : events) ea.push_back(event_json(e));
      auto j = result_json(res, events);
      j["events"] = ea;
      j["file"] = lo_file;
      atomic_write(lo_json, dump(j, {.indent = 2}));
    }
    rc = res.level >= AirLevel::Moderate ? 0 : 2;
  });

  // aircraft fetch DAY...
  auto* fe = air->add_subcommand("fetch", "store days of ADSB.lol history (non-jet aircraft), reading ~4 GB per day");
  static std::vector<std::string> fe_days, fe_parts;
  static std::string fe_bbox;
  static bool fe_force = false, fe_jets = false, fe_push = false, fe_drop = false, fe_json = false;
  fe->add_option("day", fe_days, "UTC day(s), YYYY-MM-DD, or ranges FIRST..LAST (fetched newest first)")->required();
  fe->add_option("--parts", fe_parts, "read the day's tar parts from these local files instead of downloading (one day only)");
  fe->add_option("--bbox", fe_bbox, "keep only fixes inside WEST,SOUTH,EAST,NORTH");
  fe->add_flag("--jets", fe_jets, "keep jets too");
  fe->add_flag("--force", fe_force, "fetch again even when stored (here, or in the bucket with --push)");
  fe->add_flag("--push", fe_push, "upload each day to the ADS-B bucket (GEOLOC_AIRPLANE_BUCKET); days it holds are skipped");
  fe->add_flag("--drop", fe_drop, "with --push: delete each day's local store once uploaded (backfilling on a small disk)");
  fe->add_flag("--json", fe_json, "print a JSON summary (the dashboard's refetch job)");
  fe->callback([&rc] {
    if (!fe_parts.empty() && fe_days.size() != 1) throw std::invalid_argument("--parts takes one day");
    if (fe_drop && !fe_push) throw std::invalid_argument("--drop needs --push");
    if (fe_push && !bucket::enabled()) throw std::invalid_argument("--push needs GEOLOC_AIRPLANE_BUCKET");
    std::vector<std::string> days;
    for (auto& spec : fe_days) {
      const auto dots = spec.find("..");
      const std::string a = spec.substr(0, dots), b = dots == std::string::npos ? a : spec.substr(dots + 2);
      const auto ta = parse_iso_utc(a), tb = parse_iso_utc(b);
      if (!ta || !tb || a.size() != 10 || b.size() != 10) throw std::invalid_argument("day must be YYYY-MM-DD or FIRST..LAST: " + spec);
      for (i64 d = floor_div(i64(*tb), 86400); d >= floor_div(i64(*ta), 86400); --d) days.push_back(iso_date(d * 86400));
    }
    std::set<std::string> in_bucket;
    if (fe_push && !fe_force) in_bucket = bucket::days();
    rc = 0;
    json::array report;
    for (auto& day : days) {
      json::object r{{"day", day}};
      if (in_bucket.contains(day)) {
        if (!fe_json) std::printf("%s: already in the bucket\n", day.c_str());
        r["state"] = "in bucket";
        report.push_back(r);
        continue;
      }
      if (has_day(day) && !fe_force) {
        if (!fe_json) std::printf("%s: already stored (%s)\n", day.c_str(), day_path(day).c_str());
        r["state"] = "stored";
      } else {
        IngestOptions o;
        o.parts = fe_parts, o.bbox = parse_bbox(fe_bbox), o.keep_jets = fe_jets;
        try {
          auto st = ingest_day(day, o);
          if (!fe_json) {
            std::printf("%s: %s, %zu traces read, %zu kept, %zu jets dropped, %zu without airborne fixes, %zu of unknown type\n", day.c_str(),
                        st.release.c_str(), st.members, st.kept, st.jets, st.ground_only, st.unknown_type);
            std::printf("  %zu fixes: %zu barometric altitude corrected per aircraft, %zu per region, %zu uncorrected; %.0f MB in, %.1f MB stored\n",
                        st.fixes, st.baro_track, st.baro_region, st.baro_raw, st.bytes_in / 1e6, st.bytes_out / 1e6);
            std::fflush(stdout);
          }
          r["state"] = "fetched", r["release"] = st.release, r["aircraft"] = st.kept, r["fixes"] = st.fixes;
          r["mb_in"] = std::round(st.bytes_in / 1e5) / 10, r["mb_stored"] = std::round(st.bytes_out / 1e5) / 10;
        } catch (const std::exception& e) {
          std::fprintf(stderr, "%s: %s\n", day.c_str(), e.what());
          r["state"] = "error", r["error"] = e.what();
          report.push_back(r);
          rc = 1;
          continue;
        }
      }
      if (fe_push) {
        try {
          bucket::push({day});
          r["pushed"] = true;
          if (fe_drop)
            for (const char* ext : {".trk", ".used"}) fs::remove(adsb_dir() / (day + ext));
        } catch (const std::exception& e) {
          std::fprintf(stderr, "%s: %s\n", day.c_str(), e.what());
          r["state"] = "error", r["error"] = e.what();
          rc = 1;
        }
      }
      report.push_back(r);
    }
    if (fe_json) std::printf("%s\n", dump(json::object{{"days", report}}).c_str());
  });

  // aircraft push [DAY...]
  auto* pu = air->add_subcommand("push", "upload stored ADS-B days to the ADS-B bucket (GEOLOC_AIRPLANE_BUCKET)");
  static std::vector<std::string> pu_days;
  pu->add_option("day", pu_days, "UTC day(s), YYYY-MM-DD (default: every stored day)");
  pu->callback([&rc] {
    if (!bucket::enabled()) throw std::invalid_argument("GEOLOC_AIRPLANE_BUCKET is not set");
    std::printf("%d day(s) uploaded\n", bucket::push(pu_days));
    rc = 0;
  });

  // aircraft pull [DAY...]
  auto* pl = air->add_subcommand("pull", "download ADS-B days (and every day's coverage) from the ADS-B bucket");
  static std::vector<std::string> pl_days;
  static bool pl_progress = false;
  pl->add_option("day", pl_days, "UTC day(s), YYYY-MM-DD (default: only the coverage sidecars)");
  pl->add_flag("--progress-json", pl_progress, "progress as JSON lines");
  pl->callback([&rc] {
    if (!bucket::enabled()) throw std::invalid_argument("GEOLOC_AIRPLANE_BUCKET is not set");
    rc = 0;
    for (auto& d : pl_days)
      if (!has_day(d) && !bucket::pull_day(d)) std::fprintf(stderr, "%s: not in the bucket\n", d.c_str()), rc = 1;
    const int n = bucket::pull_coverage([](int done, int total) {
      if (pl_progress) std::printf("%s\n", dump(json::object{{"done", done}, {"total", total}}).c_str()), std::fflush(stdout);
    });
    if (pl_progress) std::printf("%s\n", dump(json::object{{"files", n}}).c_str());
    else std::printf("%d coverage file(s) downloaded\n", n);
  });
}

}  // namespace geo::air
