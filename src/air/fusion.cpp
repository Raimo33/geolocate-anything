#include "air/fusion.hpp"

#include <algorithm>
#include <cmath>
#include <iostream>
#include <set>

#include "air/adsb.hpp"
#include "air/bucket.hpp"
#include "air/geodesy.hpp"

#include "audio.hpp"
#include "grids.hpp"
#include "util/parallel.hpp"
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
// `r`'s event indices are into `events`; spectrum times are from the spectrogram's start.
void add_views(std::vector<FlightView>& flights, std::vector<SpectrumView>& spectra, const Spectrogram& spec, const std::vector<Event>& events,
               const SearchResult* r) {
  const std::size_t first = spectra.size();
  for (std::size_t i = 0; i < events.size() && i < 12; ++i) spectra.push_back(crop(spec, events[i], int(i)));
  if (!r || r->candidates.empty()) return;
  const Candidate& c = r->candidates[0];
  for (std::size_t i = first; i < spectra.size(); ++i) spectra[i].utc0 = c.start_utc;
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
      const Path path = path_of(tr);
      if (path.empty()) break;
      // The stored nodes are sparse: the drawn track samples the curve every 4 s.
      for (double t = std::max(path.t0(), m.cpa_utc - 600); t <= std::min(path.t1(), m.cpa_utc + 600); t += 4) {
        Vec3 p, vel;
        path.state(t, p, vel);
        const Geodetic g = geodetic(p);
        fv.track.push_back({t, g.lat, g.lon, g.h});
      }
      const Approach ap = closest_approach(path, o);
      Vec3 p, vel;
      path.state(ap.tau, p, vel);
      const Geodetic g = geodetic(p);
      const Vec3 ve = Enu(g.lat, g.lon).rot(vel);
      fv.cpa_lat = g.lat, fv.cpa_lon = g.lon, fv.cpa_alt_m = g.h;
      fv.cpa_track_deg = std::fmod(std::atan2(ve.x, ve.y) / DEG + 360.0, 360.0);
      if (m.event >= 0 && first + std::size_t(m.event) < spectra.size() && m.event < int(events.size())) {
        SpectrumView& sv = spectra[first + std::size_t(m.event)];
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
    flights.push_back(std::move(fv));
  }
}


// Days of ADS-B history a hypothesis needs: stored (or in the ADS-B bucket), else
// fetched from ADSB.lol when allowed (and then kept in the bucket). Returns why it cannot
// be searched, empty when it can.
std::string ensure_days(const Hypothesis& h, double duration_s, const AircraftOptions& o, int& fetched) {
  std::string note;
  std::vector<std::string> missing;
  for (auto& d : days_of(h.start_min - 60, h.start_max + duration_s + 60))
    if (!ensure_day(d)) missing.push_back(d);
  for (auto it = missing.begin(); it != missing.end() && o.fetch && fetched < o.max_days;) {
    try {
      std::cerr << "fetching ADS-B history for " << *it << std::endl;
      ingest_day(*it, {});
      ++fetched;
      if (bucket::enabled()) try {
          bucket::push({*it});
        } catch (const std::exception& e) {
          std::cerr << "ADS-B bucket: " << e.what() << std::endl;
        }
      it = missing.erase(it);
    } catch (const std::exception& e) {
      note = "ADS-B history for " + *it + " unavailable: " + e.what();
      ++it;
    }
  }
  if (!missing.empty() && note.empty()) note = "no ADS-B history stored for " + missing.front() + " (`geoloc aircraft fetch " + missing.front() + "`)";
  return missing.empty() ? std::string() : note;
}

// The hypothesis' own box, else the run's, else its grid's extent.
std::optional<Bbox> box_for(const Hypothesis& h, const AircraftOptions& o) {
  if (h.box) return h.box;
  if (o.box) return o.box;
  if (h.grid)
    if (auto ext = grids::grid_extent(*h.grid, iso_date(i64(h.start_min)))) return Bbox{(*ext)[0], (*ext)[1], (*ext)[2], (*ext)[3]};
  return std::nullopt;
}

