#include "air/fusion.hpp"

#include <algorithm>
#include <cmath>
#include <iostream>
#include <set>

#include "air/adsb.hpp"

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

std::string base64(std::string_view in) {
  static const char* A = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  std::string out;
  out.reserve((in.size() + 2) / 3 * 4);
  for (std::size_t i = 0; i < in.size(); i += 3) {
    const std::uint32_t n = std::uint32_t(static_cast<unsigned char>(in[i])) << 16 |
                            (i + 1 < in.size() ? std::uint32_t(static_cast<unsigned char>(in[i + 1])) << 8 : 0u) |
                            (i + 2 < in.size() ? std::uint32_t(static_cast<unsigned char>(in[i + 2])) : 0u);
    out += A[n >> 18 & 63], out += A[n >> 12 & 63];
    out += i + 1 < in.size() ? A[n >> 6 & 63] : '=';
    out += i + 2 < in.size() ? A[n & 63] : '=';
  }
  return out;
}

const char* kind_name(char k) {
  switch (k) {
    case 'L': return "landplane";
    case 'S': return "seaplane";
    case 'A': return "amphibian";
    case 'H': return "helicopter";
    case 'G': return "gyrocopter";
    case 'T': return "tiltrotor";
    default: return "";
  }
}

// A flyover's spectrogram around its closest approach, the band covering every line's
// Doppler sweep; columns keep the strongest of the frames they cover.
SpectrumView crop(const Spectrogram& s, const Event& e, int index) {
  SpectrumView v;
  v.event = index;
  const double hw = e.half_window_s + 8;
  v.t0 = std::max(0.0, e.tc - hw), v.t1 = std::min(s.duration_s, e.tc + hw);
  double lo = INFINITY, hi = 0;
  for (auto& l : e.lines) lo = std::min(lo, l.f0), hi = std::max(hi, l.f0);
  const double vmax = std::min(e.v_hi + 15, SOUND_SPEED * 0.6);
  v.f_lo = std::max(s.freq(0), lo * SOUND_SPEED / (SOUND_SPEED + vmax) * 0.95);
  v.f_hi = std::min(s.fmax(), hi * SOUND_SPEED / (SOUND_SPEED - vmax) * 1.05);
  const int j0 = std::max(0, int(std::ceil(s.frame_at(v.t0)))), j1 = std::min(s.nf - 1, int(std::floor(s.frame_at(v.t1))));
  if (j1 <= j0 || v.f_hi <= v.f_lo) return v;
  v.w = std::min(480, j1 - j0 + 1), v.h = std::clamp(int((v.f_hi - v.f_lo) / s.df * 1.5), 120, 240);
  std::string px(std::size_t(v.w) * std::size_t(v.h), '\0');
  const double fh = (v.f_hi - v.f_lo) / v.h;
  for (int x = 0; x < v.w; ++x) {
    const int ja = j0 + int(std::floor(double(x) * (j1 - j0 + 1) / v.w)), jb = std::max(ja, j0 + int(std::floor(double(x + 1) * (j1 - j0 + 1) / v.w)) - 1);
    for (int y = 0; y < v.h; ++y) {
      const double f = v.f_hi - (y + 0.5) * fh;
      float z = -INFINITY;
      for (int j = ja; j <= jb; ++j)
        for (double df : {-0.33 * fh, 0.0, 0.33 * fh}) {
          const float q = s.at(j, f + df);
          if (std::isfinite(q)) z = std::max(z, q);
        }
      px[std::size_t(y) * std::size_t(v.w) + std::size_t(x)] = char(static_cast<unsigned char>(std::lround(std::clamp((z - 0.5) / 6.5, 0.0, 1.0) * 255)));
    }
  }
  v.pixels = base64(px);
  for (auto& l : e.lines) {  // the straight-pass measurement
    std::vector<std::array<float, 2>> c;
    for (double t = std::max(v.t0, e.tc - e.half_window_s); t <= std::min(v.t1, e.tc + e.half_window_s); t += 0.25)
      c.push_back({float(t), float(l.f0 * cv_ratio(t, e.tc, e.v, e.d))});
    v.straight.push_back(std::move(c));
  }
  return v;
}

