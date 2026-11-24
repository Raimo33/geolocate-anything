#include "air/opensky.hpp"

#include <openssl/evp.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <iostream>
#include <map>
#include <mutex>
#include <stdexcept>
#include <thread>

#include "air/bucket.hpp"
#include "air/geodesy.hpp"
#include "net/http.hpp"
#include "store/store.hpp"
#include "util/files.hpp"
#include "util/text.hpp"
#include "util/time.hpp"

namespace geo::air::opensky {

namespace asio = boost::asio;

namespace {

constexpr const char* TOKEN_URL = "https://auth.opensky-network.org/auth/realms/opensky-network/protocol/openid-connect/token";
constexpr const char* TRINO_URL = "https://trino.opensky-network.org";
constexpr const char* FORMAT = "opensky-v1";  // bump when the SQL's meaning or the conversion changes
constexpr std::int64_t ROW_LIMIT = 2000000;   // a safety net: a tight box and hour holds far fewer

std::string hex(const unsigned char* p, std::size_t n) {
  static const char* h = "0123456789abcdef";
  std::string s;
  for (std::size_t i = 0; i < n; ++i) s += h[p[i] >> 4], s += h[p[i] & 15];
  return s;
}

std::string sha256_hex(std::string_view s) {
  unsigned char md[EVP_MAX_MD_SIZE];
  unsigned int n = 0;
  EVP_Digest(s.data(), s.size(), md, &n, EVP_sha256(), nullptr);
  return hex(md, n);
}

std::string b64url_decode(std::string_view in) {
  std::string out;
  int val = 0, bits = -8;
  for (char ch : in) {
    int d;
    if (ch >= 'A' && ch <= 'Z') d = ch - 'A';
    else if (ch >= 'a' && ch <= 'z') d = ch - 'a' + 26;
    else if (ch >= '0' && ch <= '9') d = ch - '0' + 52;
    else if (ch == '-' || ch == '+') d = 62;
    else if (ch == '_' || ch == '/') d = 63;
    else break;  // '=' padding
    val = (val << 6) | d, bits += 6;
    if (bits >= 0) out += char((val >> bits) & 0xff), bits -= 8;
  }
  return out;
}

double snap_down(double x) { return std::floor(x / SNAP_DEG + 1e-9) * SNAP_DEG; }
double snap_up(double x) { return std::ceil(x / SNAP_DEG - 1e-9) * SNAP_DEG; }

std::vector<Track> load_cached(const fs::path& p) {
  auto bytes = read_file(p);
  if (!bytes) throw std::runtime_error("cannot read " + p.string());
  if (bytes->empty()) return {};  // the window held no aircraft
  return decode_tracks(*bytes);
}

// ---------------------------------------------------------------- Trino client

struct Token {
  std::string access, user;
  double expires = 0;
};

std::mutex query_mu;  // one query at a time, process-wide (OpenSky allows 2 running per user)
Token token;

asio::awaitable<void> login(net::Http& http) {
  if (!token.access.empty() && now_seconds() < token.expires) co_return;
  const std::string user = lower(trim(env("OPENSKY_USERNAME"))), pw = env("OPENSKY_PASSWORD");
  net::Request rq{.url = TOKEN_URL, .method = "POST"};
  rq.headers = {"Content-Type: application/x-www-form-urlencoded"};
  rq.body = "client_id=trino-client&grant_type=password&username=" + url_encode(user) + "&password=" + url_encode(pw);
  rq.total_timeout_s = 60;
  auto r = co_await http.fetch(std::move(rq));
  if (r.status == 400 || r.status == 401) throw std::runtime_error("OpenSky refused the login (check OPENSKY_USERNAME / OPENSKY_PASSWORD)");
  if (!r.ok()) throw std::runtime_error("OpenSky login failed: " + (r.error.empty() ? "HTTP " + std::to_string(r.status) : r.error));
  auto j = parse_json(r.body);
  token.access = jstr(j, "access_token");
  if (token.access.empty()) throw std::runtime_error("OpenSky login: no access token in the answer");
  token.user = token_user(token.access);
  if (token.user.empty()) token.user = user;
  token.expires = now_seconds() + std::max(60.0, jnum(j, "expires_in", 300) - 60);
}

std::vector<std::string> trino_headers() {
  return {"Authorization: Bearer " + token.access, "X-Trino-User: " + token.user, "X-Trino-Source: geolocate-anything", "X-Trino-Catalog: minio",
          "X-Trino-Schema: osky"};
}

using OnRows = std::function<void(const std::vector<std::string>& columns, const json::array& rows)>;

// Submits one statement and follows nextUri to the end. A full queue (2 running and
// 2 queued per user) is waited out; past 20 minutes the query is cancelled.
asio::awaitable<void> run_query(net::Http& http, const std::string& sql_text, const OnRows& on_rows) {
  const auto t_start = std::chrono::steady_clock::now();
  for (int submit = 0;; ++submit) {
    co_await login(http);
    net::Request rq{.url = std::string(TRINO_URL) + "/v1/statement", .method = "POST"};
    rq.headers = trino_headers();
    rq.body = sql_text;
    rq.total_timeout_s = 120;
    auto r = co_await http.fetch(std::move(rq));
    if (r.status == 401 && submit == 0) {
      token = {};
      continue;
    }
    if (r.status == 429 || r.status == 503) {
      if (submit >= 5) throw std::runtime_error("OpenSky Trino busy (HTTP " + std::to_string(r.status) + ")");
      co_await net::sleep_for(30);
      continue;
    }
    if (!r.ok()) throw std::runtime_error("OpenSky Trino: " + (r.error.empty() ? "HTTP " + std::to_string(r.status) : r.error));
    std::vector<std::string> columns;
    bool requeue = false;
    for (int polls = 0;; ++polls) {
      auto page = parse_json(r.body);
      if (auto* e = jget(page, "error"); e && e->is_object()) {
        const std::string name = jstr(*e, "errorName");
        if ((name == "QUERY_QUEUE_FULL" || name == "TOO_MANY_REQUESTS_FAILED") && submit < 5) {
          std::cerr << "opensky: queue full, waiting 30 s" << std::endl;
          requeue = true;
          break;
        }
        throw std::runtime_error("OpenSky query failed: " + name + ": " + jstr(*e, "message").substr(0, 300));
      }
      if (columns.empty())
        if (auto* cs = jget(page, "columns"); cs && cs->is_array())
          for (auto& c : cs->as_array()) columns.push_back(jstr(c, "name"));
      bool got = false;
      if (auto* d = jget(page, "data"); d && d->is_array() && !d->as_array().empty()) {
        on_rows(columns, d->as_array());
        got = true;
      }
      const std::string next = jstr(page, "nextUri");
      if (next.empty()) co_return;
      if (std::chrono::steady_clock::now() - t_start > std::chrono::minutes(20)) {
        net::Request del{.url = next, .method = "DELETE"};
        del.headers = trino_headers();
        co_await http.fetch(std::move(del));
        throw std::runtime_error("OpenSky query ran past 20 minutes: cancelled");
      }
      if (!got) co_await net::sleep_for(polls < 10 ? 0.25 : 1.0);
      for (int attempt = 0;; ++attempt) {
        net::Request g{.url = next};
        g.headers = trino_headers();
        g.total_timeout_s = 120;
        r = co_await http.fetch(std::move(g));
        if (r.ok()) break;
        if (attempt == 4 || (r.error.empty() && r.status != 429 && r.status < 500))
          throw std::runtime_error("OpenSky Trino: " + (r.error.empty() ? "HTTP " + std::to_string(r.status) : r.error));
        co_await net::sleep_for(double(2 << attempt));
      }
    }
    if (requeue) co_await net::sleep_for(30);
  }
}

// Fixes of one aircraft merged from several windows: time order, one per second.
void tidy(Track& tr) {
  std::sort(tr.fixes.begin(), tr.fixes.end(), [](const Fix& a, const Fix& b) { return a.t < b.t; });
  std::vector<Fix> fx;
  for (auto& f : tr.fixes)
    if (fx.empty() || f.t > fx.back().t + 0.5) fx.push_back(f);
  tr.fixes = std::move(fx);
}

}  // namespace

std::vector<Row> query_rows(const std::string& sql_text) {
  std::lock_guard lk(query_mu);
  std::vector<Row> rows;
  net::run_sync_void([&](asio::io_context& io) -> asio::awaitable<void> {
    net::Http http(io);
    co_await run_query(http, sql_text, [&](const std::vector<std::string>& cols, const json::array& page) { append_rows(cols, page, rows); });
  });
  return rows;
}

// ---------------------------------------------------------------- queries

std::vector<Window> windows(double t0, double t1, const Bbox& box) {
  Bbox b{std::max(-180.0, snap_down(box.west)), std::max(-90.0, snap_down(box.south)), std::min(180.0, snap_up(box.east)),
         std::min(90.0, snap_up(box.north))};
  const std::int64_t s = std::int64_t(SNAP_S);
  const std::int64_t a = floor_div(std::int64_t(std::floor(t0)), s) * s;
  const std::int64_t z = -floor_div(-std::int64_t(std::ceil(t1)), s) * s;
  std::vector<Window> out;
  for (std::int64_t h = floor_div(a, 3600) * 3600; h < z; h += 3600) {
    Window w{h, std::max(a, h), std::min(z, h + 3599), b};
    if (w.t0 <= w.t1) out.push_back(w);
  }
  return out;
}

namespace {
// Degrees as typed: 40.8, 41.0, -73.6, 22.5, 11.25 (no binary noise; the keys of earlier answers unchanged).
std::string deg(double x) {
  char b[32];
  std::snprintf(b, sizeof b, "%.4f", std::round(x * 1e4) / 1e4 + 0.0);
  std::string s = b;
  while (s.back() == '0' && s[s.size() - 2] != '.') s.pop_back();  // at least one decimal, as "%.1f" wrote them
  return s == "-0.0" ? "0.0" : s;
}
}  // namespace

std::string sql(const Window& w, std::int64_t limit) {
  char b[1024];
  std::snprintf(b, sizeof b,
                "SELECT time, icao24, lat, lon, velocity, heading, vertrate, baroaltitude, geoaltitude\n"
                "FROM state_vectors_data4\n"
                "WHERE hour = %lld\n"
                "  AND time BETWEEN %lld AND %lld\n"
                "  AND time %% %d = 0\n"
                "  AND lat BETWEEN %s AND %s\n"
                "  AND lon BETWEEN %s AND %s\n"
                "  AND onground = false\n"
                "  AND time - lastcontact <= 15\n"
                "  AND (velocity IS NULL OR velocity <= %.0f)\n"
                "  AND COALESCE(geoaltitude, baroaltitude) <= %.0f\n"
                "LIMIT %lld",
                (long long)w.hour, (long long)w.t0, (long long)w.t1, int(SAMPLE_S), deg(w.box.south).c_str(), deg(w.box.north).c_str(),
                deg(w.box.west).c_str(), deg(w.box.east).c_str(), MAX_SPEED, MAX_ALT_M, (long long)(limit > 0 ? limit : ROW_LIMIT));
  return b;
}

std::string cache_key(const Window& w) { return sha256_hex(std::string(FORMAT) + "\n" + sql(w)).substr(0, 32); }

std::string token_user(std::string_view jwt) {
  const auto a = jwt.find('.');
  if (a == std::string_view::npos) return {};
  const auto b = jwt.find('.', a + 1);
  try {
    return jstr(parse_json(b64url_decode(jwt.substr(a + 1, b == std::string_view::npos ? std::string_view::npos : b - a - 1))), "preferred_username");
  } catch (const std::exception&) {
    return {};
  }
}

// ---------------------------------------------------------------- rows -> tracks

void append_rows(const std::vector<std::string>& columns, const json::array& rows, std::vector<Row>& out) {
  auto col = [&](const char* n) {
    auto it = std::find(columns.begin(), columns.end(), n);
    return it == columns.end() ? -1 : int(it - columns.begin());
  };
  const int c_t = col("time"), c_id = col("icao24"), c_lat = col("lat"), c_lon = col("lon"), c_v = col("velocity"), c_h = col("heading"),
            c_vr = col("vertrate"), c_baro = col("baroaltitude"), c_geo = col("geoaltitude");
  if (c_t < 0 || c_id < 0 || c_lat < 0 || c_lon < 0) throw std::runtime_error("OpenSky rows lack time, icao24, lat or lon");
  for (auto& rv : rows) {
    if (!rv.is_array()) continue;
    auto& a = rv.as_array();
    auto num = [&](int i) { return i >= 0 && std::size_t(i) < a.size() ? as_number(a[std::size_t(i)]) : NAN; };
    if (std::size_t(c_id) >= a.size() || !a[std::size_t(c_id)].is_string()) continue;
    const auto& id = a[std::size_t(c_id)].as_string();
    if (id.empty() || id.size() > 6 || std::string_view(id).find_first_not_of("0123456789abcdefABCDEF") != std::string_view::npos) continue;
    Row r{std::uint32_t(std::strtoul(id.c_str(), nullptr, 16)), num(c_t), num(c_lat), num(c_lon), float(num(c_v)), float(num(c_h)), float(num(c_vr)),
          float(num(c_baro)), float(num(c_geo))};
    if (!std::isfinite(r.t) || !std::isfinite(r.lat) || !std::isfinite(r.lon)) continue;
    if (!std::isfinite(r.geo) && !std::isfinite(r.baro)) continue;
    out.push_back(r);
  }
}

std::vector<Track> tracks_of_rows(std::vector<Row> rows, bool keep_jets) {
  auto median = [](std::vector<double>& v) {
    std::nth_element(v.begin(), v.begin() + std::ptrdiff_t(v.size() / 2), v.end());
    return v[v.size() / 2];
  };
  // The local pressure deviation: GNSS minus barometric altitude, per 2 x 2 degree cell
  // (as an ADSB.lol day; the rows span an hour at most).
  auto cell = [](double lat, double lon) { return std::pair<int, int>{int(std::floor(lat / 2)), int(std::floor(lon / 2))}; };
  std::map<std::pair<int, int>, std::vector<double>> cell_off;
  for (auto& r : rows)
    if (std::isfinite(r.geo) && std::isfinite(r.baro)) cell_off[cell(r.lat, r.lon)].push_back(double(r.geo) - double(r.baro));
  std::map<std::pair<int, int>, double> region;
  for (auto& [k, v] : cell_off)
    if (v.size() >= 3) region[k] = median(v);
  std::sort(rows.begin(), rows.end(), [](const Row& x, const Row& y) { return x.icao != y.icao ? x.icao < y.icao : x.t < y.t; });
  std::vector<Track> out;
  for (std::size_t i = 0; i < rows.size();) {
    std::size_t j = i;
    while (j < rows.size() && rows[j].icao == rows[i].icao) ++j;
    Track tr;
    tr.icao = rows[i].icao;
    tr.type = AircraftDb::get().type(tr.icao);
    if (auto ti = TypeDb::get().find(tr.type)) tr.info = *ti;
    if (tr.info.engine == EngineType::Jet && !keep_jets) {
      i = j;
      continue;
    }
    std::vector<double> own;
    for (std::size_t k = i; k < j; ++k)
      if (std::isfinite(rows[k].geo) && std::isfinite(rows[k].baro)) own.push_back(double(rows[k].geo) - double(rows[k].baro));
    const double off = own.size() >= 3 ? median(own) : NAN;
    for (std::size_t k = i; k < j; ++k) {
      const Row& r = rows[k];
      Fix f;
      f.t = r.t, f.lat = r.lat, f.lon = r.lon;
      f.gs = r.v, f.track = r.h, f.vrate = r.vr;
      if (std::isfinite(r.geo)) f.alt_m = r.geo, f.alt_src = AltSource::Geometric;
      else if (std::isfinite(off)) f.alt_m = float(r.baro + off), f.alt_src = AltSource::BaroTrack;
      else if (auto it = region.find(cell(r.lat, r.lon)); it != region.end()) f.alt_m = float(r.baro + it->second), f.alt_src = AltSource::BaroRegion;
      else f.alt_m = r.baro, f.alt_src = AltSource::BaroRaw;
      tr.fixes.push_back(f);
    }
    tidy(tr);
    if (!tr.fixes.empty()) out.push_back(std::move(tr));
    i = j;
  }
  return out;
}

std::vector<Track> tracks_from_rows(const std::vector<std::string>& columns, const json::array& rows, bool keep_jets) {
  std::vector<Row> r;
  append_rows(columns, rows, r);
  return tracks_of_rows(std::move(r), keep_jets);
}

std::vector<Track> to_nodes(const std::vector<Track>& tracks) {
  if (std::none_of(tracks.begin(), tracks.end(), [](const Track& t) { return !t.fixes.empty(); })) return {};
  return decode_tracks(encode_tracks(tracks));
}

// ---------------------------------------------------------------- merging

Merged merge(const std::vector<Track>& adsb_segments, const std::vector<Track>& opensky_tracks) {
  Merged m;
  m.segments = adsb_segments;
  m.from_adsb = adsb_segments.size();
  std::map<std::uint32_t, std::vector<std::pair<double, double>>> covered;
  std::map<std::uint32_t, const Track*> meta;
  for (auto& s : adsb_segments)
    if (!s.fixes.empty()) covered[s.icao].emplace_back(s.fixes.front().t, s.fixes.back().t), meta.emplace(s.icao, &s);
  for (auto& seg : split_tracks(opensky_tracks)) {
    auto it = covered.find(seg.icao);
    if (it == covered.end()) {
      m.segments.push_back(seg);
      m.only_opensky.insert(seg.icao);
      continue;
    }
    // ADSB.lol has this aircraft: OpenSky only where it has no segment within GAP_S.
    Track cur = seg;
    const Track& known = *meta.at(seg.icao);
    cur.type = known.type, cur.reg = known.reg, cur.info = known.info;  // one aircraft, one identity
    cur.fixes.clear();
    bool added = false;
    auto flush = [&] {
      if (cur.fixes.size() >= 2) m.segments.push_back(cur), added = true;
      cur.fixes.clear();
    };
    for (auto& f : seg.fixes) {
      const bool inside = std::any_of(it->second.begin(), it->second.end(), [&](auto& ab) { return f.t >= ab.first - GAP_S && f.t <= ab.second + GAP_S; });
      if (inside) flush();
      else cur.fixes.push_back(f);
    }
    flush();
    if (added) m.gap_filled.insert(seg.icao);
  }
  return m;
}

// ---------------------------------------------------------------- fetching

Options options_from_env() {
  Options o;
  const std::string on = env("GEOLOC_OPENSKY");
  o.enabled = !on.empty() && on != "0" && on != "false" && on != "off";
  if (auto n = to_int(env("GEOLOC_OPENSKY_PLACES")); n && *n > 0) o.places = int(*n);
  if (auto r = to_double(env("GEOLOC_OPENSKY_RADIUS_KM")); r && *r > 0) o.radius_m = *r * 1000.0;
  o.push = env("GEOLOC_OPENSKY_PUSH") != "0";
  if (auto n = to_int(env("GEOLOC_OPENSKY_MAX_QUERIES")); n && *n >= 0) o.max_queries = int(*n);
  return o;
}

bool has_credentials() { return !env("OPENSKY_USERNAME").empty() && !env("OPENSKY_PASSWORD").empty(); }

// ---------------------------------------------------------------- checking answers

namespace {

constexpr double SAME_M = 250.0;      // the same answer: within about a corridor cell
constexpr double SAME_SCORE = 0.9;    //   and nearly its score
constexpr double MOVED_SCORE = 0.5;   // the same flights elsewhere nearby, still fitting this well: moved

const Candidate* with_combo(const SearchResult& r, const std::string& combo) {
  for (auto& c : r.candidates)
    if (c.combo == combo) return &c;
  return nullptr;
}

std::vector<Fix> clip(const std::vector<Fix>& fx, double t0, double t1, const Bbox& box) {
  auto a = std::size_t(std::lower_bound(fx.begin(), fx.end(), t0, [](const Fix& f, double t) { return f.t < t; }) - fx.begin());
  auto b = std::size_t(std::upper_bound(fx.begin(), fx.end(), t1, [](double t, const Fix& f) { return t < f.t; }) - fx.begin());
  if (a > 0) --a;
  if (b < fx.size()) ++b;
  std::vector<Fix> out;
  for (std::size_t i = a; i < b; ++i)
    if (box.contains(fx[i].lat, fx[i].lon)) out.push_back(fx[i]);
  return out;
}

struct Plan {
  int rank = 0;
  Candidate cand;
  Bbox box;
  double t0 = 0, t1 = 0;
  std::vector<Track> tracks;  // OpenSky's, one per aircraft
};

}  // namespace

void check(SearchResult& r, const Spectrogram& spec, const std::vector<Event>& events, const SearchOptions& so, const TrackSource& adsb, Terrain& terrain,
           const Options& o) {
  if (!o.enabled) return;
  auto rep = std::make_shared<Report>();
  rep->tiles = o.tiles;
  r.opensky = rep;
  rep->status = "checked";
  if (r.candidates.empty()) {
    rep->note = "no answer to check";
    return;
  }
  // 1. Every window first: when OpenSky cannot answer one, nothing changes.
  std::vector<Plan> plans;
  for (int k = 0; k < o.places && std::size_t(k) < r.candidates.size(); ++k) {
    Plan pl;
    pl.rank = k, pl.cand = r.candidates[std::size_t(k)];
    const Candidate& c = pl.cand;
    double a = INFINITY, b = -INFINITY;
    for (auto& m : c.matches) {
      if (m.event < 0 || std::size_t(m.event) >= events.size()) continue;
      const Event& e = events[std::size_t(m.event)];
      const double hw = std::min(so.max_window_s, e.half_window_s * std::max(1.0, so.window_factor));
      a = std::min(a, c.start_utc + e.tc_lo - hw), b = std::max(b, c.start_utc + e.tc_hi + hw);
    }
    if (so.silence_whole || !std::isfinite(a)) a = std::min(a, c.start_utc), b = std::max(b, c.start_utc + spec.duration_s);
    pl.t0 = a - o.margin_s, pl.t1 = b + o.margin_s;
    const auto ws = windows(pl.t0, pl.t1, Bbox{c.lon, c.lat, c.lon, c.lat}.grown(o.radius_m));
    if (ws.empty()) continue;
    pl.box = ws.front().box;
    std::map<std::uint32_t, Track> by;
    for (auto& w : ws) {
      std::string src;
      std::vector<Track> got;
      try {
        if (o.source) got = o.source(w), src = "given";
        else got = fetch(w, &src, o.push);
      } catch (const NotConfigured& e) {
        rep->status = "not configured";
        rep->note = std::string(e.what()) + ": ADSB.lol only, the search is unchanged";
        rep->places.clear();
        return;
      } catch (const std::exception& e) {
        rep->status = "unreachable";
        rep->note = std::string("OpenSky could not answer (") + e.what() + "): ADSB.lol only, the search is unchanged";
        rep->places.clear();
        return;
      }
      rep->queries.push_back(iso(double(w.t0)) + " .. " + iso(double(w.t1)) + " (" + src + ")");
      for (auto& t : got) {
        auto [it, fresh] = by.try_emplace(t.icao, t);
        if (!fresh) it->second.fixes.insert(it->second.fixes.end(), t.fixes.begin(), t.fixes.end());
      }
    }
    // Exactly what ADSB.lol is read for (load_tracks): the window and one fix either
    // side, inside the box. The query windows are snapped outwards; read as they are,
    // OpenSky would bring passes ADSB.lol was never asked for.
    for (auto& [icao, t] : by) {
      tidy(t);
      t.fixes = clip(t.fixes, pl.t0, pl.t1, pl.box);
      if (!t.fixes.empty()) pl.tracks.push_back(std::move(t));
    }
    plans.push_back(std::move(pl));
  }
  // The silence range, as the search measures it: the strongest events' slant ranges.
  double max_d = 0;
  for (int i : r.events_used)
    if (i >= 0 && std::size_t(i) < events.size()) max_d = std::max(max_d, events[std::size_t(i)].d);
  const double silence_m = so.silence_factor * max_d;

  // 2. Each place searched again, ADSB.lol alone and merged.
  std::map<std::string, std::optional<Candidate>> verdict;  // combo -> replacement (nullopt: dropped)
  for (auto& pl : plans) {
    const Candidate& c = pl.cand;
    PlaceCheck pc;
    pc.rank = pl.rank, pc.lat = c.lat, pc.lon = c.lon, pc.start_utc = c.start_utc, pc.score = c.score, pc.combo = c.combo;
    const auto ad = adsb(pl.box, pl.t0, pl.t1);
    const auto mg = merge(ad, pl.tracks);
    pc.adsb_segments = mg.from_adsb, pc.opensky_segments = mg.segments.size() - mg.from_adsb;
    SearchOptions ro = so;
    ro.box = Bbox{c.lon, c.lat, c.lon, c.lat}.grown(1500.0);
    ro.start_min = c.start_utc - 15, ro.start_max = c.start_utc + 15;
    ro.top_k = 5, ro.max_refine = 2, ro.score_floor = -INFINITY;
    const SearchResult base = search(spec, events, ad, terrain, ro);
    const SearchResult with = pc.opensky_segments ? search(spec, events, mg.segments, terrain, ro) : base;
    const Candidate* bm = with_combo(base, c.combo);
    const Candidate* wm = with_combo(with, c.combo);
    char b[256];
    // The answer stands only where it was: the same flights within SAME_M of the place
    // the search without OpenSky finds, scoring nearly as well. (Elsewhere in the box
    // they may still survive, beyond the silence range of what OpenSky adds.)
    const bool same = bm && wm && haversine_m(wm->lat, wm->lon, bm->lat, bm->lon) <= SAME_M && wm->score >= SAME_SCORE * bm->score;
    if (!bm && wm) {
      pc.outcome = "found with OpenSky";
      pc.detail = "these flights fit here only with OpenSky's tracks (ADSB.lol alone does not find them)";
    } else if (!bm) {
      pc.outcome = "not reproduced";
      pc.detail = "the place's own search with ADSB.lol alone did not find these flights again; left as it was";
    } else if (same && with.candidates[0].combo != c.combo && with.candidates[0].score > wm->score &&
               with.candidates[0].combo != base.candidates[0].combo) {  // a winner OpenSky brought, not one already there
      pc.outcome = "changed";
      pc.replacement = with.candidates[0];
      std::snprintf(b, sizeof b, "flights %s now score %.1f here, above these flights' %.1f (with ADSB.lol alone the best here is %s, %.1f)",
                    with.candidates[0].combo.c_str(), with.candidates[0].score, wm->score, base.candidates[0].combo.c_str(), base.candidates[0].score);
      pc.detail = b;
      verdict[c.combo] = with.candidates[0];
      ++rep->changed;
    } else if (same) {
      pc.outcome = "unchanged";
      if (base.candidates[0].combo != c.combo) {
        std::snprintf(b, sizeof b, "flights %s outscore these here with ADSB.lol alone too (%.1f)", base.candidates[0].combo.c_str(),
                      base.candidates[0].score);
        pc.detail = b;
      } else if (std::abs(wm->score - bm->score) > 1e-6) {
        std::snprintf(b, sizeof b, "score %.1f with OpenSky's tracks, %.1f without", wm->score, bm->score);
        pc.detail = b;
      }
    } else if (wm && wm->score >= MOVED_SCORE * bm->score) {
      pc.outcome = "moved";
      pc.replacement = *wm;
      std::snprintf(b, sizeof b, "these flights now fit best %.0f m away, scoring %.1f (%.1f without OpenSky)", haversine_m(wm->lat, wm->lon, bm->lat, bm->lon),
                    wm->score, bm->score);
      pc.detail = b;
      verdict[c.combo] = *wm;
      ++rep->changed;
    } else {
      pc.outcome = "dropped";
      if (with.candidates.empty()) pc.detail = "no combination of flights survives within 1.5 km once OpenSky's aircraft are known";
      else {
        std::snprintf(b, sizeof b, "these flights no longer survive here; the best left within 1.5 km (%s) scores %.1f, against %.1f without OpenSky",
                      with.candidates[0].combo.c_str(), with.candidates[0].score, bm->score);
        pc.detail = b;
      }
      verdict[c.combo] = std::nullopt;
      ++rep->dropped;
    }
    // OpenSky's additions near the place while the matched flyovers were heard.
    double first = INFINITY, last = -INFINITY;
    std::set<std::string> mine;
    for (auto& m : c.matches)
      if (m.event >= 0 && std::size_t(m.event) < events.size()) {
        const double t = c.start_utc + events[std::size_t(m.event)].tc;
        first = std::min(first, t), last = std::max(last, t);
        mine.insert(m.flight);
      }
    if (so.silence_whole) first = c.start_utc, last = c.start_utc + spec.duration_s;
    const Vec3 lst = ecef(c.lat, c.lon, c.ground_m + so.mic_height);
    std::map<std::uint32_t, Aircraft> near;
    for (std::size_t i = mg.from_adsb; i < mg.segments.size(); ++i) {
      const Track& seg = mg.segments[i];
      const Path path = path_of(seg);
      if (path.empty()) continue;
      Aircraft ac;
      ac.flight = seg.id(), ac.type = seg.type, ac.reg = seg.reg, ac.only_opensky = mg.only_opensky.count(seg.icao) > 0;
      for (double t = path.t0(); t <= path.t1(); t += 2.0) {
        Vec3 p, v;
        path.state(t, p, v);
        const double rr = (p - lst).norm(), t_rx = t + rr / so.c;
        if (t_rx < first - 120 || t_rx > last + 120) continue;
        if (!(rr >= ac.closest_m)) ac.closest_m = rr, ac.closest_utc = t_rx;
        if (rr <= silence_m && t_rx >= first + 30 && t_rx <= last - 30 && !mine.count(ac.flight)) {
          bool heard = false;
          for (auto& e : events) heard |= std::abs(t_rx - (c.start_utc + e.tc)) < std::max(30.0, e.half_window_s);
          if (!heard) ac.unheard = true;
        }
      }
      if (!std::isfinite(ac.closest_m) || ac.closest_m > std::max(2 * silence_m, 10000.0)) continue;
      auto [it, fresh] = near.try_emplace(seg.icao, ac);
      if (!fresh) {
        it->second.unheard |= ac.unheard;
        if (ac.closest_m < it->second.closest_m) it->second.closest_m = ac.closest_m, it->second.closest_utc = ac.closest_utc;
      }
    }
    for (auto& [icao, ac] : near) pc.aircraft.push_back(ac);
    std::sort(pc.aircraft.begin(), pc.aircraft.end(), [](auto& x, auto& y) { return x.closest_m < y.closest_m; });
    rep->places.push_back(std::move(pc));
  }

  // 3. The outcome applied.
  if (!verdict.empty()) {
    std::vector<Candidate> next;
    for (auto& c : r.candidates) {
      auto it = verdict.find(c.combo);
      if (it == verdict.end()) next.push_back(c);
      else if (it->second) next.push_back(*it->second);
    }
    std::sort(next.begin(), next.end(), [](const Candidate& x, const Candidate& y) { return x.score > y.score; });
    std::vector<Candidate> uniq;
    std::set<std::string> seen;
    for (auto& c : next)
      if (seen.insert(c.combo).second) uniq.push_back(std::move(c));
    r.candidates = std::move(uniq);
    judge(r);
  }
  char b[256];
  std::snprintf(b, sizeof b, "OpenSky: %zu best place(s) checked against its aircraft too: %d dropped, %d changed", rep->places.size(), rep->dropped,
                rep->changed);
  r.reasons.push_back(b);
}

// ---------------------------------------------------------------- whole days

std::vector<Window> day_windows(const std::string& day, double band_deg) {
  const auto t = parse_iso_utc(day + "T00:00:00Z");
  if (!t) throw std::invalid_argument("not a day: " + day);
  std::vector<Window> out;
  for (int h = 0; h < 24; ++h)
    for (double la = -90; la < 90 - 1e-9; la += band_deg) {
      const std::int64_t hour = *t + h * 3600;
      out.push_back(Window{hour, hour, hour + 3599, Bbox{-180, la, 180, std::min(90.0, la + band_deg)}});
    }
  return out;
}

fs::path source_path(const std::string& day) { return adsb_dir() / (day + ".source"); }

std::string chunk_name(const Window& w) {
  char name[160];
  std::snprintf(name, sizeof name, "%02d_%s_%s_%s_%s.trk", int(((w.hour % 86400) + 86400) % 86400 / 3600), deg(w.box.south).c_str(),
                deg(w.box.west).c_str(), deg(w.box.north).c_str(), deg(w.box.east).c_str());
  return name;
}

DayStats build_day(const std::string& day, const DayOptions& o, const std::function<void(const std::string&)>& progress) {
  const auto clock0 = std::chrono::steady_clock::now();
  auto say = [&](const std::string& m) {
    if (progress) progress(m);
  };
  if (!o.force) {
    if (has_day(day) && !fs::exists(source_path(day))) throw std::runtime_error(day + " is stored from ADSB.lol already (--force to build it anyway)");
    if (o.push && bucket::enabled() && bucket::days().count(day) && !fs::exists(source_path(day)))
      throw std::runtime_error(day + " is in the aircraft bucket already (--force to build it anyway)");
  }
  const fs::path dir = store::cache_dir() / "opensky" / "days" / day;
  fs::create_directories(dir);
  DayStats st;
  std::map<std::uint32_t, Track> by;
  auto take = [&](std::vector<Track> got) {
    for (auto& t : got) {
      st.nodes += t.fixes.size();
      auto [it, fresh] = by.try_emplace(t.icao, std::move(t));
      if (!fresh) it->second.fixes.insert(it->second.fixes.end(), t.fixes.begin(), t.fixes.end());
    }
  };
  std::vector<Window> todo = day_windows(day, o.band_deg);
  std::reverse(todo.begin(), todo.end());  // a stack: splits are done next
  const std::size_t first = todo.size();
  std::size_t done = 0;
  while (!todo.empty()) {
    const Window w = todo.back();
    todo.pop_back();
    const std::string name = chunk_name(w);
    const fs::path p = dir / name;
    if (fs::exists(p)) {
      take(load_cached(p));
      ++st.cached;
    } else {
      if (!has_credentials()) throw NotConfigured("OPENSKY_USERNAME / OPENSKY_PASSWORD are not set");
      if (st.queries) std::this_thread::sleep_for(std::chrono::milliseconds(std::int64_t(o.pause_s * 1000)));
      auto rows = query_rows(sql(w, o.limit));
      ++st.queries;
      if (std::int64_t(rows.size()) >= o.limit) {
        // Too much for one answer: halve the box (longitude first, down to 22.5 degrees).
        ++st.splits;
        const bool lon = w.box.east - w.box.west > 22.5 + 1e-9;
        Window a = w, b = w;
        if (lon) a.box.east = b.box.west = 0.5 * (w.box.west + w.box.east);
        else a.box.north = b.box.south = 0.5 * (w.box.south + w.box.north);
        todo.push_back(b), todo.push_back(a);
        say(name + ": " + std::to_string(rows.size()) + " rows reach the limit, split");
        continue;
      }
      st.rows += rows.size();
      auto tracks = tracks_of_rows(std::move(rows));
      const bool any = std::any_of(tracks.begin(), tracks.end(), [](const Track& t) { return !t.fixes.empty(); });
      atomic_write(p, any ? encode_tracks(tracks) : std::string());
      take(load_cached(p));
    }
    if (++done % 12 == 0 || todo.empty())
      say(day + ": " + std::to_string(done) + " band-hours (" + std::to_string(first) + "+ splits), " + std::to_string(st.queries) + " queries, " +
          std::to_string(st.rows) + " rows, " + std::to_string(by.size()) + " aircraft");
  }
  std::vector<Track> tracks;
  tracks.reserve(by.size());
  for (auto& [icao, t] : by) {
    tidy(t);
    if (t.fixes.size() >= 2) tracks.push_back(std::move(t));
  }
  st.aircraft = tracks.size();
  const std::string bytes = encode_tracks(tracks);
  st.bytes = bytes.size();
  atomic_write(day_path(day), bytes);
  atomic_write(source_path(day), "opensky " + now_isoformat() + "\nThe OpenSky Network, https://opensky-network.org\n");
  ensure_coverage(day);
  ensure_index(day, true);
  mark_used(day);
  if (o.push && bucket::enabled()) {
    bucket::push({day});
    bucket::put_object("adsb/" + day + ".source", source_path(day));
  }
  st.seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - clock0).count();
  return st;
}

