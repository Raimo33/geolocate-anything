// The seismic sources: a grid's frequency from the mains hum in EarthScope's archived
// accelerometer/seismometer recordings (see refs/seismic.hpp). seismic_east is the US
// Eastern Interconnection (60 Hz), seismic_west the Western one and seismic_ercot Texas
// (both 60 Hz), seismic_ips is IPS/UPS (50 Hz); seismic_cn_* are four
// Chinese grids from Raspberry Shake geophones (50 Hz on the Nyquist of 100 samples/s),
// seismic_swis and seismic_darwin Western Australia's SWIS and Darwin-Katherine the same way,
// seismic_perth_basin the SWIS in 2017 from a 250 samples/s array at AusPass;
// seismic_brazil is Brazil's SIN (60 Hz) in 2004-02..2008-08, from the University of São Paulo's archive;
// seismic_estonia is IPS/UPS in 2015-05..2025-02 from the Estonian network at GEOFON (250 samples/s);
// seismic_ips_shake is IPS/UPS from Raspberry Shakes and seismic_ips's stations together.
#pragma once

#include <cmath>
#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

#include <boost/asio.hpp>
#include <boost/json.hpp>

#include "net/http.hpp"

namespace geo::seismic {

struct Station {
  const char *id;         // NET.STA.LOC.CHA
  double lat, lon;
  const char *from, *to;  // channel epoch, YYYY-MM-DD (to exclusive, "" = open)
  // Agreement with the other stations in the screening windows (one per year): the
  // Pearson r after 12 s smoothing, by year; a month uses the stations whose nearest
  // screened year scored best.
  std::vector<std::pair<int, float>> score;
  // Sites on a coast or river bank that the grid outline (coarse coastline) leaves just
  // outside: the nearest point inside it (within 5 km) is what the placement audit checks.
  double audit_lat = NAN, audit_lon = NAN;
};

// One reconstructed grid: its screened stations and how a month picks among them.
struct Network {
  const char* source;       // name in the store
  double mains_hz;
  const char* first_month;  // earliest month worth fetching ("" = the first station's epoch)
  float min_score;          // a station is used in years where it scored at least this
  int base_stations;        // fetched for the whole month
  int max_stations;         // with the reserves, fetched only where the base ones fall short
  bool lone;                // may one quiet station stand without another to confirm it
  const std::vector<Station>& (*stations)();
  // Raspberry Shake networks: 100 samples/s geophones, the 50 Hz line on Nyquist
  // (seismic::track_nyquist), served by the Raspberry Shake FDSN archive.
  bool nyquist = false;
  float agree_r = 0.9f;          // two stations agree from this correlation on
  float lone_noise_mhz = 1.5f;   // a lone station must be at least this quiet
  const char* archive = "EarthScope";
  // The rest differ from the defaults only for seismic_brazil.
  const char* dataselect = nullptr;  // FDSN dataselect URL (nullptr: EarthScope's, or Raspberry Shake's when nyquist)
  const char* licence = nullptr;
  const char* timestamps = nullptr;  // how the samples are timed, for the manifest
  bool robust_floor = false;         // TrackOptions::robust_floor
  // Months [skip_from, skip_to] are not listed: a measured source covers them
  // (seismic_west leaves AESO's 2019-01..2025-07 out).
  const char* skip_from = nullptr;
  const char* skip_to = nullptr;
  // A lone station may stand only in months before this one (seismic_ercot: one site
  // in 2014, enough to confirm each other from 2015).
  const char* lone_before = nullptr;
  // Stations closer than this count as one site whatever their codes (0: by code only).
  float same_site_km = 0;
  // The grid leaves the stations' area at this UTC second (0: never): no month after
  // the one holding it is listed, and nothing from it on is stored (seismic_estonia: the
  // Baltic states left IPS/UPS on 2025-02-08).
  std::int64_t until = 0;
  // Both kinds of station in one network (seismic_ips_shake): Raspberry Shakes (network
  // AM) are fetched from their archive and decoded on Nyquist, the others from EarthScope
  // and tracked directly, and all are fused together (FuseOptions::nyquist).
  bool mixed = false;
  float agree_r_nyquist = 0.7f;       // a pair with a Shake agrees from this r
  float lone_noise_nyquist_mhz = 0;   // a lone Shake must be this quiet (0: never alone)
  float lone_min_sd_mhz = 0;          // FuseOptions::lone_min_sd_mhz
};
const Network& east();
const Network& west();
const Network& ercot();
const Network& ips();
// IPS/UPS again, from Raspberry Shakes in Kazakhstan, Russia, Belarus, the Caucasus,
// Central Asia (and Ukraine, Moldova and the Baltic states while they were part of it)
// fused with seismic_ips's stations: offline only (GEOLOC_SHAKE_REFRESH).
const Network& ips_shake();
// China, from Raspberry Shakes: the Southern grid (Hong Kong, Guangdong), North & Central
// China, East China and the Southwest.
const Network& cn_south();
const Network& cn_north();
const Network& cn_east();
const Network& cn_southwest();
// Australia outside the NEM, from Raspberry Shakes: the South West Interconnected System
// (Perth) and the Darwin-Katherine system.
const Network& swis();
const Network& darwin();
// The SWIS again, 2017-01..06, from the direct line in a 250 samples/s array at AusPass.
const Network& perth_basin();
const Network& brazil();
const Network& estonia();
const Network* network(std::string_view source);

// Months worth fetching, from the first to the current one (or the last a station's epoch reaches).
std::vector<std::string> months(const Network& net);

struct MonthResult {
  std::vector<float> values;  // Hz deviation from 60 per second (NaN = none), as the store keeps it
  boost::json::object meta;
};
// Downloads and fuses one UTC month; throws Unavailable-like std::runtime_error when no
// station has data (the caller maps it), std::runtime_error when downloads failed.
struct NoData : std::runtime_error {
  using std::runtime_error::runtime_error;
};
// The network's best `base_stations` for the whole month; up to `max_stations` in all,
// the extra ones fetched only for the 6 h chunks where the base ones did not all hold up.
// `skip(a, b)`: seconds [a, b) are stored already (a live month fetched again); those
// chunks are not downloaded and come back NaN.
boost::asio::awaitable<MonthResult> fetch_month(net::Http& http, const Network& net, const std::string& month, std::function<bool(std::int64_t, std::int64_t)> skip = {});

}  // namespace geo::seismic
