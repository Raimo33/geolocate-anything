// S3-compatible buckets (e.g. Railway buckets): addressing, AWS SigV4 signing (by
// libcurl) and paged ListObjectsV2. Shared by the reference-store mirror (refs/remote)
// and the ADS-B store (air/bucket), each with its own bucket and credentials.
#pragma once

#include <map>
#include <optional>
#include <string>

#include "net/http.hpp"

namespace geo::net::s3 {

struct Bucket {
  std::string name, scheme, host, region, key, secret;
  bool path_style = false;
};

// From the environment, for a prefix such as "GEOLOC_GRID_": <prefix>BUCKET,
// <prefix>ACCESS_KEY_ID and <prefix>SECRET_ACCESS_KEY; the endpoint and region are shared,
// GEOLOC_S3_ENDPOINT_URL and GEOLOC_S3_REGION (default "auto"). nullopt when the bucket
// name is not set.
std::optional<Bucket> from_env(const std::string& prefix);

std::string object_url(const Bucket& b, const std::string& key);
Request signed_request(const Bucket& b, std::string url);  // GET; set method/upload_file for others

// Every object under `prefix`: key relative to it -> ETag (quotes stripped); `modified`
// (if given) gets each key's LastModified in UTC seconds.
asio::awaitable<std::map<std::string, std::string>> list(Http& http, const Bucket& b, const std::string& prefix,
                                                         std::map<std::string, double>* modified = nullptr);

// One ListObjectsV2 page: key -> etag; next_token is empty on the last page.
std::map<std::string, std::string> parse_list_objects(const std::string& xml, std::string& next_token,
                                                      std::map<std::string, double>* modified = nullptr);

}  // namespace geo::net::s3