// ---------------------------------------------------------------- tiles

namespace {

constexpr std::int64_t TILE_LIMIT = 1000000;  // rows per tile query; more: the rectangle is halved

std::string two(int h) {
  char b[4];
  std::snprintf(b, sizeof b, "%02d", h);
  return b;
}

fs::path tile_path(const Tile& t) { return store::cache_dir() / tile_key(t); }
std::string hour_prefix(std::int64_t hour) { return "opensky/tiles/" + iso_date(hour) + "/" + two(int((hour % 86400) / 3600)) + "/"; }
bool day_from_opensky(std::int64_t hour) { return fs::exists(source_path(iso_date(hour))); }
Tile tile_of_key(const std::string& key);  // after tile_key

std::vector<Track> tracks_of_tiles(const std::vector<Tile>& tiles, double t0, double t1, const Bbox& box) {
  std::map<std::uint32_t, Track> by;
  for (auto& t : tiles) {
    const fs::path p = tile_path(t);
    if (!fs::exists(p)) continue;
    for (auto& tr : load_cached(p)) {
      auto [it, fresh] = by.try_emplace(tr.icao, tr);
      if (!fresh) it->second.fixes.insert(it->second.fixes.end(), tr.fixes.begin(), tr.fixes.end());
    }
  }
  std::vector<Track> out;
  for (auto& [icao, tr] : by) {
    tidy(tr);
    tr.fixes = clip(tr.fixes, t0, t1, box);
    if (!tr.fixes.empty()) out.push_back(std::move(tr));
  }
  return out;
}

}  // namespace