// Tracks, details and curves of the best answer's aircraft.
void add_views(AircraftStage& st, const Spectrogram& spec, Terrain& terrain) {
  const auto& events = st.detection.events;
  for (std::size_t i = 0; i < events.size() && i < 12; ++i) st.spectra.push_back(crop(spec, events[i], int(i)));
  const SearchResult* r = st.best_result();
  if (!r || r->candidates.empty()) return;
  const Candidate& c = r->candidates[0];
  st.start_utc = c.start_utc;
  const Vec3 o = ecef(c.lat, c.lon, c.ground_m + 2.0);
  std::set<std::uint32_t> ids;
  for (auto& m : c.matches) ids.insert(std::uint32_t(std::strtoul(m.flight.c_str(), nullptr, 16)));
  const auto reg = registry_lookup(ids);
  for (auto& m : c.matches) {
    FlightView fv;
    fv.event = m.event, fv.flight = m.flight, fv.reg = m.reg, fv.type = m.type;
    fv.cpa_m = m.cpa_m, fv.speed = m.speed, fv.score = m.score, fv.fit = m.fit, fv.f0 = m.f0, fv.cpa_utc = m.cpa_utc;
    const auto icao = std::uint32_t(std::strtoul(m.flight.c_str(), nullptr, 16));
    if (auto it = reg.find(icao); it != reg.end()) {
      fv.desc = it->second.desc, fv.owner = it->second.owner, fv.year = it->second.year;
      if (fv.reg.empty()) fv.reg = it->second.reg;
      if (fv.type.empty()) fv.type = it->second.type;
    }
    if (auto ti = TypeDb::get().find(fv.type)) fv.kind = kind_name(ti->kind), fv.engine = engine_name(ti->engine), fv.engines = ti->engines;
    // The segment holding the closest approach, its track, and the curve it predicts.
    const Bbox near{c.lon - 0.6, c.lat - 0.45, c.lon + 0.6, c.lat + 0.45};
    for (auto& tr : split_tracks(load_tracks(m.cpa_utc - 900, m.cpa_utc + 900, near))) {
      if (tr.icao != icao || tr.fixes.front().t > m.cpa_utc || tr.fixes.back().t < m.cpa_utc) continue;
      double last = -INFINITY;
      for (auto& f : tr.fixes)
        if (std::abs(f.t - m.cpa_utc) <= 600 && f.t - last >= 4) fv.track.push_back({f.t, f.lat, f.lon, double(f.alt_m)}), last = f.t;
      const Path path = path_of(tr);
      if (path.empty()) break;
      const Approach ap = closest_approach(path, o);
      Vec3 p, vel;
      path.state(ap.tau, p, vel);
      const Geodetic g = geodetic(p);
      const Vec3 ve = Enu(g.lat, g.lon).rot(vel);
      fv.cpa_lat = g.lat, fv.cpa_lon = g.lon, fv.cpa_alt_m = g.h;
      fv.cpa_track_deg = std::fmod(std::atan2(ve.x, ve.y) / DEG + 360.0, 360.0);
      if (m.event >= 0 && m.event < int(st.spectra.size()) && m.event < int(events.size())) {
        SpectrumView& sv = st.spectra[std::size_t(m.event)];
        const Event& e = events[std::size_t(m.event)];
        sv.matched = true;
        const double scale = e.lines.empty() ? 1.0 : m.f0 / e.lines[0].f0;
        for (auto& l : e.lines) {
          std::vector<std::array<float, 2>> cv;
          double guess = NAN;
          for (double t = sv.t0; t <= sv.t1; t += 0.25)
            if (auto em = emission_for(path, o, c.start_utc + t, SOUND_SPEED, guess))
              cv.push_back({float(t), float(l.f0 * scale * em->ratio())}), guess = em->tau;
          sv.model.push_back(std::move(cv));
        }
      }
      break;
    }
    st.flights.push_back(std::move(fv));
  }
}

}  // namespace

AircraftStage run_aircraft(const std::string& path, const std::vector<Hypothesis>& hyps, const AircraftOptions& o) {
  AircraftStage st;
  std::cerr << "aircraft: decoding at 4 kHz" << std::endl;
  auto au = audio::load_audio(path, SPEC_RATE);
  std::cerr << "aircraft: spectrogram" << std::endl;
  const auto spec = whitened_spectrogram(au.samples);
  au.samples.clear();
  au.samples.shrink_to_fit();
  std::cerr << "aircraft: detecting flyovers" << std::endl;
  st.detection = detect_events(spec);
  std::cerr << "aircraft: " << st.detection.events.size() << " flyover(s)" << std::endl;
  if (st.detection.events.empty()) {
    st.notes.push_back("no aircraft flyover detected");
    return st;
  }
  if (hyps.empty()) {
    Terrain none(false);
    add_views(st, spec, none);  // the flyovers' spectrograms still show
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
    std::cerr << "aircraft: " << h.label << std::endl;
    auto r = search_regions(spec, st.detection.events, [&](const Bbox& b) { return split_tracks(load_tracks(t0, t1, b)); }, terrain, so, 3.0,
                            [](const std::string& m) { std::cerr << "  " << m << std::endl; });
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
  add_views(st, spec, terrain);
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
  json::array fl, sp;
  for (auto& f : s.flights) {
    json::array tr;
    for (auto& p : f.track) tr.push_back(json::array{p[0], std::round(p[1] * 1e5) / 1e5, std::round(p[2] * 1e5) / 1e5, std::round(p[3])});
    fl.push_back(json::object{{"event", f.event},         {"flight", f.flight},     {"registration", f.reg},   {"type", f.type},
                              {"description", f.desc},    {"owner", f.owner},       {"year", f.year},          {"kind", f.kind},
                              {"engine", f.engine},       {"engines", f.engines},   {"cpa_m", f.cpa_m},        {"speed_ms", f.speed},
                              {"score", f.score},         {"fit", f.fit},           {"f0_hz", f.f0},           {"cpa_utc", iso(f.cpa_utc)},
                              {"cpa_epoch", f.cpa_utc},   {"cpa_lat", num_or_null(f.cpa_lat)}, {"cpa_lon", num_or_null(f.cpa_lon)},
                              {"cpa_alt_m", num_or_null(f.cpa_alt_m)}, {"cpa_track_deg", num_or_null(f.cpa_track_deg)}, {"track", tr}});
  }
  auto curves = [](const std::vector<std::vector<std::array<float, 2>>>& cs) {
    json::array a;
    for (auto& c : cs) {
      json::array pts;
      for (auto& p : c) pts.push_back(json::array{std::round(p[0] * 100) / 100, std::round(p[1] * 100) / 100});
      a.push_back(pts);
    }
    return a;
  };
  for (auto& v : s.spectra)
    sp.push_back(json::object{{"event", v.event}, {"matched", v.matched}, {"t0", v.t0},          {"t1", v.t1},
                              {"f_lo", v.f_lo},   {"f_hi", v.f_hi},       {"w", v.w},            {"h", v.h},
                              {"pixels", v.pixels}, {"model", curves(v.model)}, {"straight", curves(v.straight)}});
  out["flights"] = fl;
  out["spectra"] = sp;
  out["start_epoch"] = num_or_null(s.start_utc);
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
