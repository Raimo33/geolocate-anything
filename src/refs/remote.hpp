// Mirror the reference store to an S3-compatible bucket (e.g. a Railway bucket). The
// bucket is the shared copy; the local store is the working copy the search reads.
// Files are compared by MD5 against the object's ETag (uploads are single-part), so
// nothing is sent twice. Requests are signed by libcurl (AWS SigV4) and run in
// parallel on the event loop. Configured from the environment when GEOLOC_GRID_BUCKET is set:
//
//   GEOLOC_GRID_BUCKET, GEOLOC_S3_ENDPOINT_URL, GEOLOC_GRID_ACCESS_KEY_ID, GEOLOC_GRID_SECRET_ACCESS_KEY,
//   GEOLOC_S3_REGION (default "auto"). Objects live under refs/.
#pragma once

#include <functional>
#include <map>
#include <string>
#include <utility>
#include <vector>

#include "util/json.hpp"

namespace geo::refs::remote {

bool enabled();
// Several machines write to the bucket (the server's refreshes, `refs push` and imports
// elsewhere), so neither direction simply overwrites:
//  - a month file is replaced only by a copy written after it (local mtime against the
//    object's LastModified); the newer copy wins either way;
//  - a manifest that differs is merged with the other side's (every month either lists,
//    the entry stored later winning), so one machine's months never drop another's.
// Download every object missing locally or older here; manifests last, merged with the
// local ones. progress(done, total).
int pull(const std::string& grid = {}, std::function<void(int, int)> progress = {});
// Upload every local file missing in the bucket or newer here; months first, manifests
// last (merged with the bucket's). Returns the files uploaded.
int push(const std::string& grid = {});

// Building blocks (tests).
// Two manifests of one source: months and "unavailable" marks of both, the entry stored
// (checked) later winning; a month with data is never also "unavailable".
json::object merge_manifests(const json::object& local, const json::object& remote);
std::string md5_file(const std::string& path);
// key -> etag; `modified` (if given) gets each key's LastModified in UTC seconds.
std::map<std::string, std::string> parse_list_objects(const std::string& xml, std::string& next_token,
                                                      std::map<std::string, double>* modified = nullptr);

}  // namespace geo::refs::remote