std::vector<Tile> tiles_for(const Bbox& box, double t0, double t1) {
  std::vector<Tile> out;
  const int la0 = std::max(-90, int(std::floor(box.south))), la1 = std::min(89, int(std::ceil(box.north)) - 1);
  const int lo0 = std::max(-180, int(std::floor(box.west))), lo1 = std::min(179, int(std::ceil(box.east)) - 1);
  for (std::int64_t h = floor_div(std::int64_t(std::floor(t0)), 3600) * 3600; h <= std::int64_t(std::floor(t1)); h += 3600)
    for (int la = la0; la <= std::max(la0, la1); ++la)
      for (int lo = lo0; lo <= std::max(lo0, lo1); ++lo) out.push_back({h, la, lo});
  return out;
}

std::string tile_key(const Tile& t) { return hour_prefix(t.hour) + std::to_string(t.lat) + "_" + std::to_string(t.lon) + ".trk"; }

namespace {
Tile tile_of_key(const std::string& key) {  // opensky/tiles/2026-09-08/20/40_-74.trk
  std::vector<std::string> parts;
  for (auto v : split(key, '/')) parts.emplace_back(v);
  if (parts.size() != 5) throw std::runtime_error("not a tile key: " + key);
  const auto day = parse_iso_utc(parts[2]);
  const auto us = parts[4].find('_');
  if (!day || us == std::string::npos) throw std::runtime_error("not a tile key: " + key);
  return {std::int64_t(*day) + std::stoi(parts[3]) * 3600, std::stoi(parts[4].substr(0, us)), std::stoi(parts[4].substr(us + 1))};
}
}  // namespace

