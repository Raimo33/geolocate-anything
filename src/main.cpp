// geoloc: geolocate audio from background signals (power-grid hum, ...).
#include <CLI/CLI.hpp>

#include <cstdio>
#include <set>
#include <iostream>

#include "air/commands.hpp"
#include "enf/extract.hpp"
#include "grids.hpp"
#include "locate.hpp"
#include "refs/aemo_annual.hpp"
#include "refs/remote.hpp"
#include "refs/sources.hpp"
#include "store/store.hpp"
#include "util/files.hpp"
#include "util/json.hpp"
#include "util/text.hpp"
#include "web/server.hpp"

using namespace geo;

namespace {

std::vector<std::string> month_spec(const std::string& spec) {
  auto c = spec.find(':');
  if (c == std::string::npos) return {spec};
  return months_between(spec.substr(0, c), spec.substr(c + 1));
}

void say(const json::value& v) {
  std::cout << dump(v, {.indent = -1, .ascii = false}) << std::endl;
}

// ---------------------------------------------------------------- commands

int cmd_locate(const std::string& file, const locate::AnalyzeOptions& o, const std::string& json_out, bool web, const std::string& display,
               long long size) {
  if (web) {  // the dashboard's child process: one JSON document on stdout
    try {
      auto rep = locate::analyze(file, o);
      auto j = locate::to_web_json(rep);
      j["path"] = display.empty() ? file : display;
      j["size_bytes"] = size;
      std::cout << dump(j, {.indent = -1, .ascii = false});
      return 0;
    } catch (const std::exception& e) {
      std::cout << dump(json::object{{"error", e.what()}});
      return 3;
    }
  }
  auto rep = locate::analyze(file, o);
  std::cout << locate::render_text(rep) << std::endl;
  if (!json_out.empty()) {
    atomic_write(json_out, dump(locate::to_json(rep), {.indent = 2}));
    std::cout << "\nJSON report: " << json_out << std::endl;
  }
  return rep.verdict && match::located(rep.verdict->level) ? 0 : 2;
}

int cmd_grids(const std::string& at, bool all) {
  const std::string day = grids::normalize_day(at);
  for (auto& g : grids::load_grids().grids) {
    if (!g.exists_at(day) && !all) continue;
    auto st = g.status_at(day);
    std::printf("%-22s %d Hz  %s  (%s)%s\n", g.id.c_str(), g.nominal_hz, g.name.c_str(), st ? st->c_str() : "not in effect",
                g.tracked() ? "" : "  [no reference data]");
    auto cs = g.countries_at(day);
    if (cs.empty()) cs = g.countries();
    std::printf("%-22s        %s\n", "", join(cs, ", ").c_str());
  }
  return 0;
}

int cmd_refs_list() {
  std::printf("Reference store: %s\n", store::refs_dir().c_str());
  auto inv = store::inventory();
  if (inv.empty()) std::printf("  (empty: run `geoloc refs refresh`)\n");
  for (auto& i : inv) {
    auto man = store::read_manifest(i.grid, i.source);
    double sum = 0;
    std::size_t n = 0;
    for (auto& kv : man["months"].as_object()) sum += jnum(kv.value(), "coverage", 0), ++n;
    std::printf("  %-20s %-10s %3zu month(s)  %s .. %s  mean coverage %5.1f%%\n", i.grid.c_str(), i.source.c_str(), i.months.size(),
                i.months.front().c_str(), i.months.back().c_str(), 100 * sum / double(std::max<std::size_t>(1, n)));
    auto stale = refs::stale_months(i.grid, i.source);
    if (!stale.empty())
      std::printf("  %-20s %zu month(s) partial or stored before a timestamp fix (%s .. %s); `refs refresh --grid %s` refreshes them\n",
                  "", stale.size(), stale.front().c_str(), stale.back().c_str(), i.grid.c_str());
  }
  std::printf("  on disk: %.1f MiB (compressed month files)\n", double(store::disk_usage()) / (1 << 20));
  std::printf("\nAvailable sources:\n");
  for (auto& s : refs::sources()) std::printf("  %-14s grids: %s\n  %-14s %s\n", s.name.c_str(), join(s.grids, ", ").c_str(), "", s.description.c_str());
  return 0;
}

// AEMO's yearly workbooks, downloaded by hand (aemo.com.au refuses automated requests).
// Each month is checked against AEMO's other 4 s series where those are stored. `name`
// stands in for the file's name (uploads arrive as temporary files); `as_json` prints one
// JSON report instead of text (the dashboard's upload runs this).
int cmd_refs_import_aemo(const std::vector<std::string>& files, const std::string& grid_opt, int shift_s, const std::string& name,
                         bool as_json) {
  json::array report;
  int warnings = 0;
  std::set<std::string> touched;
  auto out = [&](const char* fmt, auto... args) {
    if (!as_json) std::printf(fmt, args...);
  };
  for (auto& f : files) {
    const std::string base = !name.empty() ? name : fs::path(f).filename().string();
    json::object rep{{"file", base}};
    try {
      MappedFile mf(f);
      if (mf.bytes().empty()) throw std::runtime_error("cannot read " + f);
      const std::string grid = grid_opt.empty() ? refs::aemo_annual_grid(base) : grid_opt;
      auto a = refs::parse_aemo_annual(mf.bytes(), 36000, shift_s);
      rep["grid"] = grid;
      rep["format"] = a.format;
      rep["sheet"] = a.sheet;
      rep["layout"] = a.layout;
      rep["samples"] = a.samples;
      rep["rejected"] = a.rejected;
      rep["first"] = iso(double(a.first));
      rep["last"] = iso(double(a.last));
      out("%s: %s, sheet '%s', %s: %zu samples (%zu not a frequency), %s .. %s -> %s\n", base.c_str(), a.format.c_str(), a.sheet.c_str(),
          a.layout.c_str(), a.samples, a.rejected, iso(double(a.first)).c_str(), iso(double(a.last)).c_str(), grid.c_str());
      json::array months;
      for (auto& [m, vals] : a.months) {
        auto [t0, t1] = month_bounds(m);
        std::size_t n = 0;
        for (float v : vals) n += std::isfinite(v);
        if (!n) continue;
        // A month may span two yearly files (NEM time is 10 h ahead). This file replaces
        // the span it covers (importing it again, say with --shift, leaves nothing of
        // the earlier import); the other file's hours outside it stay.
        json::array month_files{json::value(base)};
        {
          auto info = store::month_info(grid, "aemo_annual", m);
          const json::value* pf = info ? info->if_contains("files") : nullptr;
          if (pf && pf->is_array())
            for (auto& x : pf->as_array())
              if (x != json::value(base)) month_files.push_back(x);
        }
        json::object meta{{"source", "aemo_annual"},
                          {"files", month_files},
                          {"file_format", a.format},
                          {"sheet", a.sheet},
                          {"layout", a.layout},
                          {"licence", "AEMO copyright permissions (free use with attribution)"},
                          {"native_resolution_s", 4.0},
                          {"timestamps", "NEM time (UTC+10, no DST), converted to UTC" + (shift_s ? ", then " + std::to_string(shift_s) + " s" : std::string())}};
        json::object mrep{{"month", m}};
        std::string checks;
        for (const char* other : {"aemo_causer_pays", "aemo_fpp"}) {
          if (!store::has_month(grid, other, m)) continue;
          auto est = refs::estimate_lag(store::read(grid, t0, t1, {other}), vals);
          if (!est) continue;
          json::object c{{"lag_s", est->first}, {"r", std::nearbyint(est->second * 1e4) / 1e4}};
          meta[std::string("check_") + other] = c;
          mrep[std::string("vs_") + other] = c;
          char b[160];
          std::snprintf(b, sizeof b, "; vs %s: lag %+d s, r = %.4f", other, est->first, est->second);
          checks += b;
          if (est->first != 0 || est->second < 0.95) ++warnings, mrep["warning"] = true;
        }
        {
          std::size_t lo = 0, hi = vals.size();
          while (!std::isfinite(vals[lo])) ++lo;
          while (!std::isfinite(vals[hi - 1])) --hi;
          store::replace_range(grid, "aemo_annual", t0 + i64(lo), std::span(vals).subspan(lo, hi - lo));
          store::write_entry(grid, "aemo_annual", m, meta);  // coverage with the other file's hours, if any
        }
        touched.insert(grid);
        json::array issues;
        for (auto& i : refs::placement_issues("aemo_annual", grid, m, &meta)) {
          issues.push_back(json::value(i.message));
          out("  placement %s: %s\n", i.severity.c_str(), i.message.c_str());
        }
        if (!issues.empty()) mrep["placement"] = issues;
        mrep["filled_s"] = n;
        months.push_back(mrep);
        out("  stored %s/aemo_annual/%s: %.1f%% of the month%s\n", grid.c_str(), m.c_str(), 100.0 * double(n) / double(vals.size()),
            checks.c_str());
      }
      rep["months"] = months;
    } catch (const std::exception& e) {
      rep["error"] = e.what();
      ++warnings;
      if (!as_json) std::fprintf(stderr, "%s: %s\n", base.c_str(), e.what());
    }
    report.push_back(rep);
  }
  int pushed = -1;
  if (refs::remote::enabled())
    for (auto& g : touched) pushed = std::max(pushed, 0) + refs::remote::push(g);
  if (warnings)
    out("WARNING: %d file(s)/month(s) need a look (see above). If every month is off by the same lag, import again with --shift <lag>.\n",
        warnings);
  if (pushed >= 0) out("%d file(s) uploaded to the bucket\n", pushed);
  if (as_json) {
    json::object o{{"files", report}, {"warnings", warnings}};
    if (pushed >= 0) o["uploaded"] = pushed;
    std::printf("%s\n", dump(o).c_str());
    return touched.empty() ? 3 : 0;  // 3: nothing stored, the report says why
  }
  return warnings ? 1 : 0;
}

// Brings grids up to date (every tracked grid by default): fetches every month a source
// has that is not stored yet, then uploads to the bucket when one is configured. With
// --progress-json, progress goes to stdout as JSON lines (the dashboard's child process).
int cmd_refs_refresh(std::vector<std::string> grids, bool progress) {
  if (grids.empty())
    for (auto* g : grids::load_grids().tracked()) grids.push_back(g->id);
  const bool push = refs::remote::enabled();
  auto emit = [&](const json::object& o) {
    if (progress) say(o);
    else std::cerr << jstr(o, "state") << ": " << jstr(o, "detail") << std::endl;
  };
  int rc = 0;
  for (auto& grid : grids) try {
      emit({{"state", "running"}, {"detail", grid + ": checking the sources"}, {"step", 0}, {"steps", 1}});
      std::vector<std::string> failures;  // "source month: error", or "source: cannot list months (...)"
      auto plan = refs::refresh_plan(grid, {}, &failures);
      const int steps = std::max(1, int(plan.size()) + (push ? 1 : 0));
      std::size_t stored = 0;
      int uploaded = 0;
      for (std::size_t i = 0; i < plan.size(); ++i) {
        auto& [source, months] = plan[i];
        std::string span = months.size() == 1 ? months[0] : months.front() + ".." + months.back();
        emit({{"state", "running"}, {"detail", grid + ": " + source + " " + span + " (" + std::to_string(months.size()) + " month(s))"},
              {"step", i}, {"steps", steps}});
        // Months go up in batches as they are fetched: a long backfill loses at most one
        // batch to a redeploy or crash.
        const std::size_t batch = push ? 12 : months.size();
        for (std::size_t k = 0; k < months.size(); k += batch) {
          std::vector<std::string> part(months.begin() + std::ptrdiff_t(k), months.begin() + std::ptrdiff_t(std::min(months.size(), k + batch)));
          std::map<std::string, std::string> failed;
          stored += refs::fetch(source, grid, part, false, &failed).size();
          for (auto& [m, why] : failed) failures.push_back(source + " " + m + ": " + why);
          if (push && k + batch < months.size()) try {
              uploaded += refs::remote::push(grid);
            } catch (const std::exception& e) {  // the final push tries again
              std::cerr << "upload to the bucket failed (" << e.what() << "), retried at the end" << std::endl;
            }
        }
      }
      std::string detail = plan.empty() ? (failures.empty() ? "already up to date" : "nothing to fetch")
                                        : std::to_string(stored) + " month(s) fetched";
      if (push) {
        emit({{"state", "running"}, {"detail", grid + ": uploading to the bucket"}, {"step", plan.size()}, {"steps", steps}});
        detail += ", " + std::to_string(uploaded + refs::remote::push(grid)) + " file(s) uploaded to the bucket";
      }
      if (!failures.empty()) {  // stored months are kept; the failed ones are planned again next time
        detail += ", " + std::to_string(failures.size()) + " failed (" + failures[0] + (failures.size() > 1 ? ", ..." : "") + ")";
        throw std::runtime_error(detail);
      }
      emit({{"state", "done"}, {"detail", detail}, {"finished", iso(now_seconds())}});
    } catch (const std::exception& e) {
      emit({{"state", "error"}, {"detail", e.what()}, {"finished", iso(now_seconds())}});
      rc = 1;
    }
  return rc;
}

int cmd_refs_sync(bool is_push, const std::string& grid, bool progress) {
  if (!refs::remote::enabled()) {
    std::cerr << "no bucket configured: set GEOLOC_BUCKET (and AWS_ENDPOINT_URL, AWS_ACCESS_KEY_ID, AWS_SECRET_ACCESS_KEY)" << std::endl;
    if (progress) say(json::object{{"error", "no bucket configured"}});
    return 1;
  }
  if (is_push) {
    std::printf("%d file(s) uploaded\n", refs::remote::push(grid));
    return 0;
  }
  try {
    int n = refs::remote::pull(grid, progress ? std::function<void(int, int)>([](int d, int t) { say(json::object{{"done", d}, {"total", t}}); })
                                              : std::function<void(int, int)>());
    if (progress) say(json::object{{"files", n}});
    else std::printf("%d file(s) downloaded\n", n);
    return 0;
  } catch (const std::exception& e) {
    if (progress) say(json::object{{"error", e.what()}});
    throw;
  }
}

}  // namespace