// One hypothesis searched over `box`; with a grid, places outside the grid itself go
// (the grid's extent is only its bounding box).
SearchResult search_hypothesis(const Spectrogram& spec, const std::vector<Event>& events, const Hypothesis& h, const Bbox& box, const SpanTrackSource& tracks,
                               Terrain& terrain, bool grid_filter, const std::function<void(const std::string&)>& progress) {
  SearchOptions so;
  so.start_min = h.start_min, so.start_max = h.start_max, so.box = box, so.min_events = h.min_events;
  const double t0 = h.start_min - 600, t1 = h.start_max + spec.duration_s + 600;
  auto r = search_regions(spec, events, [&](const Bbox& b) { return tracks(b, t0, t1); }, terrain, so, 3.0, progress);
  if (grid_filter && h.grid) {
    const std::string day = iso_date(i64(h.start_min));
    std::vector<Candidate> kept;
    for (auto& c : r.candidates) {
      auto pl = grids::grids_at(c.lat, c.lon, day, 15.0);
      if (std::any_of(pl.begin(), pl.end(), [&](auto& p) { return p.grid == *h.grid; })) kept.push_back(std::move(c));
    }
    r.candidates = std::move(kept);
    judge(r);
  }
  return r;
}

// The best search: highest level, then score.
int pick_best(const std::vector<HypothesisResult>& tried) {
  int best = -1;
  for (std::size_t i = 0; i < tried.size(); ++i) {
    const auto& r = tried[i].result;
    if (!r || r->candidates.empty()) continue;
    if (best < 0) {
      best = int(i);
      continue;
    }
    const auto& b = *tried[std::size_t(best)].result;
    if (r->level > b.level || (r->level == b.level && r->candidates[0].score > b.candidates[0].score)) best = int(i);
  }
  return best;
}

// A one-flyover answer reflected across its aircraft's track at closest approach: the
// curve a straight pass draws is the same on either side.
std::optional<std::pair<double, double>> mirror(const Candidate& c, const SpanTrackSource& tracks, double t0, double t1) {
  const auto& m = c.matches[0];
  const Bbox near{c.lon - 0.2, c.lat - 0.15, c.lon + 0.2, c.lat + 0.15};
  for (auto& tr : tracks(near, t0, t1)) {
    if (tr.id() != m.flight || tr.fixes.empty() || tr.fixes.front().t > m.cpa_utc || tr.fixes.back().t < m.cpa_utc) continue;
    const Path path = path_of(tr);
    if (path.empty()) return std::nullopt;
    const Enu f(c.lat, c.lon, c.ground_m);
    const Approach ap = closest_approach(path, f.o);
    Vec3 p, v;
    path.state(ap.tau, p, v);
    const Vec3 pe = f.rot(p - f.o), ve = f.rot(v);
    const double vh = std::hypot(ve.x, ve.y);
    if (vh < 1.0) return std::nullopt;
    // The listener (the origin) reflected across the horizontal line through pe along ve.
    const double ux = ve.x / vh, uy = ve.y / vh, s = -(pe.x * ux + pe.y * uy);
    const double fx = pe.x + s * ux, fy = pe.y + s * uy;  // foot of the perpendicular
    return std::pair{c.lat + 2 * fy / 111195.0, c.lon + 2 * fx / (111195.0 * std::cos(c.lat * DEG))};
  }
  return std::nullopt;
}