std::vector<Tile> fetch_tiles(std::vector<Tile> tiles, int max_queries, bool push, TileStats& st) {
  std::map<std::int64_t, std::set<std::pair<int, int>>> by_hour;
  for (auto& t : tiles) by_hour[t.hour].insert({t.lat, t.lon});
  std::vector<Tile> left;
  std::vector<std::pair<std::string, fs::path>> uploads;
  for (auto& [hour, want] : by_hour) {
    int la0 = 90, la1 = -91, lo0 = 180, lo1 = -181;
    for (auto& [la, lo] : want) la0 = std::min(la0, la), la1 = std::max(la1, la), lo0 = std::min(lo0, lo), lo1 = std::max(lo1, lo);
    std::vector<std::array<int, 4>> stack{{la0, lo0, la1 + 1, lo1 + 1}};  // south, west, north, east (whole degrees)
    while (!stack.empty()) {
      const auto r = stack.back();
      stack.pop_back();
      std::vector<std::pair<int, int>> inside;
      for (auto& [la, lo] : want)
        if (la >= r[0] && la < r[2] && lo >= r[1] && lo < r[3]) inside.push_back({la, lo});
      if (inside.empty()) continue;
      if (max_queries > 0 && st.queries >= max_queries) {
        for (auto& [la, lo] : inside) left.push_back({hour, la, lo});
        continue;
      }
      if (st.queries) std::this_thread::sleep_for(std::chrono::seconds(1));
      std::cerr << "opensky: tiles " << iso(double(hour)) << " [" << r[1] << "," << r[0] << "," << r[3] << "," << r[2] << "], " << inside.size()
                << " tile(s)" << std::endl;
      auto rows = query_rows(sql(Window{hour, hour, hour + 3599, Bbox{double(r[1]), double(r[0]), double(r[3]), double(r[2])}}, TILE_LIMIT));
      ++st.queries;
      if (std::int64_t(rows.size()) >= TILE_LIMIT && (r[2] - r[0] > 1 || r[3] - r[1] > 1)) {
        ++st.splits;
        if (r[3] - r[1] >= r[2] - r[0]) {
          const int m = r[1] + (r[3] - r[1]) / 2;
          stack.push_back({r[0], m, r[2], r[3]}), stack.push_back({r[0], r[1], r[2], m});
        } else {
          const int m = r[0] + (r[2] - r[0]) / 2;
          stack.push_back({m, r[1], r[2], r[3]}), stack.push_back({r[0], r[1], m, r[3]});
        }
        continue;
      }
      st.rows += rows.size();
      std::map<std::pair<int, int>, std::vector<Row>> per;
      for (auto& row : rows) per[{int(std::floor(row.lat)), int(std::floor(row.lon))}].push_back(row);
      rows.clear();
      rows.shrink_to_fit();
      for (auto& tl : inside) {
        const Tile t{hour, tl.first, tl.second};
        auto it = per.find(tl);
        auto tracks = it == per.end() ? std::vector<Track>{} : tracks_of_rows(std::move(it->second));
        const bool any = std::any_of(tracks.begin(), tracks.end(), [](const Track& x) { return !x.fixes.empty(); });
        const fs::path p = tile_path(t);
        fs::create_directories(p.parent_path());
        atomic_write(p, any ? encode_tracks(tracks) : std::string());
        ++st.fetched;
        if (push) uploads.push_back({tile_key(t), p});
      }
    }
  }
  if (!uploads.empty() && bucket::enabled()) {
    try {
      bucket::put_objects(uploads);
    } catch (const std::exception& e) {
      std::cerr << "opensky: upload of tiles failed: " << e.what() << std::endl;
    }
  }
  return left;
}

