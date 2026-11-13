#include "locate.hpp"
#include "util/files.hpp"

#include <algorithm>
#include <cmath>
#include <iostream>
#include <map>

#include "air/adsb.hpp"
#include "air/bucket.hpp"
#include "refs/sources.hpp"
#include "store/store.hpp"
#include "util/text.hpp"
#include "util/time.hpp"

namespace geo::locate {

using enf::ENFTrace;
using enf::HumDetection;
using match::Candidate;
using match::GridResult;
using match::pyf;
using match::Verdict;

json::object region(const grids::Grid& g, const std::string& day) {
  const std::string d = grids::normalize_day(day);
  auto countries = g.countries_at(d);
  if (countries.empty()) countries = g.countries();
  json::object o;
  o["grid"] = g.id;
  o["name"] = g.name;
  o["nominal_hz"] = g.nominal_hz;
  json::array c;
  for (auto& x : countries) c.emplace_back(x);
  o["countries"] = std::move(c);
  o["tracked"] = g.tracked();
  auto st = g.status_at(d);
  o["status"] = st ? json::value(*st) : json::value(nullptr);
  return o;
}

std::optional<int> grid_nominal(const std::string& id) {
  if (auto* g = grids::load_grids().find(id)) return g->nominal_hz;
  for (auto& src : store::sources_for(id)) {
    auto man = store::read_manifest(id, src);
    if (auto* meta = man.if_contains("meta")) {
      double n = jnum(*meta, "nominal");
      if (n == 50 || n == 60) return static_cast<int>(n);
    }
  }
  return std::nullopt;
}

static std::optional<i64> parse_time(const std::optional<std::string>& s) {
  if (!s || s->empty()) return std::nullopt;
  auto t = parse_iso_utc(*s);
  if (!t) throw std::invalid_argument("Invalid isoformat string: '" + *s + "'");
  return t;
}

static std::string clock(double t) {
  char b[32];
  std::snprintf(b, sizeof b, "%d:%02d", int(std::floor(t / 60)), int(std::fmod(t, 60.0)));
  return b;
}

static double best_score(const GridResult& g) { return g.best() ? g.best()->score : -INFINITY; }

// ---------------------------------------------------------------- one continuous piece

namespace {

// A segment's hum hypotheses, waiting for the batched search.
struct Work {
  HumMatch m;
  std::vector<ENFTrace> hyps;
  std::vector<std::string> grid_ids, candidate_ids;
  std::map<std::string, std::optional<int>> gn;
  std::vector<std::vector<GridResult>> per_hyp;  // results per hypothesis
  double duration = 0;
  bool search = false;
};

Work prepare(const enf::Spectrum& spec, HumDetection hum, const AnalyzeOptions& o) {
  Work w;
  w.duration = spec.T;
  w.m.hum = std::move(hum);
  auto& rep = w.m;
  const auto& table = grids::load_grids();
  std::vector<int> noms = o.nominal ? std::vector<int>{*o.nominal} : enf::plausible_nominals(rep.hum);
  if (noms.empty()) {
    rep.warnings.push_back("No mains hum found. Common causes: battery-powered recorder far from wiring, "
                           "aggressive noise suppression (phones, VoIP), or lossy compression.");
    rep.verdict = Verdict{match::Level::None, {}, {}, {"no hum detected"}, NAN};
    return w;
  }
  // When both families carry harmonics (often a device's own 120/240 Hz series next to
  // a 50 Hz hum), both are extracted and the grid data decides; likewise an agreeing
  // alternative group of harmonics.
  for (int nom : noms) {
    try {
      enf::ExtractOptions eo;
      eo.nominal = nom;
      eo.frame_s = o.frame_s;
      auto tr = enf::extract_enf(spec, rep.hum, eo);
      auto alts = tr.alternatives;
      w.hyps.push_back(std::move(tr));
      for (std::size_t a = 0; a < std::min<std::size_t>(1, alts.size()); ++a) {
        eo.harmonics = alts[a];
        w.hyps.push_back(enf::extract_enf(spec, rep.hum, eo));
      }
    } catch (const std::invalid_argument& e) {
      rep.warnings.push_back(std::to_string(nom) + " Hz: " + e.what());
    }
  }
  if (w.hyps.empty()) {
    std::vector<std::string> rs;
    if (!rep.warnings.empty()) rs.push_back(rep.warnings.back());
    else rs.push_back("no usable hum");
    rep.verdict = Verdict{match::Level::None, {}, {}, rs, NAN};
    return w;
  }
  std::vector<int> traces;
  for (auto& t : w.hyps)
    if (std::find(traces.begin(), traces.end(), t.nominal) == traces.end()) traces.push_back(t.nominal);
  auto has = [&](int n) { return std::find(traces.begin(), traces.end(), n) != traces.end(); };
  for (auto& g : table.grids)
    if (has(g.nominal_hz)) {
      w.candidate_ids.push_back(g.id);
      rep.candidate_regions.push_back(region(g));
    }
  if (!o.grids.empty()) {
    std::vector<std::string> wrong;
    for (auto& g : o.grids)
      if (auto* gg = table.find(g); gg && !has(gg->nominal_hz)) wrong.push_back(g);
    if (!wrong.empty()) rep.warnings.push_back("ignoring grids with a different nominal frequency: " + join(wrong, ", "));
    for (auto& g : o.grids)
      if (std::find(wrong.begin(), wrong.end(), g) == wrong.end()) w.grid_ids.push_back(g);
  } else {
    for (auto& id : w.candidate_ids)
      if (table.get(id).tracked()) w.grid_ids.push_back(id);
  }
  for (auto& g : w.grid_ids) w.gn[g] = grid_nominal(g);
  w.per_hyp.resize(w.hyps.size());
  w.search = true;
  return w;
}

void finish(Work& w, const AnalyzeOptions& o) {
  auto& rep = w.m;
  if (!w.search) return;
  const auto& table = grids::load_grids();
  // One result per grid: candidates of all hypotheses merged, so the false-match
  // estimate sees every hypothesis's wrong matches.
  std::vector<std::string> order;
  std::map<std::string, std::vector<GridResult>> per_grid;
  for (std::size_t i = 0; i < w.hyps.size(); ++i) {
    auto& rs = w.per_hyp[i];
    std::stable_sort(rs.begin(), rs.end(), [](auto& a, auto& b) { return best_score(a) > best_score(b); });
    for (auto& r : rs) {
      if (!per_grid.count(r.grid)) order.push_back(r.grid);
      per_grid[r.grid].push_back(std::move(r));
    }
  }
  std::size_t maxlen = 0;
  for (auto& t : w.hyps) maxlen = std::max(maxlen, t.freq.size());
  for (auto& g : order) {
    auto& rs = per_grid[g];
    std::stable_sort(rs.begin(), rs.end(), [](auto& a, auto& b) { return best_score(a) > best_score(b); });
    GridResult res = rs[0];
    if (rs.size() > 1) {
      const double excl = std::max<double>(30, double(maxlen / 2));
      std::vector<Candidate> all;
      for (auto& x : rs) all.insert(all.end(), x.candidates.begin(), x.candidates.end());
      res.candidates = match::merge_topk(std::move(all), std::size_t(o.top_k), excl);
      if (res.best()) {
        res.trace_index = res.best()->origin;
        res.nominal = w.hyps[std::size_t(res.trace_index)].nominal;
      }
    }
    rep.results.push_back(std::move(res));
  }
  std::stable_sort(rep.results.begin(), rep.results.end(), [](auto& a, auto& b) { return best_score(a) > best_score(b); });
  const GridResult* top = !rep.results.empty() && rep.results[0].best() ? &rep.results[0] : nullptr;
  rep.trace = w.hyps[top ? std::size_t(top->trace_index) : 0];
  const int win = rep.trace->nominal;
  auto harm = [](const std::vector<int>& v) {
    std::string s = "[";
    for (std::size_t i = 0; i < v.size(); ++i) s += (i ? ", " : "") + std::to_string(v[i]);
    return s + "]";
  };
  if (w.hyps.size() > 1) {
    std::string s = std::to_string(w.hyps.size()) + " hum hypotheses searched (";
    for (std::size_t i = 0; i < w.hyps.size(); ++i)
      s += (i ? "; " : "") + std::to_string(w.hyps[i].nominal) + " Hz harmonics " + harm(w.hyps[i].harmonics);
    s += "); best: " + std::to_string(win) + " Hz harmonics " + harm(rep.trace->harmonics);
    rep.warnings.push_back(s);
  }
  if (!rep.hum.nominal || win != *rep.hum.nominal)
    rep.warnings.push_back("the loudest tones belong to the " + (rep.hum.nominal ? std::to_string(*rep.hum.nominal) : std::string("None")) +
                           " Hz family, but the " + std::to_string(win) + " Hz harmonics matched the grid data better: analysed as " +
                           std::to_string(win) + " Hz");
  rep.hum.nominal = win;
  json::array keep;
  for (auto& r : rep.candidate_regions)
    if (jint(r, "nominal_hz") == win) keep.push_back(r);
  rep.candidate_regions = std::move(keep);
  if (rep.trace->valid_fraction() < 0.5)
    rep.warnings.push_back("hum is intermittent: only " + pct0(rep.trace->valid_fraction()) + " of frames gave a usable estimate");
  std::vector<std::string> missing, untracked;
  for (auto& id : w.candidate_ids) {
    const auto& g = table.get(id);
    if (g.nominal_hz != win) continue;
    bool got = std::any_of(rep.results.begin(), rep.results.end(), [&](auto& r) { return r.grid == id; });
    if (g.tracked() && !got) missing.push_back(id);
    if (!g.tracked()) untracked.push_back(g.name);
  }
  if (!missing.empty())
    rep.warnings.push_back("no reference data loaded for: " + join(missing, ", ") + " (a match there cannot be confirmed or excluded)");
  if (!untracked.empty())
    rep.warnings.push_back(std::to_string(untracked.size()) + " " + std::to_string(win) +
                           " Hz grid(s) have no public reference data, so a recording from there cannot match: " + join(untracked, ", "));
  rep.verdict = match::judge(rep.results, w.duration);
  if (rep.verdict->candidate) {  // describe the grids as they were at the matched time
    const std::string when = grids::day_of(rep.verdict->candidate->start_utc);
    for (auto& r : rep.candidate_regions)
      if (auto* g = table.find(jstr(r, "grid"))) r = region(*g, when);
  }
}

// Stream every grid once for all hypotheses of all segments.
void search_all(std::vector<Work*>& works, const AnalyzeOptions& o) {
  match::SearchOptions so;
  so.from = parse_time(o.from);
  so.to = parse_time(o.to);
  so.top_k = o.top_k;
  struct Ref {
    Work* w;
    std::size_t h;
  };
  std::vector<std::string> grid_order;
  std::map<std::string, std::vector<Ref>> by_grid;
  for (auto* w : works) {
    if (!w->search) continue;
    for (std::size_t h = 0; h < w->hyps.size(); ++h)
      for (auto& g : w->grid_ids) {
        auto n = w->gn[g];
        if (n && *n != w->hyps[h].nominal) continue;
        if (!by_grid.count(g)) grid_order.push_back(g);
        by_grid[g].push_back({w, h});
      }
  }
  for (auto& g : grid_order) {
    if (store::coverage_intervals(g).empty()) continue;
    auto& refs = by_grid[g];
    std::vector<const ENFTrace*> traces;
    for (auto& r : refs) traces.push_back(&r.w->hyps[r.h]);
    auto results = match::search_grid(traces, g, so);
    for (std::size_t i = 0; i < refs.size(); ++i) {
      auto& res = results[i];
      if (!res.n_lags) continue;  // too few valid frames left after filtering
      res.nominal = refs[i].w->hyps[refs[i].h].nominal;
      res.trace_index = static_cast<int>(refs[i].h);
      for (auto& c : res.candidates) c.origin = static_cast<int>(refs[i].h);
      refs[i].w->per_hyp[refs[i].h].push_back(std::move(res));
    }
  }
}

Verdict overall_verdict(const std::vector<SegmentReport>& segments) {
  std::vector<const SegmentReport*> searched;
  for (auto& s : segments)
    if (!s.match.results.empty()) searched.push_back(&s);
  if (searched.empty()) return Verdict{match::Level::None, {}, {}, {"no segment had usable hum and was long enough to locate"}, NAN};
  auto pf = [](const SegmentReport* s) {
    double p = s->match.verdict->p_false;
    return std::isfinite(p) ? p : 1.0;
  };
  const SegmentReport* best = searched[0];
  for (auto* s : searched)
    if (pf(s) < pf(best)) best = s;
  const Verdict& v = *best->match.verdict;
  const double p = std::isfinite(v.p_false) ? 1 - std::pow(1 - v.p_false, double(searched.size())) : v.p_false;
  const Candidate* c = best->match.results.empty() ? nullptr : best->match.results[0].best();
  match::Level level = c ? match::confidence_level(p, c->r) : match::Level::None;
  std::string where = "segment " + std::to_string(best->index + 1) + " (" + clock(best->start_s) + "-" + clock(best->end_s) + " in the file)";
  std::vector<std::string> reasons{where + ": " + (v.reasons.empty() ? std::string("no match") : v.reasons[0])};
  for (std::size_t i = 1; i < v.reasons.size(); ++i) reasons.push_back(v.reasons[i]);
  if (std::isfinite(p) && searched.size() > 1)
    reasons.push_back("best of " + std::to_string(searched.size()) + " segments searched: chance of a wrong match this good somewhere is " +
                      pyf(p, "%.1e"));
  int located = 0;
  for (auto& s : segments) located += s.match.verdict && match::located(s.match.verdict->level);
  reasons.push_back(std::to_string(located) + " of " + std::to_string(segments.size()) + " segments located with confidence");
  Verdict out{level, {}, {}, reasons, p};
  if (level != match::Level::None) {
    out.grid = best->match.results[0].grid;
    out.candidate = *c;
  }
  return out;
}

// The device's UTC offset from its metadata clock against the hum time (see tzclue.hpp).
// In an edited file the matched clip's own start is moved back to where the file starts.
void add_time_zone(Report& rep) {
  const auto& v = rep.verdict;
  if (!(v && v->candidate && v->grid)) {
    rep.tz = tzclue::infer(rep.clues, std::nullopt, NAN, rep.duration_s);
    return;
  }
  double clip_start = 0;
  for (auto& sg : rep.segments)
    if (sg.match.verdict && sg.match.verdict->candidate && sg.match.verdict->candidate->start_utc == v->candidate->start_utc) clip_start = sg.start_s;
  rep.tz = tzclue::infer(rep.clues, v->grid, v->candidate->start_utc - clip_start, rep.duration_s);
}

std::vector<std::string> consistency_checks(const Report& rep) {
  std::vector<std::string> out;
  const auto& v = rep.verdict;
  if (!(v && v->candidate)) return out;
  char b[512];
  for (auto& e : rep.tz.evidence) {
    if (!std::isfinite(e.residual_s)) continue;  // not compared with the hum
    const char* tag = e.used ? "CLUE" : e.offset_s ? "OK" : "MISMATCH";
    out.push_back(std::string(tag) + ": metadata " + e.key + " (" + e.raw + ") " + e.note);
  }
  if (rep.clues.gps) {
    auto [lat, lon] = *rep.clues.gps;
    const std::string day = grids::day_of(v->candidate->start_utc);
    auto hits = grids::grids_at(lat, lon, day);
    const auto& table = grids::load_grids();
    auto name = [&](const std::optional<std::string>& id) {
      if (!id) return std::string("None");
      auto* g = table.find(*id);
      return g ? g->name : *id;
    };
    auto it = std::find_if(hits.begin(), hits.end(), [&](auto& h) { return h.grid == v->grid; });
    if (it != hits.end()) {
      std::string near;
      if (it->distance_km) {
        std::snprintf(b, sizeof b, " (within %.0f km of its coast/edge)", it->distance_km);
        near = b;
      }
      std::snprintf(b, sizeof b, "OK: metadata GPS %.4f,%.4f lies inside %s as of %s%s", lat, lon, name(v->grid).c_str(), day.c_str(),
                    near.c_str());
    } else if (!hits.empty()) {
      std::vector<std::string> names;
      for (auto& h : hits) names.push_back(name(h.grid));
      std::snprintf(b, sizeof b,
                    "MISMATCH: metadata GPS %.4f,%.4f lies in %s as of %s, but the hum matched %s (edited metadata, or GPS from another place)",
                    lat, lon, join(names, ", ").c_str(), day.c_str(), name(v->grid).c_str());
    } else {
      std::snprintf(b, sizeof b, "MISMATCH: metadata GPS %.4f,%.4f lies outside every grid we know as of %s, but the hum matched %s", lat,
                    lon, day.c_str(), name(v->grid).c_str());
    }
    out.emplace_back(b);
  }
  return out;
}

}  // namespace

// ---------------------------------------------------------------- aircraft

// Time hypotheses from the hum for the aircraft search, then the hum re-searched around
// the aircraft's answer when the hum alone did not locate.
static void add_aircraft(Report& rep, const AnalyzeOptions& o) {
  using AM = AnalyzeOptions::Aircraft;
  if (o.aircraft == AM::Off) return;
  // A server can fetch the ADS-B day a recording needs (GEOLOC_AIRPLANE_FETCH, at most
  // GEOLOC_AIRPLANE_MAX_FETCH days per analysis, default 1).
  const bool fetch = o.aircraft_fetch || !env("GEOLOC_AIRPLANE_FETCH").empty();
  if (o.aircraft == AM::Auto && air::stored_days().empty() && !fetch && !air::bucket::enabled()) return;
  std::vector<air::Hypothesis> hyps;
  const bool located = rep.verdict && match::located(rep.verdict->level) && rep.verdict->candidate;
  if (located) {
    const auto& c = *rep.verdict->candidate;
    hyps.push_back({std::string("hum match (") + match::level_name(rep.verdict->level) + ")", c.grid, c.start_utc - 3, c.start_utc + 3, 1, {}});
  } else {
    std::vector<const match::Candidate*> cs;
    for (auto& g : rep.results)
      for (std::size_t i = 0; i < std::min<std::size_t>(3, g.candidates.size()); ++i)
        if (g.candidates[i].consistent()) cs.push_back(&g.candidates[i]);
    std::sort(cs.begin(), cs.end(), [](auto* a, auto* b) { return a->score > b->score; });
    for (std::size_t i = 0; i < std::min<std::size_t>(5, cs.size()); ++i)
      hyps.push_back({"hum candidate #" + std::to_string(i + 1) + " (r = " + pyf(cs[i]->r, "%.2f") + ")", cs[i]->grid, cs[i]->start_utc - 3,
                      cs[i]->start_utc + 3, 2, {}});
    if (auto a = parse_time(o.from), b = parse_time(o.to); a && b) hyps.push_back({"time window", std::nullopt, double(*a), double(*b), 2, {}});
  }
  air::AircraftOptions ao;
  if (o.aircraft_box) ao.box = air::Bbox{(*o.aircraft_box)[0], (*o.aircraft_box)[1], (*o.aircraft_box)[2], (*o.aircraft_box)[3]};
  ao.fetch = fetch;
  if (!o.aircraft_fetch && fetch) ao.max_days = std::max(1, std::atoi(env("GEOLOC_AIRPLANE_MAX_FETCH", "1").c_str()));
  rep.aircraft = air::run_aircraft(rep.path, hyps, ao);
  if (hyps.empty()) rep.aircraft->notes.push_back("no time to search at: no hum candidate and no date window");
  const auto* r = rep.aircraft->best_result();
  if (located || !r || r->level < air::AirLevel::Moderate || !rep.trace || r->candidates.empty()) return;
  // The hum, where the aircraft put the recording: only +-6 h of one grid compete now.
  const auto& c = r->candidates[0];
  for (auto& p : grids::grids_at(c.lat, c.lon, grids::day_of(c.start_utc), 15.0)) {
    if (!p.grid) continue;
    Report::HumCheck hc;
    hc.grid = *p.grid, hc.aircraft_start = c.start_utc, hc.window_s = 6 * 3600;
    if (!grids::load_grids().get(*p.grid).tracked()) {
      hc.note = "no reference data for this grid";
      rep.hum_checks.push_back(hc);
      continue;
    }
    match::SearchOptions so;
    so.from = i64(c.start_utc - hc.window_s), so.to = i64(c.start_utc + hc.window_s), so.top_k = 30;
    auto gr = match::search_grid(*rep.trace, *p.grid, so);
    if (gr.n_lags == 0) hc.note = gr.note.empty() ? "no reference data around this time" : gr.note;
    for (std::size_t i = 0; i < gr.candidates.size(); ++i)
      if (std::abs(gr.candidates[i].start_utc - c.start_utc) <= 3.0) {
        hc.rank = int(i), hc.candidate = gr.candidates[i];
        break;
      }
    rep.hum_checks.push_back(hc);
  }
}

// ---------------------------------------------------------------- analyze

Report analyze(const std::string& path, const AnalyzeOptions& o) {
  parse_time(o.from);  // validate before the expensive part
  parse_time(o.to);
  Report rep;
  rep.path = path;
  rep.clues = audio::file_clues(path);
  audio::Audio au;
  {
    au = audio::load_audio(path);
  }
  const double rate = au.rate;
  const auto max_hz = enf::nyquist_limit(au.source_rate);
  rep.duration_s = au.duration();
  enf::DetectOptions dop;
  dop.max_hz = max_hz;
  std::optional<enf::Spectrum> whole;
  auto whole_spec = [&]() -> const enf::Spectrum& {
    if (!whole) whole = enf::Spectrum::of(au.samples, rate);
    return *whole;
  };
  {
    std::cerr << "locate: audio decoded (" << rep.duration_s << " s), detecting hum" << std::endl;
    rep.hum = enf::detect_hum(au.samples, rate, &whole_spec(), dop);
  }
  if (o.split && (o.nominal || rep.hum.nominal)) {
    rep.cuts = cuts::clean_cuts(rep.duration_s, cuts::detect_cuts(au.samples, rate, rep.hum));
  }

  if (rep.cuts.empty()) {
    Work w = prepare(whole_spec(), rep.hum, o);
    std::vector<Work*> ws{&w};
    std::cerr << "locate: searching the hum" << std::endl;
    search_all(ws, o);
    finish(w, o);
    rep.hum = w.m.hum;
    rep.trace = w.m.trace;
    rep.results = w.m.results;
    rep.verdict = w.m.verdict;
    rep.candidate_regions = w.m.candidate_regions;
    rep.warnings.insert(rep.warnings.end(), w.m.warnings.begin(), w.m.warnings.end());
    rep.segments.push_back({0, 0.0, rep.duration_s, std::move(w.m)});
    add_time_zone(rep);
    rep.consistency = consistency_checks(rep);
    std::cerr << "locate: hum done" << std::endl;
    add_aircraft(rep, o);
    return rep;
  }

  whole.reset();  // segments get their own spectra
  auto segs = cuts::segments_from_cuts(rep.duration_s, rep.cuts);
  std::vector<Work> works(segs.size());
  for (std::size_t i = 0; i < segs.size(); ++i) {
    const auto& seg = segs[i];
    const auto a = static_cast<std::size_t>(seg.start_s * rate), b = std::min(au.samples.size(), static_cast<std::size_t>(seg.end_s * rate));
    std::span<const double> x(au.samples.data() + a, b - a);
    auto spec = enf::Spectrum::of(x, rate);
    auto sh = enf::detect_hum(x, rate, &spec, dop);
    // The whole file already shows hum: a segment only needs usable harmonics.
    if (!sh.nominal && rep.hum.nominal &&
        std::any_of(sh.harmonics.begin(), sh.harmonics.end(), [&](auto& h) { return h.nominal == *rep.hum.nominal && h.snr_db >= sh.min_snr_db; }))
      sh.nominal = rep.hum.nominal;
    if (seg.duration_s() < MIN_LOCATE_S) {
      works[i].m.hum = std::move(sh);
      works[i].m.verdict = Verdict{match::Level::None, {}, {}, {"segment is " + pyf(seg.duration_s(), "%.0f") + " s long: too short to locate on its own"}, NAN};
    } else {
      works[i] = prepare(spec, std::move(sh), o);
    }
  }
  std::vector<Work*> ws;
  for (auto& w : works) ws.push_back(&w);
  search_all(ws, o);
  for (std::size_t i = 0; i < segs.size(); ++i) {
    finish(works[i], o);
    rep.segments.push_back({int(i), segs[i].start_s, segs[i].end_s, std::move(works[i].m)});
  }
  rep.verdict = overall_verdict(rep.segments);
  const int nom = o.nominal ? *o.nominal : rep.hum.nominal.value_or(0);
  for (auto& g : grids::load_grids().grids)
    if (g.nominal_hz == nom) rep.candidate_regions.push_back(region(g));
  rep.warnings.push_back(std::to_string(rep.cuts.size()) + " edit point(s) found in the hum: the file joins " + std::to_string(rep.segments.size()) +
                         " separately recorded segments, each located on its own. Metadata times describe the edited file, not the clips.");
  add_time_zone(rep);
  rep.consistency = consistency_checks(rep);
  if (o.aircraft == AnalyzeOptions::Aircraft::On)
    rep.warnings.push_back("aircraft search skipped: an edited file has no single clock for its flyovers");
  return rep;
}

// ---------------------------------------------------------------- rendering

static std::string usable_list(const HumDetection& h) {
  std::string s;
  for (auto& u : h.usable()) {
    if (!s.empty()) s += ", ";
    s += pyf(u.centre_hz, "%.0f") + " Hz (" + pyf(u.snr_db, "%.0f") + " dB)";
  }
  return s;
}

std::string render_text(const Report& rep) {
  std::vector<std::string> L;
  char b[1024];
  const auto& table = grids::load_grids();
  std::snprintf(b, sizeof b, "File: %s  (%.1f s)", rep.path.c_str(), rep.duration_s);
  L.emplace_back(b);
  L.emplace_back("");
  L.emplace_back("== File metadata");
  const auto& c = rep.clues;
  if (c.gps) {
    std::snprintf(b, sizeof b, "  GPS in metadata: %.5f, %.5f  <-- check https://www.openstreetmap.org/?mlat=%s&mlon=%s", c.gps->first,
                  c.gps->second, py_float(c.gps->first).c_str(), py_float(c.gps->second).c_str());
    L.emplace_back(b);
  } else {
    L.emplace_back("  GPS in metadata: none");
  }
  L.push_back("  Creation time:   " + c.creation_time.value_or("none"));
  if (!c.device.empty()) {
    std::string d;
    for (auto& [k, v] : c.device) d += (d.empty() ? "" : ", ") + k + "=" + v;
    L.push_back("  Device:          " + d);
  }
  L.emplace_back("");
  L.emplace_back("== Mains hum");
  const auto& h = rep.hum;
  std::snprintf(b, sizeof b, "  50 Hz family score: %.1f dB   60 Hz family score: %.1f dB", h.score_db.at(50), h.score_db.at(60));
  L.emplace_back(b);
  if (!h.ridge_log10p.empty()) {
    std::string s = "  Ridge test (wandering line vs. off-grid nulls): ";
    for (std::size_t i = 0; i < h.ridge_log10p.size(); ++i)
      s += (i ? "   " : "") + std::to_string(h.ridge_log10p[i].first) + " Hz p = " + pyf(std::pow(10.0, -h.ridge_log10p[i].second), "%.1g");
    L.push_back(s);
  }
  if (h.nominal) {
    L.push_back("  Detected: " + std::to_string(*h.nominal) + " Hz mains. Usable harmonics: " + usable_list(h));
    std::vector<std::string> names;
    for (auto& r : rep.candidate_regions) names.push_back(jstr(r, "name"));
    L.push_back("  => recorded on a " + std::to_string(*h.nominal) + " Hz grid: " + join(names, ", "));
  } else {
    L.emplace_back("  Detected: none");
  }
  if (rep.trace) {
    const auto& t = *rep.trace;
    double lo = INFINITY, hi = -INFINITY;
    for (double f : t.freq)
      if (std::isfinite(f)) {
        lo = std::min(lo, (f - t.nominal) * 1000);
        hi = std::max(hi, (f - t.nominal) * 1000);
      }
    L.push_back("  ENF trace: " + std::to_string(t.freq.size()) + " points, " + pct0(t.valid_fraction()) + " valid, range " +
                fmt_num(std::isfinite(lo) ? lo : NAN, "%+.1f") + ".." + fmt_num(std::isfinite(hi) ? hi : NAN, "%+.1f") + " mHz");
  }
  L.emplace_back("");
  if (!rep.results.empty()) {
    L.emplace_back("== Grid matches (score: log-likelihood ratio vs. a random time, from wobble shape + level; r: wobble correlation)");
    for (auto& g : rep.results) {
      if (!g.best() && !g.candidates.empty()) {  // every candidate failed the gain/mismatch check
        const auto& c = g.candidates[0];
        std::snprintf(b, sizeof b, "  %-22s no consistent match (best score=%6.1f r=%.3f: wobble gain %s, %s of frames off by >%.0f mHz)",
                      g.grid.c_str(), c.score, c.r, pyf(c.gain, "%.2f").c_str(), pct0(c.mismatch).c_str(), match::MISMATCH_HZ * 1000);
        L.emplace_back(b);
        continue;
      }
      if (!g.best()) {
        std::snprintf(b, sizeof b, "  %-22s no overlap (%s)", g.grid.c_str(), g.note.c_str());
        L.emplace_back(b);
        continue;
      }
      std::string nb = fmt_num(g.candidates.size() > 1 ? g.candidates[1].score : NAN, "%6.1f");
      std::snprintf(b, sizeof b, "  %-22s score=%6.1f (r=%.3f)  next-best=%s  start=%s  [%.0f days in %.1fs]", g.grid.c_str(), g.best()->score,
                    g.best()->r, nb.c_str(), g.best()->start_iso().c_str(), double(g.n_lags) / 86400, g.seconds);
      L.emplace_back(b);
    }
    const auto& top = rep.results[0];
    if (top.best() && top.candidates.size() > 1) {
      L.push_back("  Top times in " + top.grid + ":");
      for (std::size_t i = 0; i < std::min<std::size_t>(5, top.candidates.size()); ++i) {
        const auto& cd = top.candidates[i];
        L.push_back("    " + cd.start_iso() + "  score=" + pyf(cd.score, "%6.1f") + "  r=" + pyf(cd.r, "%.3f") + "  offset=" +
                    pyf(cd.offset_hz * 1000, "%+.1f") + " mHz  rmse=" + pyf(cd.rmse_hz * 1000, "%.1f") + " mHz  gain=" + pyf(cd.gain, "%.2f") +
                    "  off=" + pct0(cd.mismatch) + (cd.consistent() ? "" : "  (inconsistent)"));
      }
    }
    L.emplace_back("");
  }
  if (!rep.cuts.empty()) {
    L.push_back("== Edit points (" + std::to_string(rep.cuts.size()) + ", from jumps in the hum's phase)");
    for (auto& ct : rep.cuts) {
      std::string hs = "[";
      for (std::size_t i = 0; i < ct.harmonics.size(); ++i) hs += (i ? ", " : "") + std::to_string(ct.harmonics[i]);
      hs += "]";
      std::snprintf(b, sizeof b, "  %s (%.1f s)  phase z=%.1f  harmonics %s  noise floor %+.1f dB", clock(ct.time_s).c_str(), ct.time_s,
                    ct.phase_z, hs.c_str(), ct.floor_jump_db);
      L.emplace_back(b);
    }
    L.emplace_back("");
    L.emplace_back("== Segments (each located on its own)");
    for (auto& sg : rep.segments) {
      const auto& m = sg.match;
      std::string hum = usable_list(m.hum);
      if (hum.empty()) hum = "none";
      std::snprintf(b, sizeof b, "  %d. %s-%s (%.0f s)  hum: %s", sg.index + 1, clock(sg.start_s).c_str(), clock(sg.end_s).c_str(),
                    sg.duration_s(), hum.c_str());
      L.emplace_back(b);
      const auto& sv = m.verdict;
      if (sv && sv->level != match::Level::None && sv->candidate) {
        auto* g = table.find(sv->grid.value_or(""));
        std::string lvl = match::level_name(sv->level);
        for (auto& ch : lvl) ch = static_cast<char>(std::toupper(ch));
        L.push_back("     [" + lvl + "] " + (g ? g->name : sv->grid.value_or("")) + ", segment recorded from ~" + sv->candidate->start_iso() +
                    "  (r=" + pyf(sv->candidate->r, "%.3f") + ", p=" + pyf(sv->p_false, "%.1e") + ")");
      } else {
        L.push_back("     [NONE] " + (sv && !sv->reasons.empty() ? sv->reasons[0] : std::string("no match")));
      }
    }
    L.emplace_back("");
  }
  if (const auto& v = rep.verdict) {
    L.emplace_back("== Verdict");
    if (v->level != match::Level::None && v->candidate) {
      auto* g = table.find(v->grid.value_or(""));
      std::string lvl = match::level_name(v->level);
      for (auto& ch : lvl) ch = static_cast<char>(std::toupper(ch));
      std::string when = rep.cuts.empty() ? "recording started" : "best segment recorded from";
      L.push_back("  [" + lvl + "] " + (g ? g->name : v->grid.value_or("")) + ", " + when + " ~" + v->candidate->start_iso());
      if (g) {
        auto cs = g->countries_at(grids::day_of(v->candidate->start_utc));
        if (cs.empty()) cs = g->countries();
        L.push_back("  Region (as of that date): " + join(cs, ", "));
      }
    } else {
      L.emplace_back("  [NONE] no confident grid/time match");
    }
    for (auto& r : v->reasons) L.push_back("   - " + r);
  }
  if (!rep.consistency.empty()) {
    L.emplace_back("");
    L.emplace_back("== Metadata vs. hum");
    for (auto& cl : rep.consistency) L.push_back("  " + cl);
  }  if (rep.aircraft) {
    L.emplace_back("");
    std::string a = air::render_stage(*rep.aircraft);
    while (!a.empty() && a.back() == '\n') a.pop_back();
    L.push_back(a);
  }
  for (auto& hc : rep.hum_checks) {
    if (!hc.note.empty()) {
      L.push_back("  Hum in " + hc.grid + " around the aircraft's time: " + hc.note);
    } else if (hc.rank < 0) {
      L.push_back("  Hum in " + hc.grid + ": no alignment within 3 s of the aircraft's time among the best 30 of +-6 h");
    } else {
      L.push_back("  Hum in " + hc.grid + ": the aircraft's time is alignment #" + std::to_string(hc.rank + 1) + " of +-6 h (r = " +
                  pyf(hc.candidate->r, "%.3f") + ", " + pyf(hc.candidate->start_utc - hc.aircraft_start, "%+.1f") + " s)" +
                  (hc.rank == 0 ? ": the two signals agree" : ""));
    }
  }

  if (!rep.tz.evidence.empty()) {
    L.emplace_back("");
    L.emplace_back("== Time zone (the device clock's UTC offset)");
    for (auto& e : rep.tz.evidence) L.push_back(std::string("  ") + (e.used ? "* " : "  ") + e.key + " = " + e.raw + ": " + e.note);
    for (auto& n : rep.tz.notes) L.push_back("  => " + n);
    for (std::size_t i = 0; i < rep.tz.zones.size() && i < 12; ++i) {
      const auto& z = rep.tz.zones[i];
      std::snprintf(b, sizeof b, "     %-24s %9.0f km²  %s%s%s", z.tz.c_str(), z.km2, join(z.countries, ",").c_str(),
                    z.subdivisions.empty() ? "" : "  ", join(z.subdivisions, ",").c_str());
      L.emplace_back(b);
    }
  }
  for (auto& w : rep.warnings) L.push_back("  ! " + w);
  std::string out;
  for (std::size_t i = 0; i < L.size(); ++i) {
    if (i) out += '\n';
    out += L[i];
  }
  return out;
}

// ---------------------------------------------------------------- JSON

static json::value cand_json(const Candidate* c) {
  if (!c) return nullptr;
  json::object o;
  o["grid"] = c->grid;
  o["start_utc"] = num_or_null(c->start_utc);
  o["r"] = num_or_null(c->r);
  o["score"] = num_or_null(c->score);
  o["r_raw"] = num_or_null(c->r_raw);
  o["offset_hz"] = num_or_null(c->offset_hz);
  o["rmse_hz"] = num_or_null(c->rmse_hz);
  o["gain"] = num_or_null(c->gain);
  o["mismatch"] = num_or_null(c->mismatch);
  o["consistent"] = c->consistent();
  o["start_iso"] = c->start_iso();
  return o;
}

static json::object hum_json(const HumDetection& h) {
  json::object o;
  o["nominal_hz"] = h.nominal ? json::value(*h.nominal) : json::value(nullptr);
  json::object sc;
  for (auto& [k, v] : h.score_db) sc[std::to_string(k)] = num_or_null(v);
  o["score_db"] = std::move(sc);
  json::object rl;
  for (auto& [k, v] : h.ridge_log10p) rl[std::to_string(k)] = num_or_null(v);
  o["ridge_log10p"] = std::move(rl);
  json::array hs;
  for (auto& s : h.harmonics) {
    json::object x;
    x["nominal"] = s.nominal;
    x["harmonic"] = s.harmonic;
    x["centre_hz"] = s.centre_hz;
    x["peak_hz"] = s.peak_hz;
    x["snr_db"] = num_or_null(s.snr_db);
    x["shared"] = s.shared;
    hs.push_back(std::move(x));
  }
  o["harmonics"] = std::move(hs);
  return o;
}

static json::value enf_json(const std::optional<ENFTrace>& t, double offset_s = 0.0, std::size_t max_points = 0) {
  if (!t) return nullptr;
  const std::size_t step = max_points ? std::max<std::size_t>(1, t->times.size() / max_points) : 1;
  json::array ts, fs, hs;
  for (std::size_t i = 0; i < t->times.size(); i += step) {
    ts.emplace_back(t->times[i] + offset_s);
    fs.push_back(num_or_null(t->freq[i]));
  }
  for (int h : t->harmonics) hs.emplace_back(h);
  json::object o;
  o["times_s"] = std::move(ts);
  o["freq_hz"] = std::move(fs);
  o["harmonics"] = std::move(hs);
  o["valid_fraction"] = t->valid_fraction();
  return o;
}

static json::value iso_or_null(double t) { return std::isfinite(t) ? json::value(iso(t)) : json::value(nullptr); }

static json::array results_json(const std::vector<GridResult>& results) {
  json::array out;
  for (auto& g : results) {
    json::object o;
    o["grid"] = g.grid;
    o["nominal_hz"] = g.nominal ? json::value(*g.nominal) : json::value(nullptr);
    o["searched_from"] = iso_or_null(g.searched_from);
    o["searched_to"] = iso_or_null(g.searched_to);
    o["lags_searched"] = g.n_lags;
    o["seconds"] = g.seconds;
    o["coverage"] = g.coverage;
    o["margin"] = num_or_null(g.margin());
    json::array cs;
    for (std::size_t i = 0; i < std::min<std::size_t>(10, g.candidates.size()); ++i) cs.push_back(cand_json(&g.candidates[i]));
    o["candidates"] = std::move(cs);
    o["note"] = g.note;
    out.push_back(std::move(o));
  }
  return out;
}

static json::value verdict_json(const std::optional<Verdict>& v) {
  if (!v) return nullptr;
  json::object o;
  o["level"] = match::level_name(v->level);
  o["grid"] = v->grid ? json::value(*v->grid) : json::value(nullptr);
  o["candidate"] = v->candidate ? cand_json(&*v->candidate) : json::value(nullptr);
  json::array rs;
  for (auto& r : v->reasons) rs.emplace_back(r);
  o["reasons"] = std::move(rs);
  o["p_false"] = num_or_null(v->p_false);
  return o;
}

static json::array strings(const std::vector<std::string>& v) {
  json::array a;
  for (auto& s : v) a.emplace_back(s);
  return a;
}

static json::object tz_json(const tzclue::Result& r) {
  json::object o;
  json::array ev;
  for (auto& e : r.evidence) {
    json::object x;
    x["key"] = e.key;
    x["raw"] = e.raw;
    x["kind"] = tzclue::kind_name(e.kind);
    x["offset_s"] = e.offset_s ? json::value(*e.offset_s) : json::value(nullptr);
    x["residual_s"] = num_or_null(e.residual_s);
    x["used"] = e.used;
    x["note"] = e.note;
    ev.push_back(std::move(x));
  }
  o["evidence"] = std::move(ev);
  json::array offs, labels;
  for (int s : r.offsets) {
    offs.emplace_back(s);
    labels.emplace_back(tzclue::fmt_offset(s));
  }
  o["offsets_s"] = std::move(offs);
  o["offsets"] = std::move(labels);
  o["grid"] = r.grid ? json::value(*r.grid) : json::value(nullptr);
  o["at"] = std::isfinite(r.at) ? json::value(iso(r.at)) : json::value(nullptr);
  o["day"] = std::isfinite(r.at) ? json::value(grids::day_of(r.at)) : json::value(nullptr);
  json::array zs;
  for (auto& z : r.zones) {
    json::object x;
    x["tz"] = z.tz;
    x["km2"] = z.km2;
    x["countries"] = strings(z.countries);
    x["subdivisions"] = strings(z.subdivisions);
    zs.push_back(std::move(x));
  }
  o["zones"] = std::move(zs);
  o["km2"] = r.km2;
  o["grid_km2"] = r.grid_km2;
  o["notes"] = strings(r.notes);
  return o;
}

static json::object report_json(const Report& rep, std::size_t max_points) {
  json::object o;
  o["path"] = rep.path;
  o["duration_s"] = rep.duration_s;
  json::object fc;
  const auto& c = rep.clues;
  fc["gps"] = c.gps ? json::value(json::array{c.gps->first, c.gps->second}) : json::value(nullptr);
  fc["gps_raw"] = c.gps_raw ? json::value(*c.gps_raw) : json::value(nullptr);
  fc["creation_time"] = c.creation_time ? json::value(*c.creation_time) : json::value(nullptr);
  json::object dev;
  for (auto& [k, v] : c.device) dev[k] = v;
  fc["device"] = std::move(dev);
  fc["duration_s"] = c.duration_s ? json::value(*c.duration_s) : json::value(nullptr);
  o["file_clues"] = std::move(fc);
  o["hum"] = hum_json(rep.hum);
  o["enf"] = enf_json(rep.trace, 0.0, max_points);
  o["candidate_regions"] = rep.candidate_regions;
  o["grid_results"] = results_json(rep.results);
  o["verdict"] = verdict_json(rep.verdict);
  json::array cs;
  for (auto& ct : rep.cuts) {
    json::object x;
    x["time_s"] = ct.time_s;
    x["phase_z"] = num_or_null(ct.phase_z);
    x["floor_jump_db"] = num_or_null(ct.floor_jump_db);
    json::array hs;
    for (int h : ct.harmonics) hs.emplace_back(h);
    x["harmonics"] = std::move(hs);
    cs.push_back(std::move(x));
  }
  o["cuts"] = std::move(cs);
  json::array segs;
  for (auto& sg : rep.segments) {
    json::object x;
    x["index"] = sg.index;
    x["start_s"] = sg.start_s;
    x["end_s"] = sg.end_s;
    x["hum"] = hum_json(sg.match.hum);
    x["enf"] = enf_json(sg.match.trace, sg.start_s, max_points);
    x["candidate_regions"] = sg.match.candidate_regions;
    x["grid_results"] = results_json(sg.match.results);
    x["verdict"] = verdict_json(sg.match.verdict);
    x["warnings"] = strings(sg.match.warnings);
    segs.push_back(std::move(x));
  }
  o["segments"] = std::move(segs);
  o["warnings"] = strings(rep.warnings);
  o["consistency"] = strings(rep.consistency);
  if (rep.aircraft) o["aircraft"] = air::stage_json(*rep.aircraft);
  if (!rep.hum_checks.empty()) {
    json::array hcs;
    for (auto& hc : rep.hum_checks)
      hcs.push_back(json::object{{"grid", hc.grid},
                                 {"aircraft_start", iso(hc.aircraft_start)},
                                 {"window_s", hc.window_s},
                                 {"rank", hc.rank},
                                 {"candidate", cand_json(hc.candidate ? &*hc.candidate : nullptr)},
                                 {"note", hc.note}});
    o["hum_checks"] = hcs;
  }
  o["timezone"] = tz_json(rep.tz);
  return o;
}

json::object to_json(const Report& rep) { return report_json(rep, 0); }

json::object to_web_json(const Report& rep) {
  auto o = report_json(rep, 600);
  o["grids"] = grid_table();
  return o;
}

json::array grid_table() {
  json::array out;
  const std::string now = grids::today();
  for (auto& g : grids::load_grids().grids) {
    auto iv = store::coverage_intervals(g.id);
    i64 months = 0;  // whole calendar months stored (intervals are month-aligned before the trim below)
    for (auto& x : iv) months += std::llround(double(x.b - x.a) / (30.436875 * 86400));
    if (!iv.empty()) {  // intervals end on month bounds; the current month is stored only up to now
      const i64 t1 = iv.back().b;
      if (auto end = store::last_data_end(g.id, t1, std::min<i64>(32 * 86400, t1 - iv.back().a))) iv.back().b = *end;
    }
    json::object o;
    o["id"] = g.id;
    o["name"] = g.name;
    o["nominal_hz"] = g.nominal_hz;
    json::array cs;
    for (auto& c : g.countries()) cs.emplace_back(c);
    o["countries"] = std::move(cs);
    o["tracked"] = g.tracked();
    auto st = g.status_at(now);
    o["status"] = st ? json::value(*st) : json::value(nullptr);
    o["refs_from"] = iv.empty() ? json::value(nullptr) : json::value(iso(double(iv.front().a)));
    o["refs_to"] = iv.empty() ? json::value(nullptr) : json::value(iso(double(iv.back().b)));
    // Per stored month, the fraction of its seconds that hold data, as [month start, fraction].
    // The month still being written is measured against the part of it that has passed.
    double covered = 0;  // seconds actually holding data, not just months stored
    json::array fill;
    const i64 last_end = iv.empty() ? 0 : iv.back().b;
    for (auto [t0, f] : store::month_fill(g.id)) {
      const i64 t1 = Month::of(t0).t1();
      covered += f * double(t1 - t0);
      const double shown = last_end > t0 && last_end < t1 ? std::min(1.0, f * double(t1 - t0) / double(last_end - t0)) : f;
      fill.push_back(json::array{json::value(t0), json::value(std::nearbyint(shown * 1e4) / 1e4)});
    }
    o["covered_s"] = std::llround(covered);
    o["months"] = months;
    o["fill"] = std::move(fill);
    json::array spans;  // stored stretches [from, to] in unix seconds; gaps between them are missing months
    for (auto& x : iv) spans.push_back(json::array{json::value(x.a), json::value(x.b)});
    o["spans"] = std::move(spans);
    json::array srcs;
    for (auto& s : refs::sources_covering(g.id)) srcs.emplace_back(s);
    o["sources"] = std::move(srcs);
    out.push_back(std::move(o));
  }
  return out;
}

}  // namespace geo::locate