// The shared place refined jointly: one point, every matched clip's clock fitted there
// (its matched flights' exact curves, on ridges), the clips' scores summed. Each clip
// alone can trade its clock against position along its aircraft's track; together the
// tracks cross at one point.
void refine_shared(SharedPlace& sp, const std::vector<ClipInput>& clips, std::vector<ClipOutcome>& outs, const SpanTrackSource& tracks, Terrain& terrain) {
  struct Term {
    Event e;
    Path path;
  };
  struct ClipFit {
    std::size_t clip;
    std::vector<Term> terms;
    double a, b, start;  // clock window
  };
  std::vector<ClipFit> fits;
  for (std::size_t i = 0; i < clips.size(); ++i) {
    auto& oc = outs[i];
    if (!oc.at_place) continue;
    const Candidate& cd = oc.at_place->candidates[0];
    const Hypothesis& h = clips[i].hyps[std::size_t(oc.at_hyp)];
    ClipFit cf{i, {}, std::max(h.start_min, cd.start_utc - 1.5), std::min(h.start_max, cd.start_utc + 1.5), cd.start_utc};
    const Bbox near{cd.lon - 0.2, cd.lat - 0.15, cd.lon + 0.2, cd.lat + 0.15};
    const auto trs = tracks(near, h.start_min - 600, h.start_max + clips[i].spec->duration_s + 600);
    for (auto& m : cd.matches) {
      if (m.event < 0 || m.event >= int(clips[i].events.size())) continue;
      Event e = clips[i].events[std::size_t(m.event)];
      const double scale = e.lines.empty() || e.lines[0].f0 <= 0 ? 1.0 : m.f0 / e.lines[0].f0;
      for (auto& l : e.lines) l.f0 *= scale;
      for (auto& tr : trs)
        if (tr.id() == m.flight && !tr.fixes.empty() && tr.fixes.front().t <= m.cpa_utc && tr.fixes.back().t >= m.cpa_utc) {
          if (Path p = path_of(tr); !p.empty()) cf.terms.push_back({std::move(e), std::move(p)});
          break;
        }
    }
    if (!cf.terms.empty() && cf.b >= cf.a) fits.push_back(std::move(cf));
  }
  if (fits.empty()) return;
  struct Eval {
    double total = -INFINITY;
    std::vector<double> starts;
  };
  auto eval = [&](double lat, double lon) {
    Eval ev;
    ev.total = 0;
    const Vec3 o = ecef(lat, lon, terrain.ground(lat, lon) + 2.0);
    for (auto& cf : fits) {
      double best = -INFINITY, bs = cf.start;
      for (double t = cf.a; t <= cf.b + 1e-9; t += 0.064) {
        double sum = 0;
        for (auto& tm : cf.terms) sum += event_score(*clips[cf.clip].spec, tm.e, tm.path, o, t, SOUND_SPEED, 0.004, true).first;
        if (sum > best) best = sum, bs = t;
      }
      ev.total += best, ev.starts.push_back(bs);
    }
    return ev;
  };
  double lat = sp.lat, lon = sp.lon;
  Eval best = eval(lat, lon);
  for (double step : {50.0, 12.0}) {
    const int n = 4;
    std::vector<Eval> grid(std::size_t((2 * n + 1) * (2 * n + 1)));
    const double clat = lat, clon = lon, dl = step / 111195.0, dn = dl / std::cos(clat * DEG);
    parallel_for(grid.size(), [&](std::size_t k) {
      const int a = int(k) / (2 * n + 1) - n, b = int(k) % (2 * n + 1) - n;
      grid[k] = eval(clat + a * dl, clon + b * dn);
    });
    for (std::size_t k = 0; k < grid.size(); ++k)
      if (grid[k].total > best.total) {
        const int a = int(k) / (2 * n + 1) - n, b = int(k) % (2 * n + 1) - n;
        best = grid[k], lat = clat + a * dl, lon = clon + b * dn;
      }
  }
  sp.lat = lat, sp.lon = lon, sp.ground_m = terrain.ground(lat, lon);
  for (std::size_t k = 0; k < fits.size(); ++k) {
    auto& oc = outs[fits[k].clip];
    oc.start_utc = best.starts[k];
    Candidate& cd = oc.at_place->candidates[0];
    cd.lat = lat, cd.lon = lon, cd.ground_m = sp.ground_m, cd.start_utc = best.starts[k];
  }
}