namespace {

// Makes `all` available here: this machine, else the bucket, else (query) OpenSky, at
// most max_queries (0: no limit). Returns the tiles still missing.
std::vector<Tile> ensure_tiles(const std::vector<Tile>& all, bool query, int max_queries, bool push, TileStats& st) {
  st.tiles += all.size();
  std::vector<Tile> need;
  for (auto& t : all) {
    if (day_from_opensky(t.hour)) ++st.skipped;
    else if (fs::exists(tile_path(t))) ++st.local;
    else need.push_back(t);
  }
  if (!need.empty() && bucket::enabled()) {
    try {
      std::set<std::int64_t> hours;
      for (auto& t : need) hours.insert(t.hour);
      std::set<std::string> held;
      for (auto h : hours)
        for (auto& [k, e] : bucket::list_keys(hour_prefix(h))) held.insert(hour_prefix(h) + k);
      std::vector<std::pair<std::string, fs::path>> items;
      std::vector<Tile> rest;
      for (auto& t : need) {
        if (held.count(tile_key(t))) items.push_back({tile_key(t), tile_path(t)});
        else rest.push_back(t);
      }
      const auto got = bucket::get_objects(items);
      st.bucket += got.size();
      for (auto& [k, p] : items)
        if (!got.count(k)) rest.push_back(tile_of_key(k));
      need = std::move(rest);
    } catch (const std::exception& e) {
      st.note = std::string("bucket: ") + e.what();
    }
  }
  if (!need.empty() && query && has_credentials()) {
    try {
      need = fetch_tiles(std::move(need), max_queries, push, st);
    } catch (const std::exception& e) {
      st.note = std::string("OpenSky: ") + e.what();
    }
  }
  st.missing += need.size();
  return need;
}

void queue_missing(const std::vector<Tile>& need, TileStats& st) {
  if (need.empty() || !bucket::enabled()) return;
  try {
    enqueue(need);
    st.queued = true;
  } catch (const std::exception& e) {
    std::cerr << "opensky: could not queue the missing tiles: " << e.what() << std::endl;
  }
}

}  // namespace

