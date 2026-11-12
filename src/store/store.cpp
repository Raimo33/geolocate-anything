#include "store/store.hpp"

#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cstring>
#include <cmath>
#include <list>
#include <mutex>
#include <map>
#include <memory>
#include <set>
#include <unordered_map>

#include "store/codec.hpp"
#include "util/files.hpp"

namespace geo::store {

using codec::MonthFile;

// ---------------------------------------------------------------- paths

fs::path cache_dir() {
  std::string root = env("GEOLOC_CACHE");
  fs::path p = root.empty() ? fs::path(env("HOME", "/tmp")) / ".cache" / "geolocate-anything" : fs::path(root);
  std::error_code ec;
  fs::create_directories(p, ec);
  return p;
}

fs::path refs_dir() {
  fs::path p = cache_dir() / "refs";
  std::error_code ec;
  fs::create_directories(p, ec);
  return p;
}

int source_rank(std::string_view s) {
  static const std::map<std::string, int, std::less<>> rank = {
      {"neso", 0},     {"fingrid", 0},     {"aemo_fpp", 0},     {"aemo_causer_pays", 0}, {"aemo_annual", 2}, {"energy_charts", 5},
      {"netztransparenz", 8}, {"osf_pgf", 10}, {"seismic_east", 15}, {"osf_ireland", 10}, {"osf_edr", 20},
      {"seismic_ips", 60}, {"seismic_cn_south", 60}, {"seismic_cn_north", 60}, {"seismic_cn_east", 60}, {"seismic_cn_southwest", 60},
      {"seismic_brazil", 60}, {"seismic_swis", 60}, {"seismic_darwin", 60}, {"seismic_perth_basin", 60}, {"seismic_estonia", 60}, {"seismic_west", 60}, {"seismic_ercot", 60}, {"local", 100}};  // these seismic sources: after the measured recorders (unlisted sources rank 50)
  auto it = rank.find(s);
  return it == rank.end() ? 50 : it->second;
}

static fs::path src_dir(const std::string& grid, const std::string& source) { return refs_dir() / grid / source; }
static fs::path month_path(const std::string& grid, const std::string& source, const std::string& month) {
  return src_dir(grid, source) / (month + ".u16z");
}

// ---------------------------------------------------------------- caches
// Manifests and month-file maps are cached for the life of the process and updated by
// our own writes; invalidate_caches() drops them (e.g. after a bucket pull).

namespace {
std::unordered_map<std::string, json::object>& manifest_cache() {
  static std::unordered_map<std::string, json::object> c;
  return c;
}

// Open month files, most recently used first. Only files that exist are cached: a
// missing month is looked up again (one stat) on each use, so a file that appears
// later is never hidden by a stale "no such file" entry. Locked: the search reads
// blocks on several threads (a MonthFile itself is immutable once open).
struct MonthCache {
  static constexpr std::size_t CAP = 64;
  std::mutex mu;
  std::list<std::pair<std::string, std::shared_ptr<MonthFile>>> lru;
  std::shared_ptr<MonthFile> get(const std::string& grid, const std::string& source, const std::string& month) {
    fs::path p = month_path(grid, source, month);
    std::string key = p.string();
    std::lock_guard lk(mu);
    for (auto it = lru.begin(); it != lru.end(); ++it) {
      if (it->first == key) {
        lru.splice(lru.begin(), lru, it);
        return lru.front().second;
      }
    }
    struct stat st{};
    if (::stat(p.c_str(), &st) != 0) return nullptr;
    auto [a, b] = month_bounds(month);
    auto mf = std::make_shared<MonthFile>(p, a, b);
    lru.emplace_front(key, mf);
    if (lru.size() > CAP) lru.pop_back();
    return mf;
  }
  void drop(const fs::path& p) {
    std::string key = p.string();
    std::lock_guard lk(mu);
    lru.remove_if([&](auto& e) { return e.first == key; });
  }
  void clear() {
    std::lock_guard lk(mu);
    lru.clear();
  }
};
MonthCache& month_cache() {
  static MonthCache c;
  return c;
}
}  // namespace

void invalidate_caches() {
  manifest_cache().clear();
  month_cache().clear();
}

static const json::object& manifest_ref(const std::string& grid, const std::string& source) {
  fs::path p = src_dir(grid, source) / "manifest.json";
  auto& c = manifest_cache();
  auto it = c.find(p.string());
  if (it != c.end()) return it->second;
  json::object man;
  if (auto txt = read_file(p)) {
    auto v = parse_json(*txt);
    if (v.is_object()) man = std::move(v.get_object());
  }
  if (!man.contains("months")) {
    man["grid"] = grid;
    man["source"] = source;
    man["months"] = json::object();
  }
  return c.emplace(p.string(), std::move(man)).first->second;
}

json::object read_manifest(const std::string& grid, const std::string& source) { return manifest_ref(grid, source); }

void write_manifest(const std::string& grid, const std::string& source, const json::object& man) {
  update_manifest(grid, source, [&](json::object& m) { m = man; });
}

// Several processes may store months of one source at once (a backfill split over
// units): each change is made under an exclusive lock on the latest copy on disk, never
// on this process's cached one, so no process undoes another's entries.
void update_manifest(const std::string& grid, const std::string& source, const std::function<void(json::object&)>& change) {
  const fs::path dir = src_dir(grid, source), p = dir / "manifest.json", lp = dir / "manifest.json.lock";
  fs::create_directories(dir);
  const int fd = ::open(lp.c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0644);
  if (fd < 0) throw std::runtime_error("cannot open " + lp.string());
  struct Unlock {
    int fd;
    ~Unlock() { ::close(fd); }  // closing releases the lock
  } unlock{fd};
  while (::flock(fd, LOCK_EX) != 0)
    if (errno != EINTR) throw std::runtime_error("cannot lock " + lp.string());
  manifest_cache().erase(p.string());
  json::object man = manifest_ref(grid, source);  // fresh from disk
  const json::object before = man;
  change(man);
  if (man != before || !fs::exists(p)) atomic_write(p, dump(man, {.indent = 1, .sort_keys = true}));
  manifest_cache()[p.string()] = std::move(man);
}

std::vector<std::string> grid_dirs() {
  std::vector<std::string> out;
  std::error_code ec;
  for (auto& e : fs::directory_iterator(refs_dir(), ec))
    if (e.is_directory()) out.push_back(e.path().filename().string());
  std::sort(out.begin(), out.end());
  return out;
}

std::vector<std::string> sources_for(const std::string& grid) {
  std::vector<std::string> out;
  fs::path g = refs_dir() / grid;
  std::error_code ec;
  if (!fs::is_directory(g, ec)) return out;
  for (auto& e : fs::directory_iterator(g, ec))
    if (e.is_directory() && fs::exists(e.path() / "manifest.json")) out.push_back(e.path().filename().string());
  std::sort(out.begin(), out.end(), [](auto& a, auto& b) {
    int ra = source_rank(a), rb = source_rank(b);
    return ra != rb ? ra < rb : a < b;
  });
  return out;
}

static const json::object& months_of(const json::object& man) {
  static const json::object empty;
  auto* m = man.if_contains("months");
  return m && m->is_object() ? m->get_object() : empty;
}

bool has_month(const std::string& grid, const std::string& source, const std::string& month) {
  return months_of(manifest_ref(grid, source)).contains(month);
}

std::optional<json::object> month_info(const std::string& grid, const std::string& source, const std::string& month) {
  auto* v = months_of(manifest_ref(grid, source)).if_contains(month);
  if (!v || !v->is_object()) return std::nullopt;
  return v->get_object();
}

// ---------------------------------------------------------------- resampling

RegularAccumulator::RegularAccumulator(i64 t0, i64 t1)
    : t0_(t0), t1_(t1), t0d_(double(t0)), t1d_(double(t1)), sums_(static_cast<std::size_t>(t1 - t0), 0.0),
      counts_(static_cast<std::size_t>(t1 - t0), 0) {}

std::vector<float> RegularAccumulator::finish() const {
  const std::size_t n = sums_.size();
  std::vector<float> out(n, NAN);
  std::vector<std::int64_t> filled;
  for (std::size_t i = 0; i < n; ++i) {
    if (counts_[i] > 0) {
      out[i] = static_cast<float>(sums_[i] / counts_[i]);
      filled.push_back(static_cast<std::int64_t>(i));
    }
  }
  if (filled.size() < 2) return out;
  std::vector<std::int64_t> gaps(filled.size() - 1);
  for (std::size_t i = 0; i + 1 < filled.size(); ++i) gaps[i] = filled[i + 1] - filled[i];
  std::vector<std::int64_t> tmp = gaps;
  const std::size_t h = tmp.size() / 2;
  std::nth_element(tmp.begin(), tmp.begin() + h, tmp.end());
  double spacing = double(tmp[h]);
  if (tmp.size() % 2 == 0) {
    double lo = double(*std::max_element(tmp.begin(), tmp.begin() + h));
    spacing = 0.5 * (spacing + lo);
  }
  if (spacing <= 1.5) return out;
  const double limit = std::max(30.0, 3 * spacing);
  // Interpolate (from the double-precision means) across gaps no longer than limit.
  for (std::size_t k = 0; k + 1 < filled.size(); ++k) {
    std::int64_t a = filled[k], b = filled[k + 1];
    if (b - a <= 1) continue;
    if (gaps[k] > limit) continue;  // real outage stays NaN
    // numpy.interp's formula (same rounding): slope * (x - x0) + y0
    const double va = sums_[a] / counts_[a], vb = sums_[b] / counts_[b];
    const double slope = (vb - va) / double(b - a);
    for (std::int64_t i = a + 1; i < b; ++i) out[i] = static_cast<float>(slope * double(i - a) + va);
  }
  return out;
}

// ---------------------------------------------------------------- writing

// Fill mode: seg's codes go into the month's gaps. Replace mode: seg's seconds take seg's
// codes, MISSING included (what the stored month held there is dropped).
static void merge_month(const std::string& grid, const std::string& source, const std::string& month, i64 a,
                        std::span<const std::uint16_t> seg, bool replace = false) {
  auto [t0, t1] = month_bounds(month);
  const i64 n_chunks = (t1 - t0 + codec::CHUNK_S - 1) / codec::CHUNK_S;
  auto old = month_cache().get(grid, source, month);
  std::vector<std::string> blobs;
  std::vector<std::uint16_t> cur, nw;
  for (i64 i = 0; i < n_chunks; ++i) {
    i64 c0 = t0 + i * codec::CHUNK_S, c1 = std::min(t1, c0 + codec::CHUNK_S);
    std::string blob;
    if (old && std::size_t(i) < old->n_chunks()) {
      auto b = old->blob(std::size_t(i));
      blob.assign(reinterpret_cast<const char*>(b.data()), b.size());
    }
    i64 s = std::max(a, c0), e = std::min(a + i64(seg.size()), c1);
    if (s < e) {
      auto part = seg.subspan(std::size_t(s - a), std::size_t(e - s));
      if (replace ? !blob.empty() || std::any_of(part.begin(), part.end(), [](auto c) { return c != codec::MISSING; })
                  : std::any_of(part.begin(), part.end(), [](auto c) { return c != codec::MISSING; })) {
        cur.assign(std::size_t(c1 - c0), codec::MISSING);
        if (old && !blob.empty()) old->chunk(std::size_t(i), cur);
        nw = cur;
        for (std::size_t k = 0; k < part.size(); ++k)
          if (replace || part[k] != codec::MISSING) nw[std::size_t(s - c0) + k] = part[k];
        if (nw != cur) blob = codec::encode_chunk(nw);
      }
    }
    blobs.push_back(std::move(blob));
  }
  fs::path p = month_path(grid, source, month);
  month_cache().drop(p);
  old.reset();
  codec::write_month_file(p, blobs);
}

static void write_codes(const std::string& grid, const std::string& source, i64 t0, std::span<const float> dev, bool replace) {
  constexpr double step = codec::STORE_STEP_CODES / codec::SCALE;
  std::vector<std::uint16_t> codes(dev.size());
  for (std::size_t i = 0; i < dev.size(); ++i) {
    double v = dev[i];
    codes[i] = std::isfinite(v) ? codec::encode(std::nearbyint(v / step) * step) : codec::MISSING;
  }
  i64 t = t0;
  std::size_t i = 0;
  while (i < codes.size()) {
    Month m = Month::of(t);
    std::size_t n = std::min<std::size_t>(codes.size() - i, std::size_t(m.t1() - t));
    std::span<const std::uint16_t> seg(codes.data() + i, n);
    const bool data = std::any_of(seg.begin(), seg.end(), [](auto c) { return c != codec::MISSING; });
    if (data || (replace && fs::exists(month_path(grid, source, m.key()))))  // nothing to clear in a month never written
      merge_month(grid, source, m.key(), t, seg, replace);
    t += i64(n);
    i += n;
  }
}

void write_range(const std::string& grid, const std::string& source, i64 t0, std::span<const float> dev) {
  write_codes(grid, source, t0, dev, false);
}

void replace_range(const std::string& grid, const std::string& source, i64 t0, std::span<const float> dev) {
  write_codes(grid, source, t0, dev, true);
}

static double round5(double x) { return std::nearbyint(x * 1e5) / 1e5; }

static double finite_fraction(std::span<const float> v) {
  if (v.empty()) return 0.0;
  std::size_t k = 0;
  for (float x : v) k += std::isfinite(x);
  return double(k) / double(v.size());
}

static double stored_coverage(const std::string& grid, const std::string& source, const std::string& month) {
  auto [t0, t1] = month_bounds(month);
  return round5(finite_fraction(read(grid, t0, t1, {source})));
}

void write_entry(const std::string& grid, const std::string& source, const std::string& month, const json::object& meta) {
  // What the month holds now: this write, plus hours spilled in from its neighbours.
  const double coverage = stored_coverage(grid, source, month);
  update_manifest(grid, source, [&](json::object& man) {
  json::object& mm = man["meta"].is_object() ? man["meta"].get_object() : (man["meta"] = json::object()).get_object();
  for (auto& kv : meta)
    if (kv.key() != "url" && kv.key() != "files" && kv.key() != "partial" && kv.key() != "format" && kv.key() != "retry_chunks" &&
        kv.key() != "fetched_to")
      mm[kv.key()] = kv.value();
  json::object entry;
  entry["coverage"] = coverage;
  entry["stored_at"] = now_isoformat();
  entry["format"] = jint(meta, "format", 1);
  for (const char* k : {"url", "files", "retry_chunks", "fetched_to"})  // the last two: what a partial month still lacks
    if (auto* v = meta.if_contains(k)) entry[k] = *v;
  if (jbool(meta, "partial")) entry["partial"] = true;
  if (!man["months"].is_object()) man["months"] = json::object();
  man["months"].get_object()[month] = std::move(entry);
  if (auto* un = man.if_contains("unavailable"); un && un->is_object()) un->get_object().erase(month);
  });
}

void save_month(const std::string& grid, const std::string& source, const std::string& month, std::span<const float> values,
                const json::object& meta) {
  auto [t0, t1] = month_bounds(month);
  if (i64(values.size()) != t1 - t0)
    throw std::invalid_argument(month + ": expected " + std::to_string(t1 - t0) + " samples, got " + std::to_string(values.size()));
  write_range(grid, source, t0, values);
  write_entry(grid, source, month, meta);
}

void mark_unavailable(const std::string& grid, const std::string& source, const std::string& month, std::string_view reason, int format) {
  update_manifest(grid, source, [&](json::object& man) {
    if (!man["unavailable"].is_object()) man["unavailable"] = json::object();
    json::object e;
    e["checked_at"] = now_isoformat();
    e["reason"] = std::string(reason.substr(0, 200));
    e["format"] = format;
    man["unavailable"].get_object()[month] = std::move(e);
  });
}

void refresh_coverage(const std::string& grid, const std::string& source, const std::string& month) {
  if (!has_month(grid, source, month)) return;
  const double coverage = stored_coverage(grid, source, month);
  update_manifest(grid, source, [&](json::object& man) {
    auto* months = man.if_contains("months");
    auto* e = months && months->is_object() && months->get_object().contains(month) ? months->get_object()[month].if_object() : nullptr;
    if (!e) return;
    (*e)["coverage"] = coverage;
    (*e)["stored_at"] = now_isoformat();
  });
}

// ---------------------------------------------------------------- reading

void read(const std::string& grid, i64 t0, i64 t1, std::span<float> out, const std::vector<std::string>& sources) {
  std::fill(out.begin(), out.end(), NAN);
  const auto& srcs = sources.empty() ? sources_for(grid) : sources;
  static thread_local std::vector<std::uint16_t> codes;
  for (std::size_t si = 0; si < srcs.size(); ++si) {
    const auto& src = srcs[si];
    i64 t = t0;
    while (t < t1) {
      Month m = Month::of(t);
      i64 e = std::min(t1, m.t1());
      auto dst = out.subspan(std::size_t(t - t0), std::size_t(e - t));
      bool hole = si == 0 || std::any_of(dst.begin(), dst.end(), [](float v) { return v != v; });
      if (hole) {
        if (auto mf = month_cache().get(grid, src, m.key())) {
          codes.resize(dst.size());
          mf->codes(t, e, codes);
          // The two extreme codes are values the encoder clipped (|dev| > 3.27 Hz): junk
          // rows that older parsers let through, never a real grid frequency.
          codec::decode_into(codes, dst);
        }
      }
      t = e;
    }
  }
}

std::vector<float> read(const std::string& grid, i64 t0, i64 t1, const std::vector<std::string>& sources) {
  std::vector<float> out(static_cast<std::size_t>(std::max<i64>(0, t1 - t0)));
  read(grid, t0, t1, out, sources);
  return out;
}

std::optional<i64> last_data_end(const std::string& grid, i64 t1, i64 max_back) {
  for (i64 back = 86400; back <= max_back * 2; back *= 2) {
    i64 a = t1 - std::min(back, max_back);
    auto v = read(grid, a, t1);
    for (std::size_t k = v.size(); k-- > 0;)
      if (std::isfinite(v[k])) return a + i64(k) + 1;
    if (back >= max_back) break;
  }
  return std::nullopt;
}

std::vector<Interval> coverage_intervals(const std::string& grid, const std::vector<std::string>& sources) {
  std::vector<Interval> spans;
  for (const auto& s : sources.empty() ? sources_for(grid) : sources)
    for (auto& kv : months_of(manifest_ref(grid, s))) {
      auto [a, b] = month_bounds(kv.key());
      spans.push_back({a, b});
    }
  std::sort(spans.begin(), spans.end(), [](auto& x, auto& y) { return x.a != y.a ? x.a < y.a : x.b < y.b; });
  std::vector<Interval> out;
  for (auto& s : spans) {
    if (!out.empty() && s.a <= out.back().b)
      out.back().b = std::max(out.back().b, s.b);
    else
      out.push_back(s);
  }
  return out;
}

std::vector<std::pair<i64, double>> month_fill(const std::string& grid) {
  std::map<i64, double> best;
  for (const auto& s : sources_for(grid))
    for (auto& kv : months_of(manifest_ref(grid, s))) {
      double c = jnum(kv.value(), "coverage", 1.0);
      if (!std::isfinite(c)) c = 1.0;
      auto& b = best[month_bounds(kv.key()).first];
      b = std::max(b, std::clamp(c, 0.0, 1.0));
    }
  return {best.begin(), best.end()};
}

double RefSeries::coverage() const { return finite_fraction(values); }

std::optional<RefSeries> load_grid(const std::string& grid, std::optional<i64> from, std::optional<i64> to) {
  auto spans = coverage_intervals(grid);
  if (spans.empty()) return std::nullopt;
  i64 a = from ? std::max(*from, spans.front().a) : spans.front().a;
  i64 b = to ? std::min(*to, spans.back().b) : spans.back().b;
  if (b <= a) return std::nullopt;
  return RefSeries{grid, a, read(grid, a, b)};
}

std::vector<Inventory> inventory() {
  std::vector<Inventory> out;
  for (auto& g : grid_dirs())
    for (auto& s : sources_for(g)) {
      std::vector<std::string> months;
      for (auto& kv : months_of(manifest_ref(g, s))) months.emplace_back(kv.key());
      std::sort(months.begin(), months.end());
      if (!months.empty()) out.push_back({g, s, std::move(months)});
    }
  return out;
}

std::uint64_t disk_usage(const std::string& grid) {
  std::uint64_t total = 0;
  std::error_code ec;
  for (auto& e : fs::recursive_directory_iterator(refs_dir(), ec)) {
    if (!e.is_regular_file()) continue;
    auto ext = e.path().extension().string();
    if (ext != ".u16z" && ext != ".u16") continue;
    if (!grid.empty() && e.path().parent_path().parent_path().filename() != grid) continue;
    struct stat st{};
    if (::stat(e.path().c_str(), &st) == 0) total += std::uint64_t(st.st_blocks) * 512;
  }
  return total;
}

}  // namespace geo::store