const SpanTrackSource STORE_TRACKS = [](const Bbox& b, double t0, double t1) { return split_tracks(load_tracks(t0, t1, b)); };
void log_line(const std::string& m) { std::cerr << "  " << m << std::endl; }

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
    add_views(st.flights, st.spectra, spec, st.detection.events, nullptr);  // the flyovers' spectrograms still show
    return st;
  }
  Terrain terrain(o.online);
  int fetched = 0;
  for (auto& h : hyps) {
    HypothesisResult hr{h, std::nullopt, ensure_days(h, spec.duration_s, o, fetched)};
    if (!hr.note.empty()) {
      st.tried.push_back(std::move(hr));
      continue;
    }
    const auto box = box_for(h, o);
    if (!box) {
      hr.note = "no search area (give one, or a hum match to take the grid's)";
      st.tried.push_back(std::move(hr));
      continue;
    }
    std::cerr << "aircraft: " << h.label << std::endl;
    hr.result = search_hypothesis(spec, st.detection.events, h, *box, STORE_TRACKS, terrain, true, log_line);
    st.tried.push_back(std::move(hr));
  }
  st.best = pick_best(st.tried);
  add_views(st.flights, st.spectra, spec, st.detection.events, st.best_result());
  if (auto* r = st.best_result()) st.start_utc = r->candidates[0].start_utc;
  return st;
}

// ---------------------------------------------------------------- edited files

namespace {

void shift_events(SearchResult& r, int off) {
  for (auto& c : r.candidates)
    for (auto& m : c.matches) m.event += off;
  for (auto& i : r.events_used) i += off;
}

constexpr const char* NO_AREA = "no search area of its own: searched only around the place the other clips give";

}  // namespace