TileStats prefetch(const Bbox& box, double t0, double t1, const Options& o, bool query) {
  TileStats st;
  const auto need = ensure_tiles(tiles_for(box, t0, t1), query, o.max_queries, o.push, st);
  if (query) queue_missing(need, st);
  return st;
}

std::vector<Track> fetch(const Window& w, std::string* source, bool push) {
  TileStats st;
  const auto all = tiles_for(w.box, double(w.t0), double(w.t1));
  const auto need = ensure_tiles(all, true, 0, push, st);
  if (!need.empty()) {
    queue_missing(need, st);
    if (!has_credentials())
      throw NotConfigured(std::to_string(need.size()) + " OpenSky tile(s) are neither here nor in the bucket, and OPENSKY_USERNAME / OPENSKY_PASSWORD are not set" +
                          (st.queued ? " (queued for `geoloc aircraft opensky-worker`)" : ""));
    throw std::runtime_error(st.note.empty() ? "tiles missing" : st.note);
  }
  if (source) *source = st.fetched ? "opensky" : st.bucket ? "bucket" : st.skipped == all.size() ? "day from OpenSky" : "cache";
  // A day built from OpenSky already holds these tracks: ADSB.lol's source returns them.
  return tracks_of_tiles(all, double(w.t0), double(w.t1), w.box);
}

