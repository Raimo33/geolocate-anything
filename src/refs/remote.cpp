#include "refs/remote.hpp"

#include <openssl/evp.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <deque>
#include <iostream>
#include <stdexcept>

#include "net/http.hpp"
#include "net/s3.hpp"
#include "store/store.hpp"
#include "util/files.hpp"
#include "util/text.hpp"

namespace geo::refs::remote {

namespace asio = boost::asio;

bool enabled() { return !env("GEOLOC_GRID_BUCKET").empty(); }

namespace {

// The reference store's bucket (GEOLOC_GRID_*); objects under refs/.
struct Config {
  net::s3::Bucket b;
  std::string prefix = "refs/";
};

Config config() {
  auto b = net::s3::from_env("GEOLOC_GRID_");
  if (!b) throw std::runtime_error("GEOLOC_GRID_BUCKET is not set");
  return {*b};
}

std::string object_url(const Config& c, const std::string& key) { return net::s3::object_url(c.b, key); }
net::Request signed_req(const Config& c, std::string url) { return net::s3::signed_request(c.b, std::move(url)); }
asio::awaitable<std::map<std::string, std::string>> remote_index(net::Http& http, const Config& c, const std::string& prefix,
                                                                 std::map<std::string, double>* modified = nullptr) {
  co_return co_await net::s3::list(http, c.b, prefix, modified);
}

double file_mtime(const fs::path& p) {
  struct stat st{};
  return ::stat(p.c_str(), &st) == 0 ? double(st.st_mtim.tv_sec) + 1e-9 * double(st.st_mtim.tv_nsec) : 0.0;
}

std::map<std::string, fs::path> local_files(const std::string& sub) {
  std::map<std::string, fs::path> out;
  fs::path root = store::refs_dir(), base = sub.empty() ? root : root / sub;
  std::error_code ec;
  if (!fs::exists(base)) return out;
  for (auto& e : fs::recursive_directory_iterator(base, ec)) {
    if (!e.is_regular_file()) continue;
    auto name = e.path().filename().string();
    if (name.ends_with(".tmp") || name.ends_with(".part") || name.ends_with(".lock")) continue;
    out[fs::relative(e.path(), root).generic_string()] = e.path();
  }
  return out;
}

}  // namespace

static std::string md5_hex(std::string_view data) {
  unsigned char d[EVP_MAX_MD_SIZE];
  unsigned int n = 0;
  EVP_Digest(data.data(), data.size(), d, &n, EVP_md5(), nullptr);
  static const char* hex = "0123456789abcdef";
  std::string s;
  for (unsigned i = 0; i < n; ++i) {
    s += hex[d[i] >> 4];
    s += hex[d[i] & 15];
  }
  return s;
}

std::string md5_file(const std::string& path) { return md5_hex(MappedFile(path).view()); }

std::map<std::string, std::string> parse_list_objects(const std::string& xml, std::string& next_token, std::map<std::string, double>* modified) {
  return net::s3::parse_list_objects(xml, next_token, modified);
}

json::object merge_manifests(const json::object& local, const json::object& remote) {
  json::object out = local;
  auto section = [](json::object& m, const char* k) -> json::object& {
    if (!m[k].is_object()) m[k] = json::object();
    return m[k].get_object();
  };
  auto stamp = [](const json::value& e, const char* k) {
    const json::object* o = e.if_object();
    return o ? jstr(*o, k) : std::string();
  };
  for (auto [name, when] : {std::pair{"months", "stored_at"}, std::pair{"unavailable", "checked_at"}}) {
    const json::object* theirs = remote.if_contains(name) ? remote.at(name).if_object() : nullptr;
    if (!theirs) continue;
    json::object& ours = section(out, name);
    for (auto& kv : *theirs) {
      auto it = ours.find(kv.key());
      if (it == ours.end() || stamp(kv.value(), when) > stamp(it->value(), when)) ours[kv.key()] = kv.value();
    }
  }
  if (!out.contains("meta"))
    if (auto* m = remote.if_contains("meta")) out["meta"] = *m;
  if (auto* un = out.if_contains("unavailable"); un && un->is_object())
    if (auto* ms = out.if_contains("months"); ms && ms->is_object())
      for (auto& kv : ms->get_object()) un->get_object().erase(kv.key());
  return out;
}

namespace {
// "grid/source/manifest.json" -> (grid, source)
std::pair<std::string, std::string> manifest_owner(const std::string& rel) {
  auto parts = split(rel, '/');
  if (parts.size() != 3) throw std::runtime_error("unexpected manifest path " + rel);
  return {std::string(parts[0]), std::string(parts[1])};
}
// Merges a manifest from the bucket into the local one (written when it changes);
// returns the merged manifest.
json::object merge_into_local(const std::string& rel, const std::string& body) {
  auto [grid, source] = manifest_owner(rel);
  auto v = parse_json(body);
  if (!v.is_object()) throw std::runtime_error("bucket manifest " + rel + " is not a JSON object");
  json::object merged;
  store::update_manifest(grid, source, [&](json::object& local) {
    const bool have_local = fs::exists(store::refs_dir() / rel);
    merged = have_local ? merge_manifests(local, v.get_object()) : v.get_object();
    local = merged;
  });
  return merged;
}
}  // namespace

namespace {
std::string tmp_suffix() {  // per process and call: another process or thread may fetch the same key
  static std::atomic<unsigned> n{0};
  return "." + std::to_string(::getpid()) + "-" + std::to_string(n++) + ".tmp";
}

// GET a month file into dst through a temporary file; false on 404. Retried on 5xx/429.
asio::awaitable<bool> get_month(net::Http& http, const Config& c, const std::string& rel, const fs::path& dst) {
  fs::create_directories(dst.parent_path());
  fs::path tmp = dst;
  tmp += tmp_suffix();
  for (int attempt = 0;; ++attempt) {
    std::FILE* f = std::fopen(tmp.c_str(), "wb");
    if (!f) throw std::runtime_error("cannot write " + tmp.string());
    auto rq = signed_req(c, object_url(c, c.prefix + rel));
    rq.on_data = [f](std::string_view d) { return std::fwrite(d.data(), 1, d.size(), f) == d.size(); };
    auto r = co_await http.fetch(std::move(rq));
    const bool flushed = std::fclose(f) == 0;  // the buffered tail can still fail (disk full)
    if (r.ok() && flushed) break;
    fs::remove(tmp);
    if (r.status == 404) co_return false;
    if (!flushed) throw std::runtime_error("cannot write " + tmp.string());
    if (attempt == 3 || (r.error.empty() && r.status < 500 && r.status != 429))
      throw std::runtime_error("download of " + rel + " failed: " + (r.error.empty() ? "HTTP " + std::to_string(r.status) : r.error));
    co_await net::sleep_for(double(1 << attempt));
  }
  store::mark_clean(tmp);
  fs::rename(tmp, dst);  // readers never see a half-written month
  co_return true;
}
}  // namespace

int fetch(const std::vector<std::string>& rels) {
  constexpr int parallel = 16;
  const Config c = config();
  std::atomic<int> n{0};
  net::run_sync_void([&](asio::io_context& io) -> asio::awaitable<void> {
    net::Http http(io);
    const fs::path root = store::refs_dir();
    co_await net::parallel_for(rels, parallel, [&](const std::string& rel) -> asio::awaitable<void> {
      if (co_await get_month(http, c, rel, root / rel)) ++n;
    });
  });
  return n;
}

void install() {
  if (!enabled()) return;
  store::set_month_fetcher([](const std::vector<std::string>& rels) {
    try {
      fetch(rels);
    } catch (const std::exception& e) {  // the search goes on without those months
      std::cerr << "bucket: " << e.what() << std::endl;
    }
  });
}

int pull(const std::string& grid, std::function<void(int, int)> progress, bool lazy) {
  constexpr int parallel = 16;
  const Config c = config();
  int total = 0;
  net::run_sync_void([&](asio::io_context& io) -> asio::awaitable<void> {
    net::Http http(io);
    const std::string sub = grid.empty() ? "" : grid + "/";
    std::map<std::string, double> modified;
    auto idx = co_await remote_index(http, c, c.prefix + sub, &modified);
    fs::path root = store::refs_dir();
    std::vector<std::string> months, manifests;
    // Lazy: each source's newest month still comes (the dashboard's grid table reads it).
    std::map<std::string, std::string> newest;  // "grid/source" -> its last month's key
    if (lazy)
      for (auto& [rel0, etag] : idx)
        if (!rel0.ends_with("manifest.json")) {
          auto& n = newest[(fs::path(sub + rel0).parent_path()).generic_string()];
          n = std::max(n, sub + rel0);
        }
    auto wanted_now = [&](const std::string& rel) { return !lazy || newest[fs::path(rel).parent_path().generic_string()] == rel; };
    for (auto& [rel0, etag] : idx) {
      std::string rel = sub + rel0;
      fs::path dst = root / rel;
      const bool manifest = rel.ends_with("manifest.json");
      if (fs::exists(dst)) {
        if (md5_file(dst) == etag) {
          if (!manifest && !store::is_clean(dst)) store::mark_clean(dst);
          continue;
        }
        // Written here after the bucket's copy (a refresh cut short before its upload):
        // keep it; the next refresh uploads it. Manifests are merged either way.
        if (auto m = modified.find(rel0); !manifest && m != modified.end() && file_mtime(dst) > m->second) continue;
        if (!manifest && !wanted_now(rel)) {  // an older copy: dropped, read again from the bucket on first use
          fs::remove(dst);
          continue;
        }
      } else if (!manifest && !wanted_now(rel)) {
        continue;
      }
      (manifest ? manifests : months).push_back(rel);
    }
    total = int(months.size() + manifests.size());
    int done = 0;
    if (progress) progress(0, total);
    auto get = [&](const std::string& rel) -> asio::awaitable<void> {
      if (!co_await get_month(http, c, rel, root / rel)) throw std::runtime_error("download of " + rel + " failed: HTTP 404");
      ++done;
      if (progress) progress(done, total);
    };
    auto get_manifest = [&](const std::string& rel) -> asio::awaitable<void> {
      auto r = co_await http.fetch(signed_req(c, object_url(c, c.prefix + rel)));
      if (!r.ok()) throw std::runtime_error("download of " + rel + " failed: " + (r.error.empty() ? "HTTP " + std::to_string(r.status) : r.error));
      merge_into_local(rel, r.body);
      ++done;
      if (progress) progress(done, total);
    };
    // Months first, manifests last: a reader never sees a manifest listing a month
    // whose file has not arrived yet.
    co_await net::parallel_for(months, parallel, get);
    co_await net::parallel_for(manifests, parallel, get_manifest);
  });
  store::invalidate_caches();
  return total;
}

int push(const std::string& grid) {
  constexpr int parallel = 8;
  const Config c = config();
  int n = 0;
  net::run_sync_void([&](asio::io_context& io) -> asio::awaitable<void> {
    net::Http http(io);
    const std::string sub = grid.empty() ? "" : grid + "/";
    std::map<std::string, double> modified;
    auto remote = co_await remote_index(http, c, c.prefix + sub, &modified);
    std::vector<std::pair<std::string, fs::path>> months, manifests;
    int skipped = 0;
    for (auto& [rel, path] : local_files(grid)) {
      std::string key_rel = grid.empty() ? rel : rel.substr(grid.size() + 1);
      auto it = remote.find(key_rel);
      const bool manifest = rel.ends_with("manifest.json");
      if (it != remote.end() && it->second == md5_file(path)) {
        if (!manifest && !store::is_clean(path)) store::mark_clean(path);
        continue;
      }
      if (!manifest && it != remote.end())
        if (auto m = modified.find(key_rel); m != modified.end() && m->second > file_mtime(path)) {
          ++skipped;  // the bucket's copy was written after ours: `refs pull` brings it here
          continue;
        }
      (manifest ? manifests : months).emplace_back(rel, path);
    }
    if (skipped) std::cerr << skipped << " file(s) not uploaded: the bucket holds newer copies" << std::endl;
    auto put = [&](const std::string& rel, const std::string& file) -> asio::awaitable<void> {
      auto rq = signed_req(c, object_url(c, c.prefix + rel));
      rq.upload_file = file;
      auto r = co_await http.fetch(std::move(rq));
      if (!r.ok()) throw std::runtime_error("upload of " + rel + " failed: " + (r.error.empty() ? "HTTP " + std::to_string(r.status) + " " + r.body.substr(0, 300) : r.error));
      ++n;
    };
    co_await net::parallel_for(months, parallel, [&](const std::pair<std::string, fs::path>& item) -> asio::awaitable<void> {
      const std::string sum = md5_file(item.second.string());
      co_await put(item.first, item.second.string());
      // In the bucket now: evictable, unless it was rewritten while uploading.
      if (md5_file(item.second.string()) == sum) store::mark_clean(item.second);
    });
    // Manifests after their months, each merged with the bucket's first: what another
    // machine stored since our last pull stays listed.
    co_await net::parallel_for(manifests, parallel, [&](const std::pair<std::string, fs::path>& item) -> asio::awaitable<void> {
      const std::string key_rel = grid.empty() ? item.first : item.first.substr(grid.size() + 1);
      if (remote.contains(key_rel)) {
        auto r = co_await http.fetch(signed_req(c, object_url(c, c.prefix + item.first)));
        if (!r.ok()) throw std::runtime_error("download of " + item.first + " failed: " + (r.error.empty() ? "HTTP " + std::to_string(r.status) : r.error));
        merge_into_local(item.first, r.body);
        if (md5_file(item.second.string()) == md5_hex(r.body)) co_return;  // the bucket's already says it all
      }
      co_await put(item.first, item.second.string());
    });
  });
  store::invalidate_caches();
  return n;
}

}  // namespace geo::refs::remote