int main(int argc, char** argv) {
  CLI::App app{"Geolocate audio from background signals (power-grid hum, ...)", "geoloc"};
  app.set_version_flag("--version", std::string("geoloc ") + GEOLOC_VERSION);
  app.require_subcommand(1);
  int rc = 0;

  // locate
  auto* loc = app.add_subcommand("locate", "full analysis of an audio/video file");
  std::string loc_file, loc_json, loc_display, date_from, date_to;
  std::vector<std::string> loc_grids;
  int loc_nominal = 0;
  bool no_split = false, web_json = false;
  long long loc_size = 0;
  loc->add_option("file", loc_file)->required();
  loc->add_option("--grid", loc_grids, "restrict to grid id (repeatable)");
  loc->add_option("--from", date_from, "search window start (ISO 8601, UTC)");
  loc->add_option("--to", date_to, "search window end (ISO 8601, UTC)");
  loc->add_option("--nominal", loc_nominal, "force 50 or 60 Hz")->check(CLI::IsMember({50, 60}));
  loc->add_flag("--no-split", no_split, "treat the file as one recording: don't look for edit points");
  loc->add_option("--json", loc_json, "write a JSON report here");
  std::string air_mode = "auto", air_bbox;
  bool air_fetch = false;
  loc->add_option("--aircraft", air_mode, "aircraft flyovers vs. ADS-B history: auto (when history is stored), on, off")
      ->check(CLI::IsMember({"auto", "on", "off"}));
  loc->add_option("--aircraft-bbox", air_bbox, "area for the aircraft search without a hum match: WEST,SOUTH,EAST,NORTH");
  loc->add_flag("--aircraft-fetch", air_fetch, "download missing ADS-B days (~4 GB each)");
  loc->add_flag("--web-json", web_json, "print the dashboard's JSON on stdout (used by `serve`)")->group("");
  loc->add_option("--display-name", loc_display)->group("");
  loc->add_option("--size", loc_size)->group("");
  loc->callback([&] {
    locate::AnalyzeOptions o;
    o.grids = loc_grids;
    if (!date_from.empty()) o.from = date_from;
    if (!date_to.empty()) o.to = date_to;
    if (loc_nominal) o.nominal = loc_nominal;
    o.split = !no_split;
    o.aircraft = air_mode == "on" ? locate::AnalyzeOptions::Aircraft::On : air_mode == "off" ? locate::AnalyzeOptions::Aircraft::Off
                                                                                           : locate::AnalyzeOptions::Aircraft::Auto;
    o.aircraft_fetch = air_fetch;
    if (!air_bbox.empty()) {
      std::array<double, 4> b{};
      if (std::sscanf(air_bbox.c_str(), "%lf,%lf,%lf,%lf", &b[0], &b[1], &b[2], &b[3]) != 4)
        throw std::invalid_argument("--aircraft-bbox must be WEST,SOUTH,EAST,NORTH");
      o.aircraft_box = b;
    }
    rc = cmd_locate(loc_file, o, loc_json, web_json, loc_display, loc_size);
  });

  // serve
  auto* srv = app.add_subcommand("serve", "web dashboard: upload a file, see where and when on a map");
  std::string host = "127.0.0.1";
  int port = std::stoi(env("PORT", "8000"));
  srv->add_option("--host", host);
  srv->add_option("--port", port);
  srv->callback([&] { web::serve(host, port); });

  // grids
  auto* gr = app.add_subcommand("grids", "list known synchronous grids");
  std::string at;
  bool all = false;
  gr->add_option("--at", at, "as of this date (YYYY-MM-DD; default today)");
  gr->add_flag("--all", all, "also grids not in effect at that date");
  gr->callback([&] { rc = cmd_grids(at, all); });

  // refs
  auto* refs = app.add_subcommand("refs", "manage reference grid-frequency data");
  refs->require_subcommand(1);
  std::string r_source, r_grid, r_months;
  std::vector<std::string> r_grids;
  bool r_force = false, r_progress = false;

  auto* rf = refs->add_subcommand("fetch", "download reference months");
  rf->add_option("source", r_source, "a source name from `refs list` (neso, netztransparenz, aemo_fpp, ...)")->required();
  rf->add_option("--grid", r_grid)->required();
  rf->add_option("--months", r_months, "YYYY-MM or YYYY-MM:YYYY-MM")->required();
  rf->add_flag("--force", r_force);
  rf->callback([&] {
    std::map<std::string, std::string> failed;
    auto done = refs::fetch(r_source, r_grid, month_spec(r_months), r_force, &failed);
    std::printf("%zu month(s) available for %s from %s", done.size(), r_grid.c_str(), r_source.c_str());
    if (!failed.empty()) std::printf(", %zu failed", failed.size());
    std::printf("\n");
    rc = done.empty() || !failed.empty() ? 1 : 0;
  });
  auto* rr = refs->add_subcommand("refresh", "fetch every month not stored yet, then upload to the bucket if one is set");
  rr->add_option("--grid", r_grids, "only this grid (repeatable; default all)");
  rr->add_flag("--progress-json", r_progress)->group("");
  rr->callback([&] { rc = cmd_refs_refresh(r_grids, r_progress); });
  for (auto [name, desc] : {std::pair{"push", "upload the local store to the bucket"},
                            std::pair{"pull", "download the bucket into the local store"}}) {
    auto* s = refs->add_subcommand(name, desc);
    s->add_option("--grid", r_grid, "only this grid");
    bool is_push = std::string(name) == "push";
    if (!is_push) s->add_flag("--progress-json", r_progress)->group("");
    s->callback([&, is_push] { rc = cmd_refs_sync(is_push, r_grid, r_progress); });
  }
  auto* rl = refs->add_subcommand("list", "show stored references and sources");
  rl->callback([&] { rc = cmd_refs_list(); });
  std::vector<std::string> r_files;
  int r_shift = 0;
  auto* rim = refs->add_subcommand("import-aemo", "import AEMO's yearly frequency workbooks, downloaded by hand from aemo.com.au");
  rim->add_option("files", r_files, ".xlsb/.xlsx/.csv (or a zip of one), e.g. 'Mainland frequency data 2019.xlsb'")->required();
  rim->add_option("--grid", r_grid, "australia_nem or australia_tas (default: Tasmania if the file name says so, else mainland)")
      ->check(CLI::IsMember({"australia_nem", "australia_tas"}));
  rim->add_option("--shift", r_shift, "seconds to add after the NEM-time conversion (a measured lag)");
  std::string r_name;
  bool r_json = false;
  rim->add_option("--name", r_name, "the file's original name (for an uploaded temporary file)")->group("");
  rim->add_flag("--json", r_json, "print a JSON report")->group("");
  rim->callback([&] { rc = cmd_refs_import_aemo(r_files, r_grid, r_shift, r_name, r_json); });
  // aircraft
  air::register_commands(app, rc);

  try {
    app.parse(argc, argv);
  } catch (const CLI::ParseError& e) {
    return app.exit(e);
  } catch (const std::exception& e) {
    std::cerr << "error: " << e.what() << std::endl;
    return 1;
  }
  return rc;
}