std::vector<Track> load_tiles(const Bbox& box, double t0, double t1) { return tracks_of_tiles(tiles_for(box, t0, t1), t0, t1, box); }

TrackSource with_tiles(TrackSource adsb) {
  return [adsb = std::move(adsb)](const Bbox& b, double t0, double t1) {
    auto ad = adsb(b, t0, t1);
    auto os = load_tiles(b, t0, t1);
    if (os.empty()) return ad;
    return merge(ad, os).segments;
  };
}

void enqueue(const std::vector<Tile>& tiles) {
  std::vector<Tile> ts = tiles;
  std::sort(ts.begin(), ts.end());
  json::array arr;
  std::string ident;
  for (auto& t : ts) {
    arr.push_back(json::array{t.hour, t.lat, t.lon});
    ident += std::to_string(t.hour) + ":" + std::to_string(t.lat) + ":" + std::to_string(t.lon) + ";";
  }
  const std::string key = "opensky/requests/" + sha256_hex(ident).substr(0, 24) + ".json";
  TempFile tmp(".json");
  atomic_write(tmp.path, dump(json::object{{"created", now_isoformat()}, {"tiles", arr}}));
  bucket::put_object(key, tmp.path);
}

int work_queue(const std::function<void(const std::string&)>& progress) {
  auto say = [&](const std::string& m) {
    if (progress) progress(m);
  };
  int done = 0;
  for (auto& [k, etag] : bucket::list_keys("opensky/requests/")) {
    const std::string key = "opensky/requests/" + k;
    TempFile tmp(".json");
    if (!bucket::get_object(key, tmp.path)) continue;
    std::vector<Tile> tiles;
    for (auto& v : parse_json(*read_file(tmp.path)).at("tiles").as_array()) {
      auto& a = v.as_array();
      tiles.push_back({std::int64_t(as_number(a.at(0))), int(as_number(a.at(1))), int(as_number(a.at(2)))});
    }
    Options o;
    o.max_queries = 0, o.push = true;
    // What the bucket or this machine has already is only uploaded or skipped.
    std::vector<Tile> need, upload;
    std::set<std::int64_t> hours;
    for (auto& t : tiles) hours.insert(t.hour);
    std::set<std::string> held;
    for (auto h : hours)
      for (auto& [kk, e] : bucket::list_keys(hour_prefix(h))) held.insert(hour_prefix(h) + kk);
    for (auto& t : tiles) {
      if (held.count(tile_key(t)) || day_from_opensky(t.hour)) continue;
      (fs::exists(tile_path(t)) ? upload : need).push_back(t);
    }
    TileStats st;
    say("request " + k + ": " + std::to_string(tiles.size()) + " tile(s), " + std::to_string(need.size()) + " to fetch, " + std::to_string(upload.size()) +
        " to upload");
    std::vector<std::pair<std::string, fs::path>> items;
    for (auto& t : upload) items.push_back({tile_key(t), tile_path(t)});
    if (!items.empty()) bucket::put_objects(items);
    fetch_tiles(need, 0, true, st);
    bucket::delete_object(key);
    say("request " + k + " done: " + std::to_string(st.queries) + " queries, " + std::to_string(st.rows) + " rows");
    ++done;
  }
  return done;
}

