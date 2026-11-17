#include "net/s3.hpp"

#include <cstring>
#include <stdexcept>

#include "util/files.hpp"
#include "util/text.hpp"
#include "util/time.hpp"

namespace geo::net::s3 {

std::optional<Bucket> from_env(const std::string& prefix) {
  Bucket b;
  b.name = env((prefix + "BUCKET").c_str());
  if (b.name.empty()) return std::nullopt;
  const std::string endpoint = env("GEOLOC_S3_ENDPOINT_URL", "https://s3.amazonaws.com");
  auto p = endpoint.find("://");
  b.scheme = p == std::string::npos ? "https" : endpoint.substr(0, p);
  b.host = p == std::string::npos ? endpoint : endpoint.substr(p + 3);
  while (!b.host.empty() && b.host.back() == '/') b.host.pop_back();
  b.region = env("GEOLOC_S3_REGION", "auto");
  b.key = env((prefix + "ACCESS_KEY_ID").c_str());
  b.secret = env((prefix + "SECRET_ACCESS_KEY").c_str());
  // Virtual-hosted addressing (bucket.host), except where the host cannot carry a
  // subdomain: an IP address or localhost (a local S3 server).
  const std::string name = b.host.substr(0, b.host.rfind(':'));
  b.path_style = name == "localhost" || (!name.empty() && name.find_first_not_of("0123456789.") == std::string::npos);
  return b;
}

static std::string base_url(const Bucket& b) {
  return b.path_style ? b.scheme + "://" + b.host + "/" + b.name : b.scheme + "://" + b.name + "." + b.host;
}

std::string object_url(const Bucket& b, const std::string& key) { return base_url(b) + "/" + url_encode(key, true); }

Request signed_request(const Bucket& b, std::string url) {
  Request r;
  r.url = std::move(url);
  r.aws_sigv4 = "aws:amz:" + b.region + ":s3";
  r.userpwd = b.key + ":" + b.secret;
  r.headers.push_back("x-amz-content-sha256: UNSIGNED-PAYLOAD");
  r.decompress = false;
  return r;
}

namespace {

std::string xml_unescape(std::string s) {
  static const std::pair<const char*, const char*> E[] = {{"&quot;", "\""}, {"&apos;", "'"}, {"&lt;", "<"}, {"&gt;", ">"}, {"&amp;", "&"}};
  for (auto [a, b] : E) {
    std::size_t p = 0;
    while ((p = s.find(a, p)) != std::string::npos) {
      s.replace(p, std::strlen(a), b);
      p += std::strlen(b);
    }
  }
  return s;
}

std::string tag(std::string_view x, std::string_view name, std::size_t& pos) {
  std::string open = "<" + std::string(name) + ">", close = "</" + std::string(name) + ">";
  auto a = x.find(open, pos);
  if (a == std::string_view::npos) return {};
  a += open.size();
  auto b = x.find(close, a);
  if (b == std::string_view::npos) return {};
  pos = b + close.size();
  return xml_unescape(std::string(x.substr(a, b - a)));
}

}  // namespace

std::map<std::string, std::string> parse_list_objects(const std::string& xml, std::string& next_token, std::map<std::string, double>* modified) {
  std::map<std::string, std::string> out;
  std::size_t pos = 0;
  for (;;) {
    auto a = xml.find("<Contents>", pos);
    if (a == std::string::npos) break;
    auto b = xml.find("</Contents>", a);
    if (b == std::string::npos) break;  // a cut-off listing: no partial last entry
    std::string_view item(xml.data() + a, b - a);
    std::size_t p = 0;
    std::string key = tag(item, "Key", p);
    p = 0;
    std::string etag = tag(item, "ETag", p);
    // Quotes come literal or XML-escaped (&quot; / &#34;) depending on the provider.
    for (const char* q : {"&quot;", "&#34;", "\""})
      for (std::size_t at; (at = etag.find(q)) != std::string::npos;) etag.erase(at, std::strlen(q));
    out[key] = etag;
    if (modified) {
      p = 0;
      if (auto w = parse_iso_datetime(tag(item, "LastModified", p))) (*modified)[key] = w->utc();
    }
    pos = b;
  }
  std::size_t p = 0;
  next_token = tag(xml, "IsTruncated", p) == "true" ? (p = 0, tag(xml, "NextContinuationToken", p)) : std::string();
  return out;
}

asio::awaitable<std::map<std::string, std::string>> list(Http& http, const Bucket& b, const std::string& prefix,
                                                         std::map<std::string, double>* modified) {
  std::map<std::string, std::string> out;
  std::string token;
  do {
    std::string q = "list-type=2&prefix=" + url_encode(prefix);
    if (!token.empty()) q += "&continuation-token=" + url_encode(token);
    std::string url = base_url(b) + (b.path_style ? "?" : "/?") + q;
    auto r = co_await http.fetch(signed_request(b, url));
    if (!r.ok()) throw std::runtime_error("bucket listing failed: " + (r.error.empty() ? "HTTP " + std::to_string(r.status) + " " + r.body.substr(0, 300) : r.error));
    std::string next;
    std::map<std::string, double> mod;
    for (auto& [k, e] : parse_list_objects(r.body, next, modified ? &mod : nullptr)) out[k.substr(std::min(k.size(), prefix.size()))] = e;
    if (modified)
      for (auto& [k, t] : mod) (*modified)[k.substr(std::min(k.size(), prefix.size()))] = t;
    token = next;
  } while (!token.empty());
  co_return out;
}

}  // namespace geo::net::s3
