// The ADS-B store's own bucket (e.g. a Railway bucket of its own, apart from the
// reference store's). Days are built once (`aircraft fetch DAY --push`, ~4 GB read from
// ADSB.lol, ~50 MB kept) and kept there; a server's volume holds only the days its
// analyses used last (GEOLOC_AIRPLANE_KEEP_DAYS), each streamed from the bucket on first use,
// plus every day's small coverage sidecar for the history map.
//
//   GEOLOC_AIRPLANE_BUCKET, GEOLOC_AIRPLANE_ACCESS_KEY_ID, GEOLOC_AIRPLANE_SECRET_ACCESS_KEY;
//   endpoint and region shared with the grid bucket: GEOLOC_S3_ENDPOINT_URL, GEOLOC_S3_REGION.
//
// Objects: adsb/<YYYY-MM-DD>.trk (the day's store) and adsb/<YYYY-MM-DD>.cov (coverage).
#pragma once

#include <functional>
#include <set>
#include <string>
#include <vector>

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

}  // namespace geo::air::bucket