std::string describe(const TileStats& st) {
  char b[300];
  std::snprintf(b, sizeof b, "OpenSky tiles: %zu needed (hour x 1 deg), %zu here, %zu from the bucket, %zu fetched (%d queries), %zu missing", st.tiles,
                st.local, st.bucket, st.fetched, st.queries, st.missing);
  std::string out = b;
  if (st.queued) out += " (queued for the worker)";
  if (st.skipped) out += ", " + std::to_string(st.skipped) + " in days built from OpenSky";
  if (!st.note.empty()) out += "; " + st.note;
  return out;
}

// ---------------------------------------------------------------- reporting

json::object report_json(const Report& rep) {
  json::array places, queries;
  for (auto& p : rep.places) {
    json::array ac;
    for (auto& a : p.aircraft)
      ac.push_back(json::object{{"flight", a.flight},
                                {"type", a.type},
                                {"registration", a.reg},
                                {"only_opensky", a.only_opensky},
                                {"closest_m", num_or_null(a.closest_m)},
                                {"closest_utc", std::isfinite(a.closest_utc) ? json::value(iso(a.closest_utc)) : json::value(nullptr)},
                                {"unheard", a.unheard}});
    json::object o{{"rank", p.rank},
                   {"lat", p.lat},
                   {"lon", p.lon},
                   {"start_utc", iso(p.start_utc)},
                   {"score", p.score},
                   {"flights", p.combo},
                   {"outcome", p.outcome},
                   {"detail", p.detail},
                   {"adsb_segments", p.adsb_segments},
                   {"opensky_segments", p.opensky_segments},
                   {"aircraft", ac}};
    if (p.replacement)
      o["replacement"] = json::object{{"lat", p.replacement->lat}, {"lon", p.replacement->lon}, {"score", p.replacement->score}, {"flights", p.replacement->combo}};
    places.push_back(std::move(o));
  }
  for (auto& q : rep.queries) queries.push_back(json::value(q));
  json::value tiles = nullptr;
  if (rep.tiles)
    tiles = json::object{{"tiles", rep.tiles->tiles},     {"local", rep.tiles->local},   {"bucket", rep.tiles->bucket},
                         {"fetched", rep.tiles->fetched}, {"missing", rep.tiles->missing}, {"skipped_opensky_days", rep.tiles->skipped},
                         {"queries", rep.tiles->queries}, {"queued", rep.tiles->queued}, {"note", rep.tiles->note}};
  return {{"status", rep.status},     {"note", rep.note},       {"places", places},
          {"dropped", rep.dropped},   {"changed", rep.changed}, {"queries", queries},
          {"tiles", tiles},
          {"source", "The OpenSky Network, https://opensky-network.org (non-commercial research use)"}};
}

std::string render_report(const Report& rep) {
  std::string out = "  OpenSky: " + rep.status + (rep.note.empty() ? "" : ": " + rep.note) + "\n";
  if (rep.tiles) out += "    " + describe(*rep.tiles) + "\n";
  char b[512];
  for (auto& p : rep.places) {
    std::snprintf(b, sizeof b, "    #%d %.5f, %.5f (%s): %s%s%s; segments ADSB.lol %zu, OpenSky %zu\n", p.rank + 1, p.lat, p.lon, p.combo.c_str(),
                  p.outcome.c_str(), p.detail.empty() ? "" : ", ", p.detail.c_str(), p.adsb_segments, p.opensky_segments);
    out += b;
    for (auto& a : p.aircraft) {
      std::snprintf(b, sizeof b, "      %s %s %s (%s): closest %.0f m at %s%s\n", a.flight.c_str(), a.type.empty() ? "?" : a.type.c_str(), a.reg.c_str(),
                    a.only_opensky ? "only OpenSky" : "gaps filled by OpenSky", a.closest_m,
                    std::isfinite(a.closest_utc) ? iso(a.closest_utc).c_str() : "?", a.unheard ? ", unheard: silence" : "");
      out += b;
    }
  }
  return out;
}

}  // namespace geo::air::opensky