JointClips search_clips(const std::vector<ClipInput>& clips, const SpanTrackSource& tracks, Terrain& terrain,
                        const std::function<std::string(const Hypothesis&, double)>& unavailable, const std::function<void(const std::string&)>& progress) {
  JointClips out;
  out.clips.resize(clips.size());
  // 1. Each clip alone, on its own clock.
  std::vector<std::vector<bool>> ready(clips.size());  // hypothesis has ADS-B history
  for (std::size_t i = 0; i < clips.size(); ++i) {
    const ClipInput& c = clips[i];
    ClipOutcome& oc = out.clips[i];
    if (!c.spec || c.events.empty()) {
      if (oc.note.empty()) oc.note = "no aircraft flyover detected";
      continue;
    }
    for (auto& h : c.hyps) {
      HypothesisResult hr{h, std::nullopt, unavailable ? unavailable(h, c.spec->duration_s) : std::string()};
      ready[i].push_back(hr.note.empty());
      if (hr.note.empty() && !h.box) hr.note = NO_AREA;
      if (hr.note.empty()) {
        if (progress) progress("clip " + std::to_string(i + 1) + ": " + h.label);
        hr.result = search_hypothesis(*c.spec, c.events, h, *h.box, tracks, terrain, true, progress);
      }
      oc.tried.push_back(std::move(hr));
    }
    if (c.hyps.empty()) oc.note = "no time to search at: no hum match in this clip and no date window";
    oc.best = pick_best(oc.tried);
  }
  // 2. Places to try: each clip's best few answers, from any of its hypotheses. A single
  //    flyover heard alone leaves a strip on either side of the track; another clip's
  //    flyovers, searched near each point, decide which side.
  struct Pt {
    double lat, lon, score;
    int clip;
  };
  std::vector<Pt> pts;
  auto add = [&](const Pt& p) {
    if (std::none_of(pts.begin(), pts.end(), [&](auto& q) { return haversine_m(p.lat, p.lon, q.lat, q.lon) < 200.0; })) pts.push_back(p);
  };
  for (std::size_t i = 0; i < clips.size(); ++i) {
    std::vector<std::pair<const Candidate*, const Hypothesis*>> mine;
    for (auto& hr : out.clips[i].tried)
      if (hr.result)
        for (auto& c : hr.result->candidates) mine.emplace_back(&c, &hr.h);
    std::sort(mine.begin(), mine.end(), [](auto& x, auto& y) { return x.first->score > y.first->score; });
    if (mine.size() > std::size_t(CLIP_PLACES)) mine.resize(std::size_t(CLIP_PLACES));
    for (auto& [c, h] : mine) {
      add({c->lat, c->lon, c->score, int(i)});
      // One flyover: its mirror image across the aircraft's track fits as well.
      if (c->matches.size() == 1)
        if (auto m = mirror(*c, tracks, h->start_min - 600, h->start_max + clips[i].spec->duration_s + 600)) add({m->first, m->second, c->score, int(i)});
    }
  }
  if (pts.empty()) return out;
  // 3. At each place, every clip searched again within CLIP_AGREE_M, on its own clock;
  //    the place's evidence is the sum of the clips' best scores there.
  auto near_box = [](double lat, double lon) {
    const double dlat = CLIP_AGREE_M / 111195.0, dlon = dlat / std::max(0.05, std::cos(lat * DEG));
    return Bbox{lon - dlon, lat - dlat, lon + dlon, lat + dlat};
  };
  struct AtPlace {
    double total = 0;
    std::vector<std::optional<SearchResult>> r;
    std::vector<int> hyp;
  };
  std::vector<AtPlace> at(pts.size());
  for (std::size_t pi = 0; pi < pts.size(); ++pi) {
    const Pt& p = pts[pi];
    const Bbox near = near_box(p.lat, p.lon);
    at[pi].r.resize(clips.size()), at[pi].hyp.assign(clips.size(), -1);
    for (std::size_t i = 0; i < clips.size(); ++i) {
      const ClipInput& c = clips[i];
      for (std::size_t k = 0; k < c.hyps.size() && k < ready[i].size(); ++k) {
        if (!ready[i][k]) continue;
        Hypothesis h = c.hyps[k];
        h.box = near;
        auto r = search_hypothesis(*c.spec, c.events, h, near, tracks, terrain, false, {});
        std::erase_if(r.candidates, [&](const Candidate& cd) { return haversine_m(cd.lat, cd.lon, p.lat, p.lon) > CLIP_AGREE_M; });
        judge(r);
        if (r.candidates.empty()) continue;
        auto& cur = at[pi].r[i];
        if (!cur || r.candidates[0].score > cur->candidates[0].score) cur = std::move(r), at[pi].hyp[i] = int(k);
      }
      if (at[pi].r[i]) at[pi].total += std::max(0.0, at[pi].r[i]->candidates[0].score);
    }
  }
  std::size_t win = 0;
  for (std::size_t pi = 1; pi < pts.size(); ++pi)
    if (at[pi].total > at[win].total) win = pi;
  const Pt place = pts[win];
  // The runner-up place: the best at least CLIP_AGREE_M away.
  double second = 0;
  for (std::size_t pi = 0; pi < pts.size(); ++pi)
    if (haversine_m(pts[pi].lat, pts[pi].lon, place.lat, place.lon) > CLIP_AGREE_M) second = std::max(second, at[pi].total);
  SharedPlace sp;
  sp.anchor = place.clip;
  double wsum = 0, wlat = 0, wlon = 0, min_fit = INFINITY;
  int flyovers = 0;
  for (std::size_t i = 0; i < clips.size(); ++i) {
    const ClipInput& c = clips[i];
    ClipOutcome& oc = out.clips[i];
    oc.at_place = std::move(at[win].r[i]), oc.at_hyp = at[win].hyp[i];
    if (oc.at_place) {
      const Candidate& cd = oc.at_place->candidates[0];
      oc.start_utc = cd.start_utc;
      ++sp.clips_matched;
      for (auto& m : cd.matches) min_fit = std::min(min_fit, m.fit), ++flyovers;
      const double w = std::max(1e-3, cd.score);
      wsum += w, wlat += w * cd.lat, wlon += w * cd.lon;
      if (int(i) == sp.anchor || sp.ground_m == 0) sp.ground_m = cd.ground_m;
    } else if (oc.note.empty() && !c.events.empty()) {
      oc.note = "no aircraft heard from the shared place matches this clip's flyovers";
    }
    // Its own answer (searched alone) against the shared place.
    if (oc.best >= 0) {
      const Candidate& own = oc.tried[std::size_t(oc.best)].result->candidates[0];
      oc.alone_m = haversine_m(own.lat, own.lon, place.lat, place.lon);
      const AirLevel lv = oc.tried[std::size_t(oc.best)].result->level;
      if (oc.alone_m <= CLIP_AGREE_M) ++sp.clips_agreeing;
      if (oc.alone_m > CLIP_AGREE_M && lv >= AirLevel::Moderate) {
        sp.disagree = true;
        char b[200];
        std::snprintf(b, sizeof b, "clip %zu alone puts the recording %.1f km away (%s): the camera may have moved, or one answer is wrong", i + 1,
                      oc.alone_m / 1000, air_level_name(lv));
        sp.reasons.push_back(b);
      }
    }
  }
  sp.lat = wsum > 0 ? wlat / wsum : place.lat, sp.lon = wsum > 0 ? wlon / wsum : place.lon;
  refine_shared(sp, clips, out.clips, tracks, terrain);
  // Verdict as for one clock (judge): every flyover explained at the place, how well,
  // and the lead of this place's evidence over the best place elsewhere.
  const double total = at[win].total, lead = total > 0 ? (total - second) / total : 0;
  if (flyovers >= 2 && min_fit >= 0.7 && lead >= 0.2) sp.level = AirLevel::Strong;
  else if (flyovers >= 1 && min_fit >= 0.5 && lead >= 0.1) sp.level = AirLevel::Moderate;
  else sp.level = flyovers ? AirLevel::Weak : AirLevel::None;
  if (sp.disagree && sp.level > AirLevel::Moderate) sp.level = AirLevel::Moderate;
  char b[300];
  std::snprintf(b, sizeof b,
                "%d of %zu clips heard from this place, each on its own clock (%d flyover(s), weakest fit %.2f); lead over the best place elsewhere %.0f%%; "
                "%d clip(s) located here on their own",
                sp.clips_matched, clips.size(), flyovers, std::isfinite(min_fit) ? min_fit : 0.0, 100 * lead, sp.clips_agreeing);
  sp.reasons.insert(sp.reasons.begin(), b);
  out.shared = std::move(sp);
  return out;
}

