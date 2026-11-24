#include "air/bucket.hpp"

#include <unistd.h>

#include <chrono>
#include <cstdio>
#include <iostream>
#include <mutex>
#include <stdexcept>

#include "air/adsb.hpp"
#include "net/s3.hpp"
#include "refs/remote.hpp"
#include "util/files.hpp"

namespace geo::air::bucket {

namespace asio = boost::asio;

namespace {

const std::string PREFIX = "adsb/";

net::s3::Bucket config() {
  auto b = net::s3::from_env("GEOLOC_AIRPLANE_");
  if (!b) throw std::runtime_error("GEOLOC_AIRPLANE_BUCKET is not set");
  return *b;
}

std::string fail(const net::Response& r) { return r.error.empty() ? "HTTP " + std::to_string(r.status) + " " + r.body.substr(0, 300) : r.error; }

// GET key into dst (through a temporary file); false on 404.
asio::awaitable<bool> get(net::Http& http, const net::s3::Bucket& b, const std::string& key, const fs::path& dst) {
  fs::create_directories(dst.parent_path());
  fs::path tmp = dst;
  tmp += "." + std::to_string(::getpid()) + ".part";  // per process: another may fetch the same key
  for (int attempt = 0;; ++attempt) {
    std::FILE* f = std::fopen(tmp.c_str(), "wb");
    if (!f) throw std::runtime_error("cannot write " + tmp.string());
    auto rq = net::s3::signed_request(b, net::s3::object_url(b, key));
    rq.on_data = [f](std::string_view d) { return std::fwrite(d.data(), 1, d.size(), f) == d.size(); };
    auto r = co_await http.fetch(std::move(rq));
    const bool flushed = std::fclose(f) == 0;
    if (r.ok() && flushed) break;
    fs::remove(tmp);
    if (r.status == 404) co_return false;
    if (!flushed) throw std::runtime_error("cannot write " + tmp.string());
    if (attempt == 3 || (r.error.empty() && r.status < 500 && r.status != 429)) throw std::runtime_error("download of " + key + " failed: " + fail(r));
    co_await net::sleep_for(double(2 << attempt));
  }
  fs::rename(tmp, dst);
  co_return true;
}

asio::awaitable<void> put(net::Http& http, const net::s3::Bucket& b, const std::string& key, const fs::path& src) {
  for (int attempt = 0;; ++attempt) {
    auto rq = net::s3::signed_request(b, net::s3::object_url(b, key));
    rq.upload_file = src.string();
    rq.stall_timeout_s = 120;
    auto r = co_await http.fetch(std::move(rq));
    if (r.ok()) co_return;
    if (attempt == 3 || (r.error.empty() && r.status < 500 && r.status != 429)) throw std::runtime_error("upload of " + key + " failed: " + fail(r));
    co_await net::sleep_for(double(2 << attempt));
  }
}

std::mutex days_mu;
std::set<std::string> days_cache;
std::chrono::steady_clock::time_point days_at;

}  // namespace

bool enabled() { return !env("GEOLOC_AIRPLANE_BUCKET").empty(); }

std::set<std::string> days() {
  std::lock_guard lk(days_mu);
  if (days_at != std::chrono::steady_clock::time_point() && std::chrono::steady_clock::now() - days_at < std::chrono::minutes(1)) return days_cache;
  const auto b = config();
  auto idx = net::run_sync<std::map<std::string, std::string>>([&](asio::io_context& io) -> asio::awaitable<std::map<std::string, std::string>> {
    net::Http http(io);
    co_return co_await net::s3::list(http, b, PREFIX);
  });
  days_cache.clear();
  for (auto& [k, e] : idx)
    if (k.size() == 14 && k.ends_with(".trk")) days_cache.insert(k.substr(0, 10));
  days_at = std::chrono::steady_clock::now();
  return days_cache;
}

bool pull_day(const std::string& day) {
  const auto b = config();
  bool found = false;
  net::run_sync_void([&](asio::io_context& io) -> asio::awaitable<void> {
    net::Http http(io);
    std::cerr << "downloading ADS-B day " << day << " from the bucket" << std::endl;
    found = co_await get(http, b, PREFIX + day + ".trk", day_path(day));
    if (found && !fs::exists(coverage_path(day))) co_await get(http, b, PREFIX + day + ".cov", coverage_path(day));
    if (found) co_await get(http, b, PREFIX + day + ".source", adsb_dir() / (day + ".source"));  // a day built from OpenSky
  });
  if (!found) return false;
  mark_used(day);
  evict_days();
  return true;
}

int pull_coverage(std::function<void(int, int)> progress) {
  const auto b = config();
  int n = 0;
  net::run_sync_void([&](asio::io_context& io) -> asio::awaitable<void> {
    net::Http http(io);
    auto idx = co_await net::s3::list(http, b, PREFIX);
    std::vector<std::string> want;
    for (auto& [k, e] : idx)
      if (k.size() == 14 && k.ends_with(".cov") && !fs::exists(coverage_path(k.substr(0, 10)))) want.push_back(k);
    int done = 0;
    if (progress) progress(0, int(want.size()));
    co_await net::parallel_for(want, 16, [&](const std::string& k) -> asio::awaitable<void> {
      if (co_await get(http, b, PREFIX + k, coverage_path(k.substr(0, 10)))) ++n;
      if (progress) progress(++done, int(want.size()));
    });
  });
  return n;
}

int push(const std::vector<std::string>& which) {
  const auto b = config();
  const std::vector<std::string> local = which.empty() ? stored_days() : which;
  int n = 0;
  net::run_sync_void([&](asio::io_context& io) -> asio::awaitable<void> {
    net::Http http(io);
    auto idx = co_await net::s3::list(http, b, PREFIX);
    for (auto& day : local) {
      if (!has_day(day)) throw std::runtime_error("no ADS-B day " + day + " stored here");
      ensure_coverage(day);
      bool sent = false;
      // The store first: a listed day always has its store.
      for (const fs::path& f : {day_path(day), coverage_path(day)}) {
        const std::string key = f.filename().string();
        if (auto it = idx.find(key); it != idx.end() && it->second == refs::remote::md5_file(f.string())) continue;
        co_await put(http, b, PREFIX + key, f);
        sent = true;
      }
      if (sent) {
        ++n;
        std::cerr << "uploaded ADS-B day " << day << " (" << fs::file_size(day_path(day)) / 1000000 << " MB)" << std::endl;
      }
    }
  });
  std::lock_guard lk(days_mu);
  days_at = {};
  return n;
}

bool get_object(const std::string& key, const fs::path& dst) {
  const auto b = config();
  bool found = false;
  net::run_sync_void([&](asio::io_context& io) -> asio::awaitable<void> {
    net::Http http(io);
    found = co_await get(http, b, key, dst);
  });
  return found;
}

void put_object(const std::string& key, const fs::path& src) {
  const auto b = config();
  net::run_sync_void([&](asio::io_context& io) -> asio::awaitable<void> {
    net::Http http(io);
    co_await put(http, b, key, src);
  });
}

void delete_object(const std::string& key) {
  const auto b = config();
  net::run_sync_void([&](asio::io_context& io) -> asio::awaitable<void> {
    net::Http http(io);
    auto rq = net::s3::signed_request(b, net::s3::object_url(b, key));
    rq.method = "DELETE";
    auto r = co_await http.fetch(std::move(rq));
    if (!r.ok() && r.status != 404) throw std::runtime_error("delete of " + key + " failed: " + fail(r));
  });
}

std::map<std::string, std::string> list_keys(const std::string& prefix) {
  const auto b = config();
  return net::run_sync<std::map<std::string, std::string>>([&](asio::io_context& io) -> asio::awaitable<std::map<std::string, std::string>> {
    net::Http http(io);
    co_return co_await net::s3::list(http, b, prefix);
  });
}

std::set<std::string> get_objects(const std::vector<std::pair<std::string, fs::path>>& items) {
  const auto b = config();
  std::set<std::string> found;
  net::run_sync_void([&](asio::io_context& io) -> asio::awaitable<void> {
    net::Http http(io);
    co_await net::parallel_for(items, 16, [&](const std::pair<std::string, fs::path>& it) -> asio::awaitable<void> {
      if (co_await get(http, b, it.first, it.second)) found.insert(it.first);
    });
  });
  return found;
}

void put_objects(const std::vector<std::pair<std::string, fs::path>>& items) {
  const auto b = config();
  net::run_sync_void([&](asio::io_context& io) -> asio::awaitable<void> {
    net::Http http(io);
    co_await net::parallel_for(items, 16, [&](const std::pair<std::string, fs::path>& it) -> asio::awaitable<void> { co_await put(http, b, it.first, it.second); });
  });
}

}  // namespace geo::air::bucket
