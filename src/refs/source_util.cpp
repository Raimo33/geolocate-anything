// Download helpers shared by the reference-data sources.
#include "refs/source_impl.hpp"

#include <sys/stat.h>

#include <algorithm>
#include <cmath>
#include <fstream>
#include <iostream>
#include <stdexcept>

#include "net/zip.hpp"
#include "store/store.hpp"
#include "util/text.hpp"

namespace geo::refs::detail {

void log(const std::string& msg) { std::cerr << msg << std::endl; }

std::string now_month() { return month_key(static_cast<i64>(now_seconds())); }

json::array to_json_array(const std::vector<json::object>& v) {
  json::array a;
  for (auto& o : v) a.push_back(o);
  return a;
}

fs::path tmp_dir() {
  fs::path p = store::cache_dir() / "tmp";
  std::error_code ec;
  fs::create_directories(p, ec);
  return p;
}

// Stream a URL into a file (atomic), with retries.
asio::awaitable<void> download_to(Ctx& c, const std::string& url, const fs::path& dst) {
  std::string last;
  for (int attempt = 0; attempt < 3; ++attempt) {
    if (attempt) co_await net::sleep_for(double(1 << attempt));
    fs::path tmp = dst;
    tmp += ".part";
    std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
    net::Request rq;
    rq.url = url;
    rq.on_data = [&](std::string_view d) {
      out.write(d.data(), std::streamsize(d.size()));
      return bool(out);
    };
    auto r = co_await c.http.fetch(std::move(rq));
    out.close();  // flushes the buffered tail, which can still fail (disk full)
    if (r.ok() && out) {
      fs::rename(tmp, dst);
      co_return;
    }
    std::error_code ec;
    fs::remove(tmp, ec);
    last = r.ok() ? "cannot write " + tmp.string() : r.error.empty() ? "HTTP " + std::to_string(r.status) : r.error;
    if (r.error.empty() && r.status >= 400 && r.status < 500 && r.status != 429) break;
  }
  throw std::runtime_error("download failed: " + url + ": " + last);
}

// A file several months share (yearly archives): downloaded once into the cache.
asio::awaitable<fs::path> cached_download(Ctx& c, const std::string& url, const std::string& name) {
  fs::path p = store::cache_dir() / "downloads" / name;
  for (;;) {
    if (auto it = c.in_flight.find(p.string()); it != c.in_flight.end()) {
      auto ev = it->second;
      co_await ev->wait();
      continue;
    }
    if (fs::exists(p)) co_return p;
    auto ev = std::make_shared<net::AsyncEvent>(c.io.get_executor());
    c.in_flight[p.string()] = ev;
    std::exception_ptr err;
    try {
      fs::create_directories(p.parent_path());
      co_await download_to(c, url, p);
    } catch (...) {
      err = std::current_exception();
    }
    c.in_flight.erase(p.string());
    ev->set();
    if (err) std::rethrow_exception(err);
    co_return p;
  }
}

// Download a file that may be a CSV, a ZIP holding one or a 7z: CSVs are parsed as they
// stream in; archives are spooled to a temporary file, then every CSV member of a ZIP (by
// name; a 7z's every member) is inflated through the same parser.
asio::awaitable<void> stream_csv_or_zip(Ctx& c, const std::string& url, const std::function<void(std::string_view)>& feed) {
  std::optional<TempFile> spool;
  std::ofstream out;
  enum class Kind { Undecided, Csv, Zip, SevenZ } kind = Kind::Undecided;  // decided by the first 6 bytes
  std::string head;
  auto handle = [&](std::string_view d) -> bool {
    if (kind == Kind::Undecided) {
      head.append(d);
      if (head.size() < 6) return true;
      kind = zip::is_zip(head) ? Kind::Zip : zip::is_7z(head) ? Kind::SevenZ : Kind::Csv;
      if (kind != Kind::Csv) {
        spool.emplace(kind == Kind::Zip ? ".zip" : ".7z", tmp_dir());
        out.open(spool->path, std::ios::binary);
      }
      d = head;
    }
    if (kind == Kind::Csv) feed(d);
    else out.write(d.data(), std::streamsize(d.size()));
    if (!head.empty()) head.clear(), head.shrink_to_fit();
    return true;
  };
  net::Request rq;
  rq.url = url;
  rq.on_data = handle;
  auto r = co_await c.http.fetch(std::move(rq));
  if (!r.ok()) throw std::runtime_error("download failed: " + url + ": " + (r.error.empty() ? "HTTP " + std::to_string(r.status) : r.error));
  if (kind == Kind::Undecided && !head.empty()) feed(head);  // under 6 bytes: not an archive
  if (kind == Kind::Csv || kind == Kind::Undecided) co_return;
  out.close();
  if (kind == Kind::SevenZ) {
    zip::for_each_libarchive(spool->path.string(), {[](const std::string&) { return true; }, feed, [&] { feed("\n"); }});
    co_return;
  }
  MappedFile mf(spool->path);
  zip::Archive z(mf.bytes());
  std::vector<const zip::Entry*> csvs;
  for (auto& e : z.entries())
    if (ends_with_ci(e.name, ".csv")) csvs.push_back(&e);
  if (csvs.empty()) throw std::runtime_error(url + ": no CSV in the zip");
  std::sort(csvs.begin(), csvs.end(), [](auto* a, auto* b) { return a->name < b->name; });
  if (csvs.size() > 1) log("  " + url + ": " + std::to_string(csvs.size()) + " CSV files in the zip, reading all");
  for (auto* e : csvs) {
    z.read(*e, feed);
    feed("\n");  // a member's last line must not run into the next one's header
  }
}

asio::awaitable<std::string> get_text(Ctx& c, const std::string& url, int retries) {
  auto r = co_await c.http.get(url, retries, 120);
  co_return std::move(r.body);
}

// Index cached on disk for a day.
asio::awaitable<std::map<std::string, std::string>> cached_index(Ctx& c, const char* file, const std::string& url,
                                                                 std::map<std::string, std::string> (*parse)(std::string_view)) {
  fs::path p = store::cache_dir() / file;
  struct stat st{};
  if (::stat(p.c_str(), &st) == 0 && now_seconds() - double(st.st_mtime) < 86400) {
    if (auto t = read_file(p)) {
      std::map<std::string, std::string> m;
      auto v = parse_json(*t);
      for (auto& kv : v.get_object()) m[std::string(kv.key())] = std::string(kv.value().get_string());
      co_return m;
    }
  }
  auto m = parse(co_await get_text(c, url));
  json::object o;
  for (auto& [k, v] : m) o[k] = v;
  atomic_write(p, dump(o));
  co_return m;
}

// ---------------------------------------------------------------- shared multi-month files

// Loads (3 attempts; Unavailable is final) or waits for the load already running.
asio::awaitable<std::shared_ptr<SharedFile>> shared_file(Ctx& c, const std::string& key, const Loader& load) {
  if (auto it = c.files.find(key); it != c.files.end()) {
    auto s = it->second;
    co_await s->ready.wait();
    if (s->error) std::rethrow_exception(s->error);
    co_return s;
  }
  auto s = std::make_shared<SharedFile>(c.io.get_executor());
  c.files[key] = s;
  std::string last;
  for (int attempt = 0; attempt < 3 && !s->error; ++attempt) {
    if (attempt) co_await net::sleep_for(double(1 << attempt));
    try {
      s->months.clear();
      co_await load(c, *s);
      s->ready.set();
      co_return s;
    } catch (const Unavailable&) {
      s->error = std::current_exception();
    } catch (const std::exception& ex) {
      last = ex.what();
    }
  }
  if (!s->error) s->error = std::make_exception_ptr(std::runtime_error(key + ": " + last));
  s->ready.set();
  c.files.erase(key);
  std::rethrow_exception(s->error);
}

// One month of a shared file; nullopt when the file does not cover it.
asio::awaitable<std::optional<std::vector<float>>> take_month(Ctx& c, const std::string& key, const std::string& month,
                                                              const Loader& load) {
  auto s = co_await shared_file(c, key, load);
  auto it = s->months.find(month);
  if (it == s->months.end() && s->handed_out.contains(month)) {  // handed out already: read the file again
    if (c.files[key] == s) c.files.erase(key);
    s = co_await shared_file(c, key, load);
    it = s->months.find(month);
  }
  if (it == s->months.end()) co_return std::nullopt;
  std::vector<float> v = std::move(it->second);
  s->months.erase(it);
  s->handed_out.insert(month);
  co_return v;
}

// Stream a URL through `feed`, failing on any HTTP error.
asio::awaitable<void> stream_url(Ctx& c, const std::string& url, const std::function<void(std::string_view)>& feed,
                                 std::optional<std::pair<std::uint64_t, std::uint64_t>> range) {
  net::Request rq;
  rq.url = url;
  if (range) {
    rq.range = *range;
    rq.decompress = false;
  }
  rq.on_data = [&](std::string_view b) {
    feed(b);
    return true;
  };
  auto r = co_await c.http.fetch(std::move(rq));
  if (!r.ok()) throw std::runtime_error("download failed: " + url + ": " + (r.error.empty() ? "HTTP " + std::to_string(r.status) : r.error));
}

bool any_finite(const std::vector<float>& v) {
  return std::any_of(v.begin(), v.end(), [](float x) { return std::isfinite(x); });
}

// Fill `into`'s gaps from `from`; returns the seconds filled.
std::int64_t fill_gaps(std::vector<float>& into, const std::vector<float>& from) {
  std::int64_t n = 0;
  for (std::size_t i = 0; i < into.size() && i < from.size(); ++i)
    if (std::isnan(into[i]) && std::isfinite(from[i])) into[i] = from[i], ++n;
  return n;
}

// Whether [a, b) (clipped to the month) is already stored in a partial month being
// fetched again: 99% of its seconds hold data.
bool already_have(const Ctx& c, const std::string& grid, const std::string& month, i64 a, i64 b) {
  auto it = c.have.find({grid, month});
  if (it == c.have.end()) return false;
  auto [t0, t1] = month_bounds(month);
  a = std::max(a, t0);
  b = std::min(b, t1);
  if (b <= a) return true;
  std::size_t n = 0;
  for (i64 t = a; t < b; ++t) n += std::isfinite(it->second[std::size_t(t - t0)]);
  return double(n) >= 0.99 * double(b - a);
}
// Add what was already stored to a freshly fetched (partly skipped) month.
void merge_have(const Ctx& c, const std::string& grid, const std::string& month, std::vector<float>& v) {
  if (auto it = c.have.find({grid, month}); it != c.have.end()) fill_gaps(v, it->second);
}

}  // namespace geo::refs::detail