AircraftStage run_aircraft_clips(const std::string& path, const std::vector<ClipSpan>& spans, const AircraftOptions& o) {
  std::cerr << "aircraft: decoding at 4 kHz" << std::endl;
  auto au = audio::load_audio(path, SPEC_RATE);
  // Each clip its own spectrogram (whitening and detection never straddle a cut).
  std::vector<Spectrogram> specs(spans.size());
  std::vector<ClipInput> in(spans.size());
  std::vector<std::string> pre(spans.size());
  for (std::size_t i = 0; i < spans.size(); ++i) {
    const auto a = std::min(au.samples.size(), std::size_t(std::max(0.0, spans[i].start_s) * SPEC_RATE));
    const auto b = std::min(au.samples.size(), std::size_t(std::max(0.0, spans[i].end_s) * SPEC_RATE));
    if (double(b - a) / SPEC_RATE < MIN_CLIP_AIR_S) {
      pre[i] = "clip too short to look for flyovers";
      continue;
    }
    specs[i] = whitened_spectrogram(std::span<const double>(au.samples.data() + a, b - a));
    in[i].spec = &specs[i];
    in[i].events = detect_events(specs[i]).events;
    for (auto h : spans[i].hyps) {
      if (!h.box) h.box = box_for(h, o);
      in[i].hyps.push_back(std::move(h));
    }
    std::cerr << "aircraft: clip " << i + 1 << ": " << in[i].events.size() << " flyover(s)" << std::endl;
  }
  au.samples.clear();
  au.samples.shrink_to_fit();
  Terrain terrain(o.online);
  int fetched = 0;
  auto j = search_clips(in, STORE_TRACKS, terrain, [&](const Hypothesis& h, double dur) { return ensure_days(h, dur, o, fetched); }, log_line);
  for (std::size_t i = 0; i < spans.size(); ++i)
    if (!pre[i].empty()) j.clips[i].note = pre[i];
  return clips_stage(spans, in, std::move(j));
}

