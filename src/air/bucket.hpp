// The ADS-B store's own bucket (e.g. a Railway bucket of its own, apart from the
// reference store's). Days are built once (`aircraft fetch DAY --push`, ~4 GB read from
// ADSB.lol, ~50 MB kept) and kept there; a server's volume holds only the days its
// analyses used last (GEOLOC_AIRPLANE_KEEP_DAYS), each streamed from the bucket on first use,
// plus every day's small coverage sidecar for the history map.
//
//   GEOLOC_AIRPLANE_BUCKET, GEOLOC_AIRPLANE_ACCESS_KEY_ID, GEOLOC_AIRPLANE_SECRET_ACCESS_KEY;
//   endpoint and region shared with the grid bucket: GEOLOC_S3_ENDPOINT_URL, GEOLOC_S3_REGION.
//
// Objects: adsb/<YYYY-MM-DD>.trk (the day's store) and adsb/<YYYY-MM-DD>.cov (coverage);
// opensky/<key>.trk, OpenSky's answers to the aircraft check (opensky.hpp).
#pragma once

#include <functional>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "util/files.hpp"

namespace geo::air::bucket {

bool enabled();
// Days held in the bucket (a listing, remembered for a minute).
std::set<std::string> days();
// Downloads a day's store and coverage into the local store; false when the bucket has
// no such day. Old local days are then evicted (GEOLOC_AIRPLANE_KEEP_DAYS).
bool pull_day(const std::string& day);
// Downloads every coverage sidecar missing here; returns how many.
int pull_coverage(std::function<void(int, int)> progress = {});
// Uploads local days (all when `days` is empty): every one missing in the bucket or
// different there; returns the days uploaded.
int push(const std::vector<std::string>& days = {});

// Any object: downloaded to dst (false when the bucket has no such key), or uploaded.
bool get_object(const std::string& key, const fs::path& dst);
void put_object(const std::string& key, const fs::path& src);
void delete_object(const std::string& key);
// Keys under `prefix` (relative to it) -> ETag.
std::map<std::string, std::string> list_keys(const std::string& prefix);
// Many at once (16 in flight): (key, local path) pairs. get_objects returns the keys found.
std::set<std::string> get_objects(const std::vector<std::pair<std::string, fs::path>>& items);
void put_objects(const std::vector<std::pair<std::string, fs::path>>& items);

}  // namespace geo::air::bucket
