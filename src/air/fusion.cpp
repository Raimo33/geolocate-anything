#include "air/fusion.hpp"

#include <algorithm>
#include <cmath>
#include <iostream>
#include <set>

#include "audio.hpp"
#include "grids.hpp"
#include "util/time.hpp"

namespace geo::air {

namespace {

std::vector<std::string> days_of(double t0, double t1) {
  std::vector<std::string> out;
  for (i64 d = floor_div(i64(std::floor(t0)), 86400); d <= floor_div(i64(std::floor(t1)), 86400); ++d) out.push_back(iso_date(d * 86400));
  return out;
}

}  // namespace

AircraftStage run_aircraft(const std::string& path, const std::vector<Hypothesis>& hyps, const AircraftOptions& o) {
  AircraftStage st;
  auto au = audio::load_audio(path, SPEC_RATE);
  const auto spec = whitened_spectrogram(au.samples);
  au.samples.clear();
  au.samples.shrink_to_fit();
  st.detection = detect_events(spec);
  if (st.detection.events.empty()) {
    st.notes.push_back("no aircraft flyover detected");
    return st;
  }
  Terrain terrain(o.online);
  int fetched = 0;
  for (auto& h : hyps) {
    HypothesisResult hr{h, std::nullopt, {}};
    const double t0 = h.start_min - 600, t1 = h.start_max + spec.duration_s + 600;
    // ADS-B days needed; fetched when allowed.
    std::vector<std::string> missing;
    for (auto& d : days_of(h.start_min - 60, h.start_max + spec.duration_s + 60))
      if (!has_day(d)) missing.push_back(d);
    for (auto it = missing.begin(); it != missing.end() && o.fetch && fetched < o.max_days;) {
      try {
        std::cerr << "fetching ADS-B history for " << *it << std::endl;
        ingest_day(*it, {});
        ++fetched;
        it = missing.erase(it);
      } catch (const std::exception& e) {
        hr.note = "ADS-B history for " + *it + " unavailable: " + e.what();
        ++it;
      }
    }
    if (!missing.empty()) {
      if (hr.note.empty()) hr.note = "no ADS-B history stored for " + missing.front() + " (`geoloc aircraft fetch " + missing.front() + "`)";
      st.tried.push_back(std::move(hr));
      continue;
    }
    std::optional<Bbox> box = h.box ? h.box : o.box;
    const std::string day = iso_date(i64(h.start_min));
    if (!box && h.grid) {
      if (auto ext = grids::grid_extent(*h.grid, day)) box = Bbox{(*ext)[0], (*ext)[1], (*ext)[2], (*ext)[3]};
    }
    if (!box) {
      hr.note = "no search area (give one, or a hum match to take the grid's)";
      st.tried.push_back(std::move(hr));
      continue;
    }
    SearchOptions so;
    so.start_min = h.start_min, so.start_max = h.start_max, so.box = *box, so.min_events = h.min_events;
    auto r = search_regions(spec, st.detection.events, [&](const Bbox& b) { return split_tracks(load_tracks(t0, t1, b)); }, terrain, so);
    if (h.grid) {  // the grid's extent is a box: keep places inside the grid itself
      std::vector<Candidate> kept;
      for (auto& c : r.candidates) {
        auto pl = grids::grids_at(c.lat, c.lon, day, 15.0);
        if (std::any_of(pl.begin(), pl.end(), [&](auto& p) { return p.grid == *h.grid; })) kept.push_back(std::move(c));
      }
      r.candidates = std::move(kept);
      judge(r);
    }
    hr.result = std::move(r);
    st.tried.push_back(std::move(hr));
  }
  // The best: highest level, then score.
  for (std::size_t i = 0; i < st.tried.size(); ++i) {
    const auto& r = st.tried[i].result;
    if (!r || r->candidates.empty()) continue;
    if (st.best < 0) {
      st.best = int(i);
      continue;
    }
    const auto& b = *st.tried[std::size_t(st.best)].result;
    if (r->level > b.level || (r->level == b.level && r->candidates[0].score > b.candidates[0].score)) st.best = int(i);
  }
  return st;
}

json::object stage_json(const AircraftStage& s) {
  json::array ev, tried, notes;
  for (auto& e : s.detection.events) ev.push_back(event_json(e));
  for (auto& t : s.tried) {
    json::object o{{"label", t.h.label},
                   {"grid", t.h.grid ? json::value(*t.h.grid) : json::value(nullptr)},
                   {"start_from", iso(t.h.start_min)},
                   {"start_to", iso(t.h.start_max)},
                   {"min_events", t.h.min_events}};
    if (t.result) o["result"] = result_json(*t.result, s.detection.events);
    if (!t.note.empty()) o["note"] = t.note;
    tried.push_back(o);
  }
  for (auto& n : s.notes) notes.push_back(json::value(n));
  json::object out{{"events", ev}, {"hypotheses", tried}, {"notes", notes}};
  if (auto* r = s.best_result(); r && !r->candidates.empty()) {
    const auto& c = r->candidates[0];
    out["best"] = json::object{{"hypothesis", s.best}, {"level", air_level_name(r->level)}, {"lat", c.lat}, {"lon", c.lon}, {"start_utc", iso(c.start_utc)}};
  }
  return out;
}

std::string render_stage(const AircraftStage& s) {
  std::string out;
  char b[400];
  std::snprintf(b, sizeof b, "Aircraft: %zu flyover(s) detected\n", s.detection.events.size());
  out += b;
  for (auto& n : s.notes) out += "  " + n + "\n";
  for (std::size_t i = 0; i < s.tried.size(); ++i) {
    auto& t = s.tried[i];
    std::snprintf(b, sizeof b, "  %s %s: start %s .. %s%s\n", int(i) == s.best ? "*" : "-", t.h.label.c_str(), iso(t.h.start_min).c_str(),
                  iso(t.h.start_max).c_str(), t.h.grid ? (" in " + *t.h.grid).c_str() : "");
    out += b;
    if (!t.note.empty()) out += "      " + t.note + "\n";
    if (t.result) {
      if (t.result->candidates.empty()) {
        out += "      no matching place\n";
        continue;
      }
      const auto& c = t.result->candidates[0];
      std::snprintf(b, sizeof b, "      %s: %.5f, %.5f, start %s (%zu flyover(s), score %.1f)\n", air_level_name(t.result->level), c.lat, c.lon,
                    iso(c.start_utc).c_str(), c.matches.size(), c.score);
      out += b;
    }
  }
  if (auto* r = s.best_result()) out += render_result(*r, s.detection.events);
  return out;
}

}  // namespace geo::air