AircraftStage clips_stage(const std::vector<ClipSpan>& spans, const std::vector<ClipInput>& in, JointClips j) {
  AircraftStage st;
  // One stage: events, hypotheses, flights and spectra of every clip, in file time.
  for (std::size_t i = 0; i < spans.size(); ++i) {
    ClipAir ca;
    ClipOutcome& oc = j.clips[i];
    ca.index = spans[i].index, ca.start_s = spans[i].start_s, ca.end_s = spans[i].end_s;
    ca.event0 = int(st.detection.events.size()), ca.n_events = int(in[i].events.size());
    for (Event e : in[i].events) {
      e.tc += ca.start_s, e.tc_lo += ca.start_s, e.tc_hi += ca.start_s;
      st.detection.events.push_back(std::move(e));
    }
    ca.hyp0 = int(st.tried.size()), ca.n_hyps = int(oc.tried.size());
    for (auto& hr : oc.tried) {
      hr.h.label = "clip " + std::to_string(i + 1) + ": " + hr.h.label;
      if (hr.result) shift_events(*hr.result, ca.event0);
      st.tried.push_back(std::move(hr));
    }
    ca.best = oc.best >= 0 ? ca.hyp0 + oc.best : -1;
    ca.at_hyp = oc.at_hyp >= 0 ? ca.hyp0 + oc.at_hyp : -1;
    ca.start_utc = oc.start_utc, ca.alone_m = oc.alone_m;
    ca.note = oc.note;
    if (in[i].spec) {
      std::vector<FlightView> fl;
      std::vector<SpectrumView> sp;
      add_views(fl, sp, *in[i].spec, in[i].events, oc.at_place ? &*oc.at_place : nullptr);
      for (auto& f : fl) f.event += ca.event0, f.clip = int(i), st.flights.push_back(std::move(f));
      for (auto& v : sp) {
        v.event += ca.event0, v.clip = int(i);
        v.t0 += ca.start_s, v.t1 += ca.start_s, v.utc0 -= ca.start_s;
        for (auto* cs : {&v.model, &v.straight})
          for (auto& c : *cs)
            for (auto& p : c) p[0] += float(ca.start_s);
        st.spectra.push_back(std::move(v));
      }
    }
    if (oc.at_place) shift_events(*oc.at_place, ca.event0), ca.at_place = std::move(oc.at_place);
    st.clips.push_back(std::move(ca));
  }
  st.shared = j.shared;
  if (st.shared) st.best = st.clips[std::size_t(st.shared->anchor)].best >= 0 ? st.clips[std::size_t(st.shared->anchor)].best : st.clips[std::size_t(st.shared->anchor)].at_hyp;
  if (st.detection.events.empty()) st.notes.push_back("no aircraft flyover detected in any clip");
  return st;
}

