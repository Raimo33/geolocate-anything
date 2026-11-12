#include "refs/seismic_fetch.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <numbers>
#include <iostream>
#include <map>
#include <optional>
#include <set>
#include <thread>

#include "net/offload.hpp"
#include "refs/mseed.hpp"
#include "refs/seismic.hpp"
#include "util/files.hpp"
#include "util/text.hpp"
#include "util/time.hpp"

namespace geo::seismic {

namespace asio = boost::asio;
namespace json = boost::json;

namespace {

constexpr const char* DATASELECT = "https://service.earthscope.org/fdsnws/dataselect/1/query";
constexpr const char* SHAKE_DATASELECT = "https://data.raspberryshake.org/fdsnws/dataselect/1/query";
constexpr i64 CHUNK_S = 6 * 3600;  // one request per station and 6 h
// Raspberry Shake: one request per station-day. The archive streams ~0.5 MB/s per request
// whatever its length but answers 429 past ~one request every 1.5-2 s, so a day per request
// moves 4x the data under the same limit (and a quarter of the padding).
constexpr i64 SHAKE_CHUNK_S = 24 * 3600;
constexpr int SHAKE_IN_FLIGHT = 10;  // Raspberry Shake requests in flight (see fetch_month)
constexpr i64 PROBE_S = 24 * 3600;  // Raspberry Shake: one chunk a day first (see fetch_month)
constexpr i64 PAD_S = 10;          // frame half-length + filter, either side

namespace east_table {
#include "refs/seismic_stations.inc"
}
namespace west_table {
#include "refs/seismic_west_stations.inc"
}
namespace ercot_table {
#include "refs/seismic_ercot_stations.inc"
}
namespace ips_table {
#include "refs/seismic_ips_stations.inc"
}
namespace cn_south_table {
#include "refs/seismic_cn_south_stations.inc"
}
namespace cn_north_table {
#include "refs/seismic_cn_north_stations.inc"
}
namespace cn_east_table {
#include "refs/seismic_cn_east_stations.inc"
}
namespace cn_southwest_table {
#include "refs/seismic_cn_southwest_stations.inc"
}
namespace swis_table {
#include "refs/seismic_swis_stations.inc"
}
namespace darwin_table {
#include "refs/seismic_darwin_stations.inc"
}
namespace perth_basin_table {
#include "refs/seismic_perth_basin_stations.inc"
}
namespace brazil_table {
#include "refs/seismic_brazil_stations.inc"
}
namespace estonia_table {
#include "refs/seismic_estonia_stations.inc"
}
const std::vector<Station>& cn_south_list() { return cn_south_table::STATIONS; }
const std::vector<Station>& cn_north_list() { return cn_north_table::STATIONS; }
const std::vector<Station>& cn_east_list() { return cn_east_table::STATIONS; }
const std::vector<Station>& cn_southwest_list() { return cn_southwest_table::STATIONS; }
const std::vector<Station>& swis_list() { return swis_table::STATIONS; }
const std::vector<Station>& darwin_list() { return darwin_table::STATIONS; }
const std::vector<Station>& perth_basin_list() { return perth_basin_table::STATIONS; }
const std::vector<Station>& east_list() { return east_table::STATIONS; }
const std::vector<Station>& west_list() { return west_table::STATIONS; }
const std::vector<Station>& ercot_list() { return ercot_table::STATIONS; }
const std::vector<Station>& ips_list() { return ips_table::STATIONS; }
const std::vector<Station>& brazil_list() { return brazil_table::STATIONS; }
const std::vector<Station>& estonia_list() { return estonia_table::STATIONS; }

std::string iso(double t) {
  const i64 s = i64(std::floor(t));
  const i64 day = floor_div(s, 86400), sod = s - day * 86400;
  auto c = civil_from_days(day);
  char b[32];
  std::snprintf(b, sizeof b, "%04d-%02d-%02dT%02d:%02d:%02d", int(c.y), int(c.m), int(c.d), int(sod / 3600), int(sod / 60 % 60), int(sod % 60));
  return b;
}

std::string day_of(i64 t) { return iso(double(t)).substr(0, 10); }

// EarthScope account access. Anonymous requests share a quota per IP (10 GiB an hour,
// 50 GiB a day); with an account's token it is 200 GiB an hour and 1 TiB a day, which a
// backfill needs (~2 GB a month). GEOLOC_EARTHSCOPE_REFRESH_TOKEN (from `es login`, then
// `es user get-refresh-token`) is exchanged for access tokens the way the EarthScope SDK
// does, again shortly before each expires.
constexpr const char* ES_TOKEN_URL = "https://login.earthscope.org/oauth/token";
constexpr const char* ES_CLIENT_ID = "b9DtAFBd6QvMg761vI3YhYquNZbJX5G0";  // the SDK's public client
struct EsAuth {
  std::string refresh = env("GEOLOC_EARTHSCOPE_REFRESH_TOKEN");
  std::string access;
  double expires = 0;
  bool announced = false;
  bool refreshing = false;  // one exchange at a time: the others wait for its token
  std::string failed;  // a refused refresh token is not tried again
};
EsAuth& es_auth() {
  static EsAuth a;
  return a;
}

// "Authorization: Bearer ..." for the next request, or "" when no account is set up.
asio::awaitable<std::string> es_bearer(net::Http& http) {
  EsAuth& a = es_auth();
  if (a.refresh.empty()) co_return std::string();
  while (a.refreshing) co_await net::sleep_for(1.0);
  if (!a.failed.empty()) throw std::runtime_error(a.failed);
  if (a.access.empty() || now_seconds() > a.expires - 300) {
    a.refreshing = true;
    struct Done {
      EsAuth& a;
      ~Done() { a.refreshing = false; }
    } done{a};
    // Transport errors, 408, 429 and 5xx are passing trouble (a dropped connection, a busy
    // login server): retried with a doubling wait (15 s up to 10 min) for up to an hour,
    // like the downloads, instead of failing every request of the month at once.
    net::Response r;
    double waited = 0;
    for (int attempt = 0;; ++attempt) {
      net::Request rq;
      rq.url = ES_TOKEN_URL;
      rq.method = "POST";
      rq.headers = {"Content-Type: application/x-www-form-urlencoded"};
      rq.body = "grant_type=refresh_token&client_id=" + std::string(ES_CLIENT_ID) + "&refresh_token=" + url_encode(a.refresh) +
                "&scopes=offline_access";
      rq.total_timeout_s = 60;
      r = co_await http.fetch(std::move(rq));
      const bool passing = !r.error.empty() || r.status == 408 || r.status == 429 || r.status >= 500;
      if (!passing) break;
      if (waited >= 3600) {  // an hour without a token: give up for this process, not once per request
        a.failed = "EarthScope login failed for an hour (" + (r.error.empty() ? "HTTP " + std::to_string(r.status) : r.error) + ")";
        break;
      }
      double delay = std::min(600.0, 15.0 * double(1 << std::min(attempt, 6)));
      const std::string ra = r.header("retry-after");
      if (!ra.empty() && std::all_of(ra.begin(), ra.end(), ::isdigit)) delay = std::clamp(std::stod(ra), 1.0, 1800.0);
      std::cerr << "  seismic: EarthScope login: " << (r.error.empty() ? "HTTP " + std::to_string(r.status) : r.error)
                << ", retrying in " << int(delay) << " s" << std::endl;
      waited += delay;
      co_await net::sleep_for(delay);
    }
    std::string access;
    double ttl = 0;
    if (r.ok()) try {
        auto v = json::parse(r.body).as_object();
        access = std::string(v.at("access_token").as_string());
        ttl = v.contains("expires_in") ? v.at("expires_in").to_number<double>() : 3600;
        if (auto* nr = v.if_contains("refresh_token"); nr && nr->is_string()) a.refresh = std::string(nr->as_string());  // rotating
      } catch (const std::exception&) {
        access.clear();
      }
    if (access.empty() && r.error.empty() && r.status >= 400 && r.status < 500)
      a.failed = "EarthScope login failed (HTTP " + std::to_string(r.status) + " " + r.body.substr(0, 200) +
                 "): renew GEOLOC_EARTHSCOPE_REFRESH_TOKEN with `es login` and `es user get-refresh-token`";
    if (!a.failed.empty()) throw std::runtime_error(a.failed);
    if (access.empty())
      throw std::runtime_error("EarthScope login failed (" + (r.error.empty() ? "HTTP " + std::to_string(r.status) + " " + r.body.substr(0, 200) : r.error) +
                               "): renew GEOLOC_EARTHSCOPE_REFRESH_TOKEN with `es login` and `es user get-refresh-token`");
    a.access = std::move(access), a.expires = now_seconds() + ttl;
    if (!a.announced) std::cerr << "  seismic: using the EarthScope account's quota (token valid " << int(ttl / 60) << " min, renewed as needed)" << std::endl;
    a.announced = true;
  }
  co_return "Authorization: Bearer " + a.access;
}

// One dataselect request, patiently: EarthScope answers 429 when a client goes over its
// limits (at most 5 concurrent connections, per its usage guidelines), so 429, 5xx and
// transport errors are retried as Retry-After says, else with a doubling wait (15 s up
// to 10 min), for up to an hour in all. Other 4xx answers won't change and throw at once.
// The Raspberry Shake archive refuses bursts (429 after ~20 requests at 10 a second, none
// in a short run at 2 a second) and, over a sustained run, refuses well under one a
// second as well (2026-11-11: ten requests in flight at one a second ran into 429s and
// the backfill stalled behind back-offs). Requests start at most every `shake_gap` in a
// process: 1 s, stretched by half at every 429 (up to 8 s) and shortened by 2% at every
// answer, so a process settles at the rate the archive takes. Event loop only.
double shake_gap = 1.0;
void shake_feedback(bool refused) {
  shake_gap = refused ? std::min(8.0, shake_gap * 1.5) : std::max(1.0, shake_gap * 0.98);
}
asio::awaitable<void> shake_pace() {
  static std::chrono::steady_clock::time_point next{};
  const auto now = std::chrono::steady_clock::now();
  const auto at = std::max(now, next);
  next = at + std::chrono::milliseconds(std::lround(shake_gap * 1000));
  if (at > now) co_await net::sleep_for(std::chrono::duration<double>(at - now).count());
}

asio::awaitable<net::Response> get_patiently(net::Http& http, const std::string& url, bool earthscope) {
  double waited = 0;
  for (int attempt = 0;; ++attempt) {
    net::Request rq;
    rq.url = url;
    rq.total_timeout_s = 600;
    if (earthscope)  // the account's token goes to EarthScope only
      if (std::string auth = co_await es_bearer(http); !auth.empty()) rq.headers.push_back(std::move(auth));
    net::Response r = co_await http.fetch(std::move(rq));
    const bool shake = url.starts_with(SHAKE_DATASELECT);
    if (shake && r.error.empty()) shake_feedback(r.status == 429);
    if (r.ok() || (r.error.empty() && r.status == 204)) co_return r;
    if (earthscope && r.error.empty() && r.status == 401 && attempt < 2) {  // token revoked or expired early: get a new one
      es_auth().access.clear();
      continue;
    }
    const std::string why = r.error.empty() ? "HTTP " + std::to_string(r.status) : r.error;
    const bool retry = !r.error.empty() || r.status == 429 || r.status == 408 || r.status >= 500;
    if (!retry || waited >= 3600) throw std::runtime_error("download failed: " + url + ": " + why);
    // EarthScope: 15 s doubling. The Raspberry Shake archive answers bursts with 429 (no
    // Retry-After) and recovers within seconds: 5 s doubling there.
    double delay = std::min(600.0, (earthscope ? 15.0 : 5.0) * double(1 << std::min(attempt, 6)));
    const std::string ra = r.header("retry-after");
    if (!ra.empty() && std::all_of(ra.begin(), ra.end(), ::isdigit)) delay = std::clamp(std::stod(ra), 1.0, 1800.0);
    if (shake) {  // the shared pace does the waiting: a short pause, then a turn like any request
      delay = 5;
      std::fprintf(stderr, "  [shake] %s, retrying; requests now %.1f s apart\n", why.c_str(), shake_gap);
    }
    waited += delay;
    co_await net::sleep_for(delay);
    if (shake) co_await shake_pace();
  }
}

// Score of a station for a year: its nearest screened year's r (0 when never screened).
float score_for(const Station& s, int year) {
  float best = 0;
  int dist = 1 << 20;
  for (auto [y, r] : s.score)
    if (std::abs(y - year) < dist) dist = std::abs(y - year), best = r;
  return dist <= 3 ? best : best * 0.9f;
}

// Kilometres between two stations (flat-earth, for short distances).
double km_apart(const Station& a, const Station& b) {
  const double dlat = (a.lat - b.lat) * 111.2, dlon = (a.lon - b.lon) * 111.2 * std::cos(a.lat * std::numbers::pi / 180);
  return std::sqrt(dlat * dlat + dlon * dlon);
}

}  // namespace

// US Eastern: dense and steady, so the three best stations carry a month and a very quiet
// one may stand alone. Up to five reserves: in thin years (2012) the third and fourth
// pick are often silent, and 2012-06 rose from 61% to 75% with the reserves 6-8.
const Network& east() {
  static const Network n{"seismic_east", 60, "", 0.9f, 3, 8, true, &east_list};
  return n;
}
// US Western, from 2001-09 (the first month EarthScope holds two listed stations). AESO's
// measured series covers 2019-01..2025-07, so those months are not reconstructed. Nobody
// stands alone: strong stations at times carry a line that is not the grid's (UW.DPW in
// five of its probes), and there are always others to ask.
const Network& west() {
  static const Network n = [] {
    Network n{"seismic_west", 60, "2001-09", 0.9f, 3, 5, false, &west_list};
    n.skip_from = "2019-01", n.skip_to = "2025-07";
    n.same_site_km = 0.5f;
    return n;
  }();
  return n;
}
// ERCOT (Texas), from 2009-06 (the first month two listed stations ran). Through 2015-01
// often only one site hums (Azle, at 30-50 dB), so it stands alone. From then on two
// sites must agree: the stations in the Permian Basin oilfields
// at times carry a strong line that is not the grid's (TX.PB46 on 2025-05-15, 32 dB).
const Network& ercot() {
  static const Network n = [] {
    Network n{"seismic_ercot", 60, "2009-06", 0.9f, 3, 5, true, &ercot_list};
    n.lone_before = "2015-02";
    n.same_site_km = 0.5f;
    return n;
  }();
  return n;
}
// IPS/UPS: few stations, each humming only part of the time (score = the share of the
// year's monthly probes in which it agreed with another site), and one of them runs on a
// generator at times, so more are fetched and none is believed alone.
const Network& ips() {
  static const Network n{"seismic_ips", 50, "2022-10", 0.05f, 4, 6, false, &ips_list};
  return n;
}
// China: Raspberry Shakes come and go and hum by spells, so every listed station that
// ever showed the line is tried (score = the share of the year's probes with the line).
// Two stations that agree are believed; one alone only when its line is strong (13 dB:
// 9 mHz by the fusion's noise law) and the roll-off test holds.
namespace {
Network shake(const char* source, const char* first, const std::vector<Station>& (*list)()) {
  Network n{source, 50, first, 0.01f, 4, 6, true, list};
  n.nyquist = true, n.agree_r = 0.7f, n.lone_noise_mhz = 9.0f, n.archive = "Raspberry Shake";
  return n;
}
}  // namespace
const Network& cn_south() {
  static const Network n = shake("seismic_cn_south", "2019-07", &cn_south_list);
  return n;
}
const Network& cn_north() {
  static const Network n = shake("seismic_cn_north", "2019-05", &cn_north_list);
  return n;
}
const Network& cn_east() {
  static const Network n = shake("seismic_cn_east", "2023-09", &cn_east_list);
  return n;
}
const Network& cn_southwest() {
  static const Network n = shake("seismic_cn_southwest", "2021-03", &cn_southwest_list);
  return n;
}
// Western Australia's South West Interconnected System, from Raspberry Shakes in and
// around Perth (Toodyay to Harvey, 150 km). Several units share an owner and a rounded
// position, so stations within a kilometre count as one site.
const Network& swis() {
  static const Network n = [] {
    Network n = shake("seismic_swis", "2019-12", &swis_list);
    n.same_site_km = 1.0f;
    return n;
  }();
  return n;
}
// The Darwin-Katherine system: one Raspberry Shake in Darwin at a time, so nobody to
// confirm it (as seismic_cn_southwest).
const Network& darwin() {
  static const Network n = [] {
    Network n = shake("seismic_darwin", "2018-02", &darwin_list);
    n.same_site_km = 1.0f;  // AM.R9544 and AM.RFE60 share an address: one of them, not a pair
    return n;
  }();
  return n;
}
// The same grid in 2017-01..06, read directly: the Perth Basin Array (network 5C, at
// AusPass) recorded at 250 samples/s with GPS time, and eight of its sites between
// Regans Ford and Kwinana (136 km) carry the 50 Hz line. Nobody stands alone: field stations on
// solar power hum at their own inverter's 50.000 Hz (the 2P array, 2020-2023).
const Network& perth_basin() {
  static const Network n = [] {
    Network n{"seismic_perth_basin", 50, "2017-01", 0.02f, 4, 6, false, &perth_basin_list};
    n.archive = "AusPass";
    n.dataselect = "https://auspass.edu.au/fdsnws/dataselect/1/query";
    n.licence = "AusPass (ANU) open FDSN data, network 5C: Murdie, Yuan & Dentith (2017), Perth Basin Array (doi:10.7914/nx28-a367)";
    return n;
  }();
  return n;
}
// Brazil's SIN, 2004-02..2008-08: one recorder at the University of São Paulo whose
// 100 samples/s channels fold the 60 Hz line to 40 Hz. BL.CAM1 is an input with no live
// sensor that carries the hum at ~45 dB; BL.CAM4, a working seismometer on another input,
// carries it more weakly and confirms it when it can. With one site there is nobody else
// to ask, so CAM1 stands alone when CAM4 is too weak (docs/grids/seismic_brazil.md).
const Network& brazil() {
  static const Network n = [] {
    Network n{"seismic_brazil", 60, "2004-02", 0.0f, 2, 2, true, &brazil_list};
    n.archive = "IAG/USP";
    n.dataselect = "https://seisrequest.iag.usp.br/fdsnws/dataselect/1/query";
    n.licence = "Centro de Sismologia da USP (IAG/USP) open FDSN data, network BL (doi:10.7914/SN/BL)";
    n.timestamps = "UTC from the recorder's clock, not known to be GPS-disciplined; within about 2 s of UTC where earthquake arrivals "
                   "could be checked (2005-06, 2007-11); each value is the 12 s frame centred on the middle of its second";
    n.robust_floor = true;
    return n;
  }();
  return n;
}
// IPS/UPS again, 2015-05..2025-02, from Estonia, which was part of it until the Baltic
// states left on 2025-02-08 07:08 UTC: the Estonian national network (EE, at GEOFON)
// records at 250 samples/s with GPS time, so the 50 Hz line is read directly. Its sites
// hum by spells (mostly at night), several at once at times; none is believed alone. The
// stations renamed in 2023 (EE01 -> NOPE, EE03 -> TOSE, EE07 -> PISE, EE08 -> SRGE) stay
// at their sites, so stations within a kilometre count as one site.
const Network& estonia() {
  static const Network n = [] {
    Network n{"seismic_estonia", 50, "2015-05", 0.02f, 4, 6, false, &estonia_list};
    n.archive = "GEOFON";
    n.dataselect = "https://geofon.gfz.de/fdsnws/dataselect/1/query";
    n.licence = "GEOFON open FDSN data, network EE: Estonian National Seismic Network, Geological Survey of Estonia (doi:10.14470/RR425351)";
    n.same_site_km = 1.0f;
    n.until = 1738998480;  // 2025-02-08 07:08:00 UTC (the Baltic grids' own recorders, osf_baltic)
    return n;
  }();
  return n;
}
const Network* network(std::string_view source) {
  for (const Network* n : {&east(), &west(), &ercot(), &ips(), &cn_south(), &cn_north(), &cn_east(), &cn_southwest(), &swis(), &darwin(), &perth_basin(), &brazil(), &estonia()})
    if (source == n->source) return n;
  return nullptr;
}

std::vector<std::string> months(const Network& net) {
  std::string first = "9999", last;
  bool open = false;
  for (auto& s : net.stations()) {
    first = std::min(first, std::string(s.from).substr(0, 7));
    if (!*s.to) open = true;
    else {
      int y = 0, m = 0, d = 0;
      std::sscanf(s.to, "%d-%d-%d", &y, &m, &d);
      last = std::max(last, month_key(days_from_civil(y, unsigned(m), unsigned(d)) * 86400 - 1));  // `to` is exclusive
    }
  }
  std::string now = month_key(i64(now_seconds()));
  if (net.until) now = std::min(now, month_key(net.until));
  auto all = months_between(std::max(first, std::string(net.first_month)), open ? now : std::min(now, last));
  if (net.skip_from && net.skip_to) std::erase_if(all, [&](const std::string& m) { return m >= net.skip_from && m <= net.skip_to; });
  return all;
}

asio::awaitable<MonthResult> fetch_month(net::Http& http, const Network& net, const std::string& month, std::function<bool(i64, i64)> skip) {
  // Requests in flight: EarthScope allows 5 concurrent connections per client. The
  // Raspberry Shake archive takes 6 to 10 s to answer each 6 h request, however many are
  // in flight (six at once each took as long as one alone), so there SHAKE_IN_FLIGHT are
  // in flight (still starting at most one a second, shake_pace) while only `decode_slots`
  // chunks decode at once (GEOLOC_THREADS, default: all cores, 4 to 8). A chunk waiting to
  // decode holds its download, a few MB.
  int parallel = 4, decode_slots = 1 << 30, decoding = 0;
  if (net.nyquist) {
    const std::string t = env("GEOLOC_THREADS");
    decode_slots = !t.empty() ? std::clamp(std::atoi(t.c_str()), 1, 32) : std::clamp(int(std::thread::hardware_concurrency()), 4, 8);
    parallel = std::max(decode_slots, SHAKE_IN_FLIGHT);
  }
  const int base_stations = net.base_stations, max_stations = net.max_stations;
  const std::string name = net.source;
  const bool nyquist = net.nyquist;
  const i64 chunk_s = nyquist ? SHAKE_CHUNK_S : CHUNK_S;
  const int probe_every = int(std::max<i64>(1, PROBE_S / chunk_s));
  TrackOptions topt;
  topt.nominal_hz = topt.mains_hz = net.mains_hz;
  topt.robust_floor = net.robust_floor;
  NyquistOptions nopt;
  nopt.mains_hz = net.mains_hz;
  FuseOptions fopt;
  fopt.agree_r = net.agree_r;
  fopt.lone_max_noise_mhz = net.lone && (!net.lone_before || month < net.lone_before) ? net.lone_noise_mhz : 0;
  const std::string dataselect = net.dataselect ? net.dataselect : net.nyquist ? SHAKE_DATASELECT : DATASELECT;
  const bool earthscope = dataselect == DATASELECT;  // the account's token goes there only
  const i64 pad = net.nyquist ? NYQUIST_PAD_S + 5 : PAD_S;
  auto [t0, t1] = month_bounds(month);
  const i64 now = i64(now_seconds());
  if (t0 > now) throw NoData(month + " is in the future");
  i64 end = std::min(t1, now - 3600);  // the archive lags real time
  if (net.until) {
    if (net.until <= t0) throw NoData(month + " is after " + name + "'s stations left the grid");
    end = std::min(end, net.until);
  }
  const int year = std::stoi(month.substr(0, 4));
  const std::string d0 = day_of(t0), d1 = day_of(t1 - 1);
  // The best-scoring stations whose epoch overlaps the month.
  std::vector<const Station*> cand;
  for (auto& s : net.stations())
    if (std::string(s.from) <= d1 && (!*s.to || std::string(s.to) > d0)) cand.push_back(&s);
  std::stable_sort(cand.begin(), cand.end(), [&](auto* a, auto* b) { return score_for(*a, year) > score_for(*b, year); });
  // One channel per site (a site's other channel sees the same building's hum): the
  // first `base` are fetched for the whole month, the rest are reserves. Where the
  // network says so (same_site_km) a site is also a place: arrays and re-named stations
  // share coordinates under different codes (Garner Valley's SB.GVA03/04/06, ZW.IPD1 and
  // ZW.IPD1V), and two channels in one building confirm nothing about the grid.
  std::vector<const Station*> pick;
  for (auto* s : cand) {
    const std::string sid = s->id, site = sid.substr(0, sid.find('.', sid.find('.') + 1));
    bool dup = false;
    for (auto* p : pick) dup |= std::string(p->id).rfind(site + ".", 0) == 0 || km_apart(*p, *s) < net.same_site_km;
    if (!dup && score_for(*s, year) >= net.min_score) pick.push_back(s);
    if (int(pick.size()) >= max_stations) break;
  }
  if (pick.empty()) throw NoData("no screened station covers " + month);

  const std::size_t n = std::size_t(t1 - t0);
  std::vector<Track> tracks(pick.size());
  for (auto& t : tracks) t.t0 = t0, t.f_mhz.assign(n, NAN), t.snr_db.assign(n, NAN);
  struct Job {
    std::size_t st;
    i64 a, b;
  };
  int failed = 0, empty = 0, n_jobs = 0;
  std::set<i64> failed_chunks;  // chunk starts where a download or decode failed
  std::string last_err;
  std::map<std::string, i64> bytes;
  std::map<std::size_t, int> failed_by;  // per station
  auto run = [&](std::vector<Job> jobs) -> asio::awaitable<void> {
    n_jobs += int(jobs.size());
    co_await net::parallel_for(std::move(jobs), parallel, [&](Job j) -> asio::awaitable<void> {
      if (nyquist) co_await shake_pace();
      const std::string id = pick[j.st]->id;
      const auto p1 = id.find('.'), p2 = id.find('.', p1 + 1), p3 = id.find('.', p2 + 1);
      const std::string net = id.substr(0, p1), sta = id.substr(p1 + 1, p2 - p1 - 1), loc = id.substr(p2 + 1, p3 - p2 - 1), cha = id.substr(p3 + 1);
      const std::string url = dataselect + "?net=" + net + "&sta=" + sta + "&loc=" + (loc.empty() ? "--" : loc) + "&cha=" + cha +
                              "&start=" + iso(double(j.a - pad)) + "&end=" + iso(double(j.b + pad));
      net::Response r;
      try {
        r = co_await get_patiently(http, url, earthscope);
      } catch (const std::exception& e) {
        ++failed, ++failed_by[j.st], last_err = e.what(), failed_chunks.insert(j.a);
        co_return;
      }
      if (r.status == 204 || r.body.empty()) {
        ++empty;
        co_return;
      }
      bytes[id] += i64(r.body.size());
      // Decoding and tracking are the CPU work (minutes per station-day for the Nyquist
      // decoder): they run on a thread of their own, so the chunks in flight use as
      // many cores.
      std::string body = std::move(r.body);
      std::optional<Track> tracked;
      std::string decode_err;
      // (named, not inline: GCC 15 has an internal error on braced lists inside co_await)
      auto work = [&]() -> std::optional<Track> {
        std::vector<mseed::Segment> segs;
        try {
          auto all = mseed::decode({reinterpret_cast<const std::uint8_t*>(body.data()), body.size()});
          for (auto& sg : all)
            if (sg.id == id) segs.push_back(std::move(sg));
        } catch (const std::exception& e) {
          decode_err = e.what();
          return std::nullopt;
        }
        std::string().swap(body);
        return nyquist ? track_nyquist(segs, j.a, j.b, nopt) : track_hum(segs, j.a, j.b, topt);
      };
      while (decoding >= decode_slots) co_await net::sleep_for(0.05);  // one event loop: no race
      ++decoding;
      try {
        tracked = co_await net::run_in_thread(std::move(work));
      } catch (...) {
        --decoding;
        throw;
      }
      --decoding;
      if (!tracked) {
        ++failed, ++failed_by[j.st], last_err = std::string("decode ") + id + ": " + decode_err, failed_chunks.insert(j.a);
        co_return;
      }
      const Track& tr = *tracked;
      auto& dst = tracks[j.st];
      std::copy(tr.f_mhz.begin(), tr.f_mhz.end(), dst.f_mhz.begin() + (j.a - t0));
      std::copy(tr.snr_db.begin(), tr.snr_db.end(), dst.snr_db.begin() + (j.a - t0));
    });
  };
  // Raspberry Shakes are often offline for whole months: one chunk a day is asked of
  // each station first, and a station that answered none of them with data (and had no
  // failed download) is not asked for the rest. A station online only between the
  // probed hours of every day is missed; anything longer is not.
  auto run_probed = [&](std::vector<Job> jobs) -> asio::awaitable<void> {
    if (!nyquist) {
      co_await run(std::move(jobs));
      co_return;
    }
    std::map<std::size_t, int> seen;
    std::vector<Job> probe, rest;
    for (auto& j : jobs) (seen[j.st]++ % probe_every == 0 ? probe : rest).push_back(j);
    co_await run(std::move(probe));
    std::erase_if(rest, [&](const Job& j) { return bytes[pick[j.st]->id] == 0 && !failed_by[j.st]; });
    co_await run(std::move(rest));
  };
  std::vector<std::string> ids;
  for (auto* s : pick) ids.push_back(s->id);
  const std::size_t base = std::min<std::size_t>(std::size_t(base_stations), pick.size());
  std::vector<std::pair<i64, i64>> chunks;  // the ones to fetch
  int skipped = 0;
  for (i64 a = t0; a < end; a += chunk_s) {
    const i64 b = std::min(a + chunk_s, end);
    if (skip && skip(a, b)) ++skipped;
    else chunks.push_back({a, b});
  }
  {
    std::vector<Job> jobs;
    for (std::size_t k = 0; k < base; ++k)
      for (auto [a, b] : chunks) jobs.push_back({k, a, b});
    co_await run_probed(std::move(jobs));
  }
  // Reserves, chunk by chunk, where fewer than `base` stations backed the fused value for
  // more than a tenth of the chunk (a station silent, noisy or out of agreement).
  FuseStats st;
  int reserve_chunks = 0;
  for (std::size_t k = base; k < pick.size(); ++k) {
    fuse(tracks, ids, &st, fopt);
    std::vector<Job> jobs;
    for (auto [a, b] : chunks) {
      i64 short_s = 0;
      for (i64 t = a; t < b; ++t) short_s += st.n_used[std::size_t(t - t0)] < base;
      if (short_s * 10 > b - a) jobs.push_back({k, a, b});
    }
    if (jobs.empty()) break;
    reserve_chunks += int(jobs.size());
    co_await run_probed(std::move(jobs));
  }

  MonthResult out;
  out.values = fuse(tracks, ids, &st, fopt);
  // 0 mHz is exactly representable; the store keeps 1 mHz steps (values are Hz there).
  for (auto& v : out.values)
    if (std::isfinite(v)) v *= 1e-3f;
  std::size_t fin = 0;
  for (float v : out.values) fin += std::isfinite(v);
  // A failed download is never taken for "no data": with nothing usable the month fails
  // (and is planned again); otherwise it is stored as partial, listing the chunks to
  // fetch again next time (and only those).
  if (failed && !fin)
    throw std::runtime_error(name + " " + month + ": " + std::to_string(failed) + " of " + std::to_string(n_jobs) + " downloads failed (" + last_err + ")");
  if (failed)
    std::cerr << "  " << name << " " << month << ": " << failed << " of " << n_jobs << " downloads failed in " << failed_chunks.size()
              << " chunk(s), kept for the next fetch (" << last_err << ")" << std::endl;
  // A month fetched again for a few chunks (`skipped` > 0: the rest is stored) keeps what
  // it has when those chunks add nothing; only a month with nothing at all has no data.
  if (!fin && !chunks.empty() && !skipped)
    throw NoData("no station's hum passed the checks in " + month + " (" + std::to_string(empty) + " empty chunk(s))");
  json::array used;
  for (std::size_t k = 0; k < pick.size(); ++k) {
    if (bytes[ids[k]] == 0) continue;  // a reserve never needed
    const Station& p = *pick[k];
    json::object o{{"channel", ids[k]}, {"seconds_used", st.used_s[k]}, {"bytes", bytes[ids[k]]}};
    if (std::isfinite(p.audit_lat)) {
      o["lat"] = p.audit_lat, o["lon"] = p.audit_lon;  // inside the outline, for the audit
      o["site_lat"] = p.lat, o["site_lon"] = p.lon;
    } else {
      o["lat"] = p.lat, o["lon"] = p.lon;
    }
    used.push_back(std::move(o));
  }
  out.meta = {{"source", name},
              {"url", dataselect},
              {"licence", net.licence  ? net.licence
                          : nyquist ? "Raspberry Shake citizen-science network AM (doi:10.7914/SN/AM), open data"
                                    : "EarthScope / FDSN open data (cite each network's DOI; see docs/grids/datasets.md)"},
              {"native_resolution_s", 1.0},
              {"method", nyquist ? "50 Hz mains hum on the Nyquist frequency of 100 samples/s geophones: the real beat is decoded to a signed "
                                   "deviation by a forward-backward pass (the digitiser's roll-off gives the sign), stations fused by agreement"
                                 : std::to_string(int(net.mains_hz)) + " Hz mains hum in " + (net.timestamps ? "" : "GPS-timed ") +
                                       "accelerometer/seismometer recordings: 12 s Hann frames each second, stations fused by agreement and SNR"},
              {"timestamps", net.timestamps ? net.timestamps
                             : nyquist ? "UTC from the stations' NTP-disciplined clocks (tens of milliseconds)"
                                     : "UTC from the digitisers' GPS clocks; each value is the 12 s frame centred on the middle of its second"},
              {"files", used},  // per month in the manifest: the placement audit reads it
              {"chunks_with_reserves", reserve_chunks},
              {"seconds_multi_station", st.multi_s},
              {"seconds_single_station", st.single_s}};
  out.meta["fetched_to"] = end;
  if (!failed_chunks.empty()) {
    json::array rc;
    for (i64 a : failed_chunks) rc.push_back(a);
    out.meta["retry_chunks"] = std::move(rc);
  }
  if (end < (net.until ? std::min(t1, net.until) : t1) || !failed_chunks.empty()) out.meta["partial"] = true;
  co_return out;
}

}  // namespace geo::seismic