json::object stage_json(const AircraftStage& s) {
  json::array ev, tried, notes;
  for (std::size_t i = 0; i < s.detection.events.size(); ++i) {
    auto e = event_json(s.detection.events[i]);
    for (std::size_t k = 0; k < s.clips.size(); ++k)
      if (int(i) >= s.clips[k].event0 && int(i) < s.clips[k].event0 + s.clips[k].n_events) e["clip"] = int(k);
    ev.push_back(std::move(e));
  }
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
    fl.push_back(json::object{{"event", f.event},         {"clip", f.clip},         {"flight", f.flight},     {"registration", f.reg},   {"type", f.type},
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
    sp.push_back(json::object{{"event", v.event}, {"clip", v.clip}, {"utc0", num_or_null(v.utc0)}, {"matched", v.matched}, {"t0", v.t0}, {"t1", v.t1},
                              {"f_lo", v.f_lo},   {"f_hi", v.f_hi},       {"w", v.w},            {"h", v.h},
                              {"pixels", v.pixels}, {"model", curves(v.model)}, {"straight", curves(v.straight)}});
  out["flights"] = fl;
  out["spectra"] = sp;
  out["start_epoch"] = num_or_null(s.start_utc);
  if (!s.clips.empty()) {
    json::array cl;
    for (std::size_t i = 0; i < s.clips.size(); ++i) {
      const ClipAir& c = s.clips[i];
      json::array ms;
      if (c.at_place)
        for (auto& m : c.at_place->candidates[0].matches)
          ms.push_back(json::object{{"event", m.event}, {"flight", m.flight}, {"registration", m.reg}, {"type", m.type}, {"cpa_utc", iso(m.cpa_utc)}, {"fit", m.fit}});
      json::object o{{"clip", int(i)},
                     {"segment", c.index},
                     {"start_s", c.start_s},
                     {"end_s", c.end_s},
                     {"events", json::array{c.event0, c.event0 + c.n_events}},
                     {"hypotheses", json::array{c.hyp0, c.hyp0 + c.n_hyps}},
                     {"best_alone", c.best},
                     {"alone_m", num_or_null(c.alone_m)},
                     {"start_utc", std::isfinite(c.start_utc) ? json::value(iso(c.start_utc)) : json::value(nullptr)},
                     {"start_epoch", num_or_null(c.start_utc)},
                     {"level", air_level_name(c.at_place ? c.at_place->level : AirLevel::None)},
                     {"time_from", c.at_hyp >= 0 ? json::value(s.tried[std::size_t(c.at_hyp)].h.label) : json::value(nullptr)},
                     {"matches", ms}};
      if (c.at_place) o["at_place"] = result_json(*c.at_place, s.detection.events);
      if (!c.note.empty()) o["note"] = c.note;
      cl.push_back(std::move(o));
    }
    out["clips"] = std::move(cl);
  }
  if (s.shared) {
    const SharedPlace& p = *s.shared;
    json::array rs;
    for (auto& r : p.reasons) rs.push_back(json::value(r));
    out["shared"] = json::object{{"lat", p.lat},
                                 {"lon", p.lon},
                                 {"ground_m", p.ground_m},
                                 {"level", air_level_name(p.level)},
                                 {"anchor_clip", p.anchor},
                                 {"clips_matched", p.clips_matched},
                                 {"clips_agreeing", p.clips_agreeing},
                                 {"disagree", p.disagree},
                                 {"reasons", rs}};
    const ClipAir& a = s.clips[std::size_t(p.anchor)];
    out["best"] = json::object{{"hypothesis", s.best},
                               {"level", air_level_name(p.level)},
                               {"lat", p.lat},
                               {"lon", p.lon},
                               {"start_utc", std::isfinite(a.start_utc) ? json::value(iso(a.start_utc)) : json::value(nullptr)},
                               {"clips", true}};
  } else if (auto* r = s.best_result(); r && !r->candidates.empty()) {
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
  if (s.shared) {
    const SharedPlace& p = *s.shared;
    std::snprintf(b, sizeof b, "  Edited file: one place for %zu clips: %s %.5f, %.5f\n", s.clips.size(), air_level_name(p.level), p.lat, p.lon);
    out += b;
    for (auto& r : p.reasons) out += "    " + r + "\n";
  }
  for (std::size_t i = 0; i < s.clips.size(); ++i) {
    const ClipAir& c = s.clips[i];
    std::snprintf(b, sizeof b, "  Clip %zu (%.0f-%.0f s in the file, %d flyover(s)): ", i + 1, c.start_s, c.end_s, c.n_events);
    out += b;
    if (c.at_place) {
      std::snprintf(b, sizeof b, "starts %s at the shared place (%s", iso(c.start_utc).c_str(), air_level_name(c.at_place->level));
      out += b;
      for (auto& m : c.at_place->candidates[0].matches) out += ", " + (m.reg.empty() ? m.flight : m.reg) + (m.type.empty() ? "" : " " + m.type);
      out += ")";
      if (std::isfinite(c.alone_m)) {
        std::snprintf(b, sizeof b, "; alone %.1f km away", c.alone_m / 1000);
        out += b;
      }
      out += "\n";
    } else {
      out += (c.note.empty() ? std::string("not matched") : c.note) + "\n";
    }
  }
  if (!s.clips.empty()) {
    for (auto& c : s.clips)
      if (c.at_place) out += render_result(*c.at_place, s.detection.events);
  } else if (auto* r = s.best_result()) {
    out += render_result(*r, s.detection.events);
  }
  return out;
}

}  // namespace geo::air
