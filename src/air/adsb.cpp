#include "air/adsb.hpp"

#include <zlib.h>
#include <zstd.h>

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <fstream>
#include <iostream>
#include <map>
#include <mutex>
#include <thread>
#include <tuple>
#include <unordered_map>

#include "air/geodesy.hpp"
#include "net/http.hpp"
#include "store/store.hpp"
#include "util/json.hpp"
#include "util/parallel.hpp"
#include "util/time.hpp"

namespace geo::air {

namespace {

constexpr char MAGIC[8] = {'G', 'L', 'A', 'D', 'S', 'B', '0', '1'};
constexpr const char* TYPES_URL = "https://raw.githubusercontent.com/wiedehopf/tar1090-db/master/db/icao_aircraft_types.js";
constexpr const char* AIRCRAFT_URL = "https://github.com/wiedehopf/tar1090-db/raw/csv/aircraft.csv.gz";

// ---------------------------------------------------------------- gzip

// Inflates a gzip member; stops once `limit` bytes are out (0 = all).
std::string gunzip(std::string_view in, std::size_t limit = 0) {
  z_stream z{};
  if (inflateInit2(&z, 16 + MAX_WBITS) != Z_OK) throw std::runtime_error("inflateInit2 failed");
  std::string out;
  out.resize(limit ? limit : std::max<std::size_t>(in.size() * 8, 4096));
  z.next_in = reinterpret_cast<Bytef*>(const_cast<char*>(in.data()));
  z.avail_in = uInt(in.size());
  std::size_t have = 0;
  for (;;) {
    z.next_out = reinterpret_cast<Bytef*>(out.data() + have);
    z.avail_out = uInt(out.size() - have);
    const int r = inflate(&z, Z_NO_FLUSH);
    have = out.size() - z.avail_out;
    if (r == Z_STREAM_END) break;
    if (r != Z_OK && r != Z_BUF_ERROR) {
      inflateEnd(&z);
      throw std::runtime_error("corrupt gzip member");
    }
    if (limit && have >= limit) break;
    if (z.avail_out == 0) out.resize(out.size() * 2);
    else if (z.avail_in == 0) break;  // truncated
  }
  inflateEnd(&z);
  out.resize(have);
  return out;
}

// ---------------------------------------------------------------- tar

// Streaming ustar/GNU tar reader: feed() any chunking; calls on_member(name, bytes) for
// each regular file whose name passes want().
class TarStream {
 public:
  std::function<bool(const std::string&)> want;
  std::function<void(std::string name, std::string data)> on_member;

  void feed(std::string_view b) {
    while (!b.empty()) {
      if (state_ == Header) {
        const std::size_t take = std::min(b.size(), 512 - hdr_.size());
        hdr_.append(b.substr(0, take));
        b.remove_prefix(take);
        if (hdr_.size() == 512) start_member();
      } else if (state_ == Data) {
        const std::size_t take = std::size_t(std::min<std::uint64_t>(b.size(), left_));
        if (keep_) data_.append(b.substr(0, take));
        b.remove_prefix(take);
        left_ -= take;
        if (!left_) end_member();
      } else {  // padding to the next 512-byte boundary
        const std::size_t take = std::size_t(std::min<std::uint64_t>(b.size(), pad_));
        b.remove_prefix(take);
        pad_ -= take;
        if (!pad_) state_ = Header;
      }
    }
  }
  bool ended() const { return zero_blocks_ >= 1; }

 private:
  enum State { Header, Data, Pad } state_ = Header;
  std::string hdr_, data_, name_, long_name_;
  std::uint64_t left_ = 0, size_ = 0, pad_ = 0;
  char type_ = 0;
  bool keep_ = false;
  int zero_blocks_ = 0;

  static std::uint64_t octal(std::string_view f) {
    if (!f.empty() && (static_cast<unsigned char>(f[0]) & 0x80)) {  // base-256
      std::uint64_t v = 0;
      for (std::size_t i = 1; i < f.size(); ++i) v = (v << 8) | static_cast<unsigned char>(f[i]);
      return v;
    }
    std::uint64_t v = 0;
    for (char c : f)
      if (c >= '0' && c <= '7') v = v * 8 + std::uint64_t(c - '0');
    return v;
  }
  static std::string cstr(std::string_view f) { return std::string(f.substr(0, std::min(f.size(), f.find('\0')))); }

  void start_member() {
    if (std::all_of(hdr_.begin(), hdr_.end(), [](char c) { return c == 0; })) {
      ++zero_blocks_;
      hdr_.clear();
      return;
    }
    std::string_view h(hdr_);
    type_ = h[156];
    size_ = octal(h.substr(124, 12));
    name_ = cstr(h.substr(0, 100));
    const std::string prefix = cstr(h.substr(345, 155));
    if (h.substr(257, 5) == "ustar" && !prefix.empty()) name_ = prefix + "/" + name_;
    if (!long_name_.empty() && type_ != 'L') name_ = std::move(long_name_), long_name_.clear();
    keep_ = type_ == 'L' || ((type_ == '0' || type_ == 0) && (!want || want(name_)));
    data_.clear();
    if (keep_) data_.reserve(std::size_t(size_));
    left_ = size_;
    pad_ = (512 - size_ % 512) % 512;
    hdr_.clear();
    if (left_) state_ = Data;
    else end_member();
  }
  void end_member() {
    if (type_ == 'L') long_name_ = cstr(data_);
    else if (keep_ && on_member) on_member(std::move(name_), std::move(data_));
    data_.clear();
    state_ = pad_ ? Pad : Header;
  }
};

// ---------------------------------------------------------------- columnar blobs

template <class T>
void put(std::string& s, const T& v) {
  s.append(reinterpret_cast<const char*>(&v), sizeof v);
}
template <class T>
T get(std::string_view& s) {
  if (s.size() < sizeof(T)) throw std::runtime_error("truncated track store");
  T v;
  std::memcpy(&v, s.data(), sizeof v);
  s.remove_prefix(sizeof v);
  return v;
}

struct IndexEntry {
  std::uint32_t icao = 0;
  char type[4] = {};
  char reg[8] = {};
  char kind = '?';
  std::uint8_t engines = 0, engine = 0, pad = 0;
  float west = 0, south = 0, east = 0, north = 0;
  double t0 = 0, t1 = 0;
  std::uint64_t offset = 0;
  std::uint32_t clen = 0, nfix = 0;
};
static_assert(sizeof(IndexEntry) == 72);

std::string fixes_blob(const Track& tr) {
  const std::size_t n = tr.fixes.size();
  std::string raw;
  raw.reserve(n * 29);
  const double t0 = n ? tr.fixes[0].t : 0;
  for (auto& f : tr.fixes) put(raw, std::int32_t(std::lround((f.t - t0) * 1000.0)));
  for (auto& f : tr.fixes) put(raw, std::int32_t(std::lround(f.lat * 1e6)));
  for (auto& f : tr.fixes) put(raw, std::int32_t(std::lround(f.lon * 1e6)));
  for (auto& f : tr.fixes) put(raw, f.alt_m);
  for (auto& f : tr.fixes) put(raw, f.gs);
  for (auto& f : tr.fixes) put(raw, f.track);
  for (auto& f : tr.fixes) put(raw, f.vrate);
  for (auto& f : tr.fixes) put(raw, std::uint8_t(std::uint8_t(f.alt_src) | (f.leg_start ? 0x80 : 0)));
  return raw;
}

std::vector<Fix> fixes_of(std::string_view raw, std::size_t n, double t0) {
  std::vector<Fix> fx(n);
  for (auto& f : fx) f.t = t0 + get<std::int32_t>(raw) / 1000.0;
  for (auto& f : fx) f.lat = get<std::int32_t>(raw) / 1e6;
  for (auto& f : fx) f.lon = get<std::int32_t>(raw) / 1e6;
  for (auto& f : fx) f.alt_m = get<float>(raw);
  for (auto& f : fx) f.gs = get<float>(raw);
  for (auto& f : fx) f.track = get<float>(raw);
  for (auto& f : fx) f.vrate = get<float>(raw);
  for (auto& f : fx) {
    const auto b = get<std::uint8_t>(raw);
    f.alt_src = AltSource(b & 0x7f), f.leg_start = b & 0x80;
  }
  return fx;
}

IndexEntry index_of(const Track& tr) {
  IndexEntry e;
  e.icao = tr.icao;
  std::memcpy(e.type, tr.type.data(), std::min<std::size_t>(4, tr.type.size()));
  std::memcpy(e.reg, tr.reg.data(), std::min<std::size_t>(8, tr.reg.size()));
  e.kind = tr.info.kind, e.engines = std::uint8_t(tr.info.engines), e.engine = std::uint8_t(tr.info.engine);
  e.west = 180, e.south = 90, e.east = -180, e.north = -90;
  for (auto& f : tr.fixes) {
    e.west = std::min(e.west, float(f.lon)), e.east = std::max(e.east, float(f.lon));
    e.south = std::min(e.south, float(f.lat)), e.north = std::max(e.north, float(f.lat));
  }
  e.t0 = tr.fixes.front().t, e.t1 = tr.fixes.back().t;
  e.nfix = std::uint32_t(tr.fixes.size());
  return e;
}

Track track_of(const IndexEntry& e) {
  Track tr;
  tr.icao = e.icao;
  tr.type = std::string(e.type, strnlen(e.type, 4));
  tr.reg = std::string(e.reg, strnlen(e.reg, 8));
  tr.info = {e.kind, e.engines, EngineType(e.engine)};
  return tr;
}

std::string zstd_compress(std::string_view raw, int level) {
  std::string out(ZSTD_compressBound(raw.size()), '\0');
  const std::size_t r = ZSTD_compress(out.data(), out.size(), raw.data(), raw.size(), level);
  if (ZSTD_isError(r)) throw std::runtime_error(std::string("zstd: ") + ZSTD_getErrorName(r));
  out.resize(r);
  return out;
}
std::string zstd_decompress(std::string_view c) {
  const auto n = ZSTD_getFrameContentSize(c.data(), c.size());
  if (n == ZSTD_CONTENTSIZE_ERROR || n == ZSTD_CONTENTSIZE_UNKNOWN) throw std::runtime_error("corrupt track blob");
  std::string out(std::size_t(n), '\0');
  const std::size_t r = ZSTD_decompress(out.data(), out.size(), c.data(), c.size());
  if (ZSTD_isError(r) || r != n) throw std::runtime_error("corrupt track blob");
  return out;
}

// ---------------------------------------------------------------- altitude offsets

// Geometric-minus-barometric offsets (ft) per 2 deg x 2 deg x 3 h cell, a bounded sample each.
class RegionOffsets {
 public:
  static std::uint64_t key(double lat, double lon, double t) {
    const auto a = std::uint64_t(std::clamp(int((lat + 90) / 2), 0, 89));
    const auto b = std::uint64_t(std::clamp(int((lon + 180) / 2), 0, 179));
    const auto c = std::uint64_t(std::max<i64>(0, i64(std::floor(t / 10800.0)) + 1000000));
    return (c << 16) | (a << 8) | b;
  }
  void add(std::uint64_t k, float off) {
    std::lock_guard lk(mu_);
    auto& v = s_[k];
    if (v.size() < 4000) v.push_back(off);
  }
  void finish() {
    for (auto& [k, v] : s_)
      if (v.size() >= 5) {
        std::nth_element(v.begin(), v.begin() + std::ptrdiff_t(v.size() / 2), v.end());
        med_[k] = v[v.size() / 2];
      }
  }
  std::optional<float> at(double lat, double lon, double t) const {
    auto it = med_.find(key(lat, lon, t));
    if (it == med_.end()) return std::nullopt;
    return it->second;
  }

 private:
  std::mutex mu_;
  std::unordered_map<std::uint64_t, std::vector<float>> s_;
  std::unordered_map<std::uint64_t, float> med_;
};

double num(const json::value& v) {
  if (v.is_double()) return v.get_double();
  if (v.is_int64()) return double(v.get_int64());
  if (v.is_uint64()) return double(v.get_uint64());
  return NAN;
}

}  // namespace

// ---------------------------------------------------------------- public

const char* engine_name(EngineType e) {
  switch (e) {
    case EngineType::Piston: return "piston";
    case EngineType::Turboprop: return "turboprop";
    case EngineType::Jet: return "jet";
    case EngineType::Electric: return "electric";
    case EngineType::Rocket: return "rocket";
    default: return "unknown";
  }
}

std::string Track::id() const {
  char b[8];
  std::snprintf(b, sizeof b, "%06x", icao);
  return b;
}

Bbox Bbox::grown(double m) const {
  const double dlat = m / 111320.0;
  const double c = std::max(0.05, std::cos(std::max(std::abs(south), std::abs(north)) * DEG));
  const double dlon = dlat / c;
  return {std::max(-180.0, west - dlon), std::max(-90.0, south - dlat), std::min(180.0, east + dlon), std::min(90.0, north + dlat)};
}

TypeDb TypeDb::parse(std::string_view text) {
  TypeDb db;
  auto v = parse_json(text);
  for (auto& kv : v.as_object()) {
    const std::string desc = jstr(kv.value(), "desc");
    if (desc.size() != 3 || kv.key().size() > 4) continue;
    TypeInfo t;
    t.kind = desc[0];
    t.engines = desc[1] >= '0' && desc[1] <= '9' ? desc[1] - '0' : 0;
    switch (desc[2]) {
      case 'P': t.engine = EngineType::Piston; break;
      case 'T': t.engine = EngineType::Turboprop; break;
      case 'J': t.engine = EngineType::Jet; break;
      case 'E': t.engine = EngineType::Electric; break;
      case 'R': t.engine = EngineType::Rocket; break;
      default: break;
    }
    std::array<char, 4> code{};
    std::memcpy(code.data(), kv.key().data(), kv.key().size());
    db.codes_.emplace_back(code, t);
  }
  std::sort(db.codes_.begin(), db.codes_.end(), [](auto& a, auto& b) { return a.first < b.first; });
  return db;
}

std::optional<TypeInfo> TypeDb::find(std::string_view d) const {
  if (d.empty() || d.size() > 4) return std::nullopt;
  std::array<char, 4> code{};
  std::memcpy(code.data(), d.data(), d.size());
  auto it = std::lower_bound(codes_.begin(), codes_.end(), code, [](auto& a, auto& b) { return a.first < b; });
  if (it == codes_.end() || it->first != code) return std::nullopt;
  return it->second;
}

const TypeDb& TypeDb::get() {
  static const TypeDb db = [] {
    const fs::path p = adsb_dir() / "icao_aircraft_types.json";
    auto text = read_file(p);
    if (!text) {
      auto body = net::run_sync<std::string>([](net::asio::io_context& io) -> net::asio::awaitable<std::string> {
        net::Http http(io);
        auto r = co_await http.get(TYPES_URL);
        co_return r.body;
      });
      if (body.size() > 2 && static_cast<unsigned char>(body[0]) == 0x1f && static_cast<unsigned char>(body[1]) == 0x8b) body = gunzip(body);
      parse_json(body);  // validate before caching
      fs::create_directories(p.parent_path());
      atomic_write(p, body);
      text = std::move(body);
    }
    return TypeDb::parse(*text);
  }();
  return db;
}

AircraftDb AircraftDb::parse(std::string_view csv) {
  AircraftDb db;
  while (!csv.empty()) {
    const auto nl = csv.find('\n');
    std::string_view line = csv.substr(0, nl);
    csv.remove_prefix(nl == std::string_view::npos ? csv.size() : nl + 1);
    // icao;registration;type;flags;description;...
    const auto a = line.find(';');
    if (a == std::string_view::npos || a != 6) continue;
    const auto b = line.find(';', a + 1);
    if (b == std::string_view::npos) continue;
    const auto c = line.find(';', b + 1);
    const std::string_view type = line.substr(b + 1, (c == std::string_view::npos ? line.size() : c) - b - 1);
    if (type.empty() || type.size() > 4) continue;
    std::array<char, 4> t{};
    std::memcpy(t.data(), type.data(), type.size());
    db.rows_.emplace_back(std::uint32_t(std::strtoul(std::string(line.substr(0, 6)).c_str(), nullptr, 16)), t);
  }
  std::sort(db.rows_.begin(), db.rows_.end());
  return db;
}

std::string AircraftDb::type(std::uint32_t icao) const {
  auto it = std::lower_bound(rows_.begin(), rows_.end(), icao, [](auto& r, std::uint32_t k) { return r.first < k; });
  if (it == rows_.end() || it->first != icao) return {};
  return std::string(it->second.data(), strnlen(it->second.data(), 4));
}

const AircraftDb& AircraftDb::get() {
  static const AircraftDb db = [] {
    const fs::path p = adsb_dir() / "aircraft.csv.gz";
    auto gz = read_file(p);
    if (!gz) try {
        auto body = net::run_sync<std::string>([](net::asio::io_context& io) -> net::asio::awaitable<std::string> {
          net::Http http(io);
          auto r = co_await http.fetch({.url = AIRCRAFT_URL, .total_timeout_s = 600, .decompress = false});
          if (!r.ok()) throw std::runtime_error("HTTP " + std::to_string(r.status) + " " + r.error);
          co_return r.body;
        });
        gunzip(body);  // validate before caching
        fs::create_directories(p.parent_path());
        atomic_write(p, body);
        gz = std::move(body);
      } catch (const std::exception& e) {
        std::cerr << "warning: aircraft registry unavailable (" << e.what() << "): types missing from traces stay unknown" << std::endl;
        return AircraftDb();
      }
    return AircraftDb::parse(gunzip(*gz));
  }();
  return db;
}

std::map<std::uint32_t, RegistryEntry> registry_lookup(const std::set<std::uint32_t>& icaos) {
  std::map<std::uint32_t, RegistryEntry> out;
  if (icaos.empty()) return out;
  AircraftDb::get();  // downloads the registry when it is missing
  auto gz = read_file(adsb_dir() / "aircraft.csv.gz");
  if (!gz) return out;
  const std::string csv = gunzip(*gz);
  std::string_view rest(csv);
  while (!rest.empty() && out.size() < icaos.size()) {
    const auto nl = rest.find('\n');
    std::string_view line = rest.substr(0, nl);
    rest.remove_prefix(nl == std::string_view::npos ? rest.size() : nl + 1);
    if (line.size() < 7 || line[6] != ';') continue;
    const auto icao = std::uint32_t(std::strtoul(std::string(line.substr(0, 6)).c_str(), nullptr, 16));
    if (!icaos.count(icao)) continue;
    // icao;registration;type;flags;description;year;owner;
    std::vector<std::string_view> f;
    for (std::size_t a = 0; a <= line.size();) {
      const auto b = std::min(line.size(), line.find(';', a));
      f.push_back(line.substr(a, b - a));
      a = b + 1;
    }
    f.resize(std::max<std::size_t>(f.size(), 7));
    out[icao] = {std::string(f[1]), std::string(f[2]), std::string(f[4]), std::string(f[5]), std::string(f[6])};
  }
  return out;
}

fs::path adsb_dir() { return store::cache_dir() / "adsb"; }
fs::path day_path(const std::string& day) { return adsb_dir() / (day + ".trk"); }
bool has_day(const std::string& day) { return fs::exists(day_path(day)); }

std::vector<std::string> stored_days() {
  std::vector<std::string> out;
  std::error_code ec;
  for (auto& e : fs::directory_iterator(adsb_dir(), ec))
    if (e.path().extension() == ".trk") out.push_back(e.path().stem().string());
  std::sort(out.begin(), out.end());
  return out;
}

namespace {
struct OffsetSample {
  double lat, lon, t;
  float off_ft;
};
std::optional<Track> parse_trace_impl(std::string_view text, const TypeDb& types, bool keep_jets, const std::optional<Bbox>& bbox,
                                      std::vector<OffsetSample>* samples) {
  json::value v = parse_json(text);
  if (!v.is_object()) return std::nullopt;
  Track tr;
  const std::string hex = jstr(v, "icao");
  if (hex.empty() || hex[0] == '~') return std::nullopt;  // non-ICAO (TIS-B) addresses
  tr.icao = std::uint32_t(std::strtoul(hex.c_str(), nullptr, 16));
  tr.type = jstr(v, "t");
  tr.reg = jstr(v, "r");
  if (tr.type.empty()) tr.type = AircraftDb::get().type(tr.icao);
  if (auto ti = types.find(tr.type)) tr.info = *ti;
  if (tr.info.engine == EngineType::Jet && !keep_jets) return std::nullopt;
  const double base = jnum(v, "timestamp");
  const json::value* trace = jget(v, "trace");
  if (!trace || !trace->is_array() || !std::isfinite(base)) return std::nullopt;
  std::vector<float> offsets;
  for (auto& pv : trace->as_array()) {
    if (!pv.is_array()) continue;
    auto& p = pv.as_array();
    if (p.size() < 8) continue;
    Fix f;
    f.t = base + num(p[0]);
    f.lat = num(p[1]), f.lon = num(p[2]);
    if (!std::isfinite(f.t) || !std::isfinite(f.lat) || !std::isfinite(f.lon)) continue;
    if (p[3].is_string()) continue;  // "ground"
    const int flags = int(num(p[6]));
    f.leg_start = flags & 2;
    double baro = NAN, geom = NAN;
    if (flags & 8) geom = num(p[3]);
    else baro = num(p[3]);
    if (p.size() > 10 && !std::isfinite(geom)) geom = num(p[10]);
    if (!std::isfinite(geom) && p[8].is_object()) geom = jnum(p[8], "alt_geom");
    if (std::isfinite(geom) && std::isfinite(baro)) {
      offsets.push_back(float(geom - baro));
      if (samples && offsets.size() % 8 == 1) samples->push_back({f.lat, f.lon, f.t, float(geom - baro)});
    }
    if (std::isfinite(geom)) f.alt_m = float(geom * FT), f.alt_src = AltSource::Geometric;
    else if (std::isfinite(baro)) f.alt_m = float(baro * FT), f.alt_src = AltSource::BaroRaw;
    else continue;
    f.gs = float(num(p[4]) * KT);
    f.track = float(num(p[5]));
    const double vr = (p.size() > 11 && std::isfinite(num(p[11]))) ? num(p[11]) : num(p[7]);
    f.vrate = float(vr * FPM);
    if (bbox && !bbox->contains(f.lat, f.lon)) continue;
    tr.fixes.push_back(f);
  }
  if (tr.fixes.empty()) return std::nullopt;
  std::sort(tr.fixes.begin(), tr.fixes.end(), [](const Fix& a, const Fix& b) { return a.t < b.t; });
  if (offsets.size() >= 3) {
    std::nth_element(offsets.begin(), offsets.begin() + std::ptrdiff_t(offsets.size() / 2), offsets.end());
    const float off = offsets[offsets.size() / 2] * float(FT);
    for (auto& f : tr.fixes)
      if (f.alt_src == AltSource::BaroRaw) f.alt_m += off, f.alt_src = AltSource::BaroTrack;
  }
  return tr;
}
}  // namespace

std::optional<Track> parse_trace(std::string_view text, const TypeDb& types, bool keep_jets, const std::optional<Bbox>& bbox) {
  return parse_trace_impl(text, types, keep_jets, bbox, nullptr);
}

std::string encode_tracks(const std::vector<Track>& tracks) {
  std::string head(MAGIC, 8), blobs;
  put(head, std::uint32_t(0));
  std::vector<IndexEntry> idx;
  for (auto& tr : tracks) {
    if (tr.fixes.empty()) continue;
    auto e = index_of(tr);
    auto c = zstd_compress(fixes_blob(tr), 3);
    e.offset = blobs.size(), e.clen = std::uint32_t(c.size());
    blobs += c;
    idx.push_back(e);
  }
  const auto n = std::uint32_t(idx.size());
  std::memcpy(head.data() + 8, &n, 4);
  for (auto& e : idx) put(head, e);
  return head + blobs;
}

namespace {

struct DayFile {
  MappedFile mf;
  std::vector<IndexEntry> idx;
  std::size_t data_off = 0;
  explicit DayFile(const fs::path& p) : mf(p) {
    std::string_view v = mf.view();
    if (v.size() < 12 || std::memcmp(v.data(), MAGIC, 8) != 0) throw std::runtime_error("not a track store: " + p.string());
    v.remove_prefix(8);
    const auto n = get<std::uint32_t>(v);
    if (v.size() < std::size_t(n) * sizeof(IndexEntry)) throw std::runtime_error("truncated track store: " + p.string());
    idx.resize(n);
    std::memcpy(idx.data(), v.data(), std::size_t(n) * sizeof(IndexEntry));
    data_off = 12 + std::size_t(n) * sizeof(IndexEntry);
  }
  std::vector<Fix> fixes(const IndexEntry& e) const {
    auto raw = zstd_decompress(mf.view().substr(data_off + e.offset, e.clen));
    return fixes_of(raw, e.nfix, e.t0);
  }
};

}  // namespace

namespace {

// Aircraft-minutes per cell for one stored day, cached next to it.
std::map<std::pair<int, int>, double> day_coverage(const std::string& day, bool force = false) {
  std::map<std::pair<int, int>, double> out;
  const fs::path side = adsb_dir() / (day + ".cov");
  if (!force)
    if (auto txt = read_file(side); txt && fs::last_write_time(side) >= fs::last_write_time(day_path(day))) {
      for (auto& c : parse_json(*txt).as_object().at("cells").as_array()) {
        auto& a = c.as_array();
        out[{int(as_number(a[0])), int(as_number(a[1]))}] = as_number(a[2]);
      }
      return out;
    }
  DayFile df(day_path(day));
  std::mutex mu;
  parallel_for(df.idx.size(), [&](std::size_t i) {
    const auto fx = df.fixes(df.idx[i]);
    std::set<std::tuple<int, int, i64>> seen;  // (cell, minute) of this aircraft
    for (auto& f : fx)
      seen.insert({int(std::floor((f.lat + 90) / COVERAGE_DEG)), int(std::floor((f.lon + 180) / COVERAGE_DEG)), i64(std::floor(f.t / 60))});
    std::map<std::pair<int, int>, double> loc;
    for (auto& [a, b, m] : seen) loc[{a, b}] += 1;
    std::lock_guard lk(mu);
    for (auto& [k, v] : loc) out[k] += v;
  });
  json::array cells;
  for (auto& [k, v] : out) cells.push_back(json::array{k.first, k.second, v});
  atomic_write(side, dump(json::object{{"day", day}, {"cell_deg", COVERAGE_DEG}, {"cells", cells}}));
  return out;
}

}  // namespace

std::string coverage_json() {
  std::map<std::pair<int, int>, double> all;
  json::array days;
  for (auto& d : stored_days()) {
    try {
      for (auto& [k, v] : day_coverage(d)) all[k] += v;
      days.push_back(json::value(d));
    } catch (const std::exception& e) {
      std::cerr << "coverage of " << d << ": " << e.what() << std::endl;
    }
  }
  json::array cells;
  for (auto& [k, v] : all) cells.push_back(json::array{k.first * COVERAGE_DEG - 90, k.second * COVERAGE_DEG - 180, v});
  return dump(json::object{{"cell_deg", COVERAGE_DEG}, {"days", days}, {"cells", cells}});
}

std::vector<Track> decode_tracks(std::string_view bytes) {
  TempFile tmp(".trk");
  atomic_write(tmp.path, bytes);
  DayFile df(tmp.path);
  std::vector<Track> out;
  for (auto& e : df.idx) {
    auto tr = track_of(e);
    tr.fixes = df.fixes(e);
    out.push_back(std::move(tr));
  }
  return out;
}

IngestStats ingest_day(const std::string& day, const IngestOptions& o) {
  const auto t_day = parse_iso_utc(day);
  if (!t_day || day.size() != 10) throw std::invalid_argument("day must be YYYY-MM-DD: " + day);
  const TypeDb& types = TypeDb::get();
  const AircraftDb& aircraft = AircraftDb::get();
  fs::create_directories(adsb_dir());
  IngestStats st;

  // Workers inflate and parse members; kept tracks go, compressed, to a spill file in
  // arrival order (memory stays bounded whatever the day's size).
  const fs::path spill_path = adsb_dir() / (day + ".spill");
  std::ofstream spill(spill_path, std::ios::binary | std::ios::trunc);
  std::vector<IndexEntry> idx;
  std::uint64_t spill_size = 0;
  RegionOffsets region;
  std::mutex out_mu, q_mu;
  std::condition_variable q_cv, q_space;
  std::deque<std::pair<std::string, std::string>> queue;  // (member name, gzip bytes)
  bool closing = false;
  std::atomic<std::size_t> members{0}, kept{0}, jets{0}, ground{0}, unknown{0};
  std::exception_ptr err;
  const std::size_t q_max = 256;

  auto work = [&] {
    for (;;) {
      std::string gz, name;
      {
        std::unique_lock lk(q_mu);
        q_cv.wait(lk, [&] { return !queue.empty() || closing; });
        if (queue.empty()) return;
        name = std::move(queue.front().first), gz = std::move(queue.front().second);
        queue.pop_front();
      }
      q_space.notify_one();
      try {
        ++members;
        // The type designator sits in the first few hundred bytes: drop jets unparsed.
        const std::string head = gunzip(gz, 512);
        std::string type;
        if (auto p = head.find("\"t\":\""); p != std::string::npos) type = head.substr(p + 5, head.find('"', p + 5) - p - 5);
        if (type.empty())  // trace_full_<icao>.json: the registry may know the type
          if (auto p = name.rfind("trace_full_"); p != std::string::npos)
            type = aircraft.type(std::uint32_t(std::strtoul(name.substr(p + 11, 6).c_str(), nullptr, 16)));
        const auto ti = types.find(type);
        if (!ti) ++unknown;
        if (ti && ti->engine == EngineType::Jet && !o.keep_jets) {
          ++jets;
          continue;
        }
        std::vector<OffsetSample> samples;
        auto tr = parse_trace_impl(gunzip(gz), types, o.keep_jets, o.bbox, &samples);
        if (!tr) {
          ++ground;
          continue;
        }
        for (auto& x : samples) region.add(RegionOffsets::key(x.lat, x.lon, x.t), x.off_ft);
        auto e = index_of(*tr);
        auto c = zstd_compress(fixes_blob(*tr), 3);
        ++kept;
        std::lock_guard lk(out_mu);
        e.offset = spill_size, e.clen = std::uint32_t(c.size());
        spill.write(c.data(), std::streamsize(c.size()));
        spill_size += c.size();
        idx.push_back(e);
      } catch (...) {
        std::lock_guard lk(out_mu);
        if (!err) err = std::current_exception();
      }
    }
  };
  std::vector<std::jthread> pool;
  const int nt = std::max(1, worker_threads(o.threads) - 1);
  for (int i = 0; i < nt; ++i) pool.emplace_back(work);

  TarStream tar;
  tar.want = [](const std::string& n) { return n.find("trace_full_") != std::string::npos && n.ends_with(".json"); };
  tar.on_member = [&](std::string name, std::string data) {
    std::unique_lock lk(q_mu);
    q_space.wait(lk, [&] { return queue.size() < q_max; });
    queue.emplace_back(std::move(name), std::move(data));
    lk.unlock();
    q_cv.notify_one();
  };
  auto finish_workers = [&] {
    {
      std::lock_guard lk(q_mu);
      closing = true;
    }
    q_cv.notify_all();
    pool.clear();
  };

  try {
    if (!o.parts.empty()) {
      std::vector<char> buf(8 << 20);
      for (auto& part : o.parts) {
        std::ifstream in(part, std::ios::binary);
        if (!in) throw std::runtime_error("cannot read " + part);
        while (in) {
          in.read(buf.data(), std::streamsize(buf.size()));
          const auto got = std::size_t(in.gcount());
          st.bytes_in += got;
          tar.feed({buf.data(), got});
        }
      }
      st.release = "local:" + fs::path(o.parts.front()).filename().string();
    } else {
      // The release for the day: prod first, then staging (not the MLAT-only one).
      const std::string year = day.substr(0, 4), tagday = day.substr(0, 4) + "." + day.substr(5, 2) + "." + day.substr(8, 2);
      const std::string repo = "https://api.github.com/repos/adsblol/globe_history_" + year + "/releases/tags/v" + tagday + "-planes-readsb-";
      net::run_sync_void([&](net::asio::io_context& io) -> net::asio::awaitable<void> {
        net::Http http(io);
        std::vector<std::string> urls;
        for (const char* pod : {"prod-0", "staging-0"}) {
          auto r = co_await http.fetch({.url = repo + pod, .headers = {"Accept: application/vnd.github+json"}});
          if (r.status == 404) continue;
          if (!r.ok()) throw std::runtime_error("GitHub release lookup failed: HTTP " + std::to_string(r.status) + " " + r.error);
          auto rel = parse_json(r.body);
          if (const json::value* as = jget(rel, "assets"); as && as->is_array())
            for (auto& a : as->as_array())
              if (jstr(a, "name").find(".tar") != std::string::npos) urls.push_back(jstr(a, "browser_download_url"));
          std::sort(urls.begin(), urls.end());
          st.release = jstr(rel, "tag_name");
          if (!urls.empty()) break;
        }
        if (urls.empty()) throw std::runtime_error("no ADSB.lol release for " + day);
        for (auto& u : urls) {
          std::cerr << "downloading " << u << std::endl;
          std::uint64_t got = 0, next_report = 256ull << 20;
          auto r = co_await http.fetch({.url = u,
                                        .on_data =
                                            [&](std::string_view b) {
                                              tar.feed(b);
                                              got += b.size();
                                              if (got >= next_report) {
                                                std::cerr << "  " << (got >> 20) << " MiB, " << kept.load() << " aircraft kept" << std::endl;
                                                next_report += 256ull << 20;
                                              }
                                              return true;
                                            },
                                        .stall_timeout_s = 300,
                                        .decompress = false});
          if (!r.ok()) throw std::runtime_error("download failed: " + u + ": HTTP " + std::to_string(r.status) + " " + r.error);
          st.bytes_in += got;
        }
      });
    }
  } catch (...) {
    finish_workers();
    spill.close();
    fs::remove(spill_path);
    throw;
  }
  finish_workers();
  spill.close();
  if (err) {
    fs::remove(spill_path);
    std::rethrow_exception(err);
  }
  st.members = members, st.kept = kept, st.jets = jets, st.ground_only = ground, st.unknown_type = unknown;

  // Regional pressure offsets from the kept aircraft that report both altitudes, applied
  // to those that report barometric altitude alone. Then the final file, sorted by start.
  region.finish();
  MappedFile sp(spill_path);
  auto blob = [&](const IndexEntry& e) { return sp.view().substr(e.offset, e.clen); };
  std::sort(idx.begin(), idx.end(), [](const IndexEntry& a, const IndexEntry& b) { return a.t0 < b.t0; });
  // Streamed out: header and a placeholder index, the blobs, then the real index.
  const fs::path out = day_path(day), tmp_out = out.string() + ".tmp";
  std::ofstream of(tmp_out, std::ios::binary | std::ios::trunc);
  std::string head(MAGIC, 8);
  put(head, std::uint32_t(idx.size()));
  head.resize(head.size() + idx.size() * sizeof(IndexEntry), '\0');
  of.write(head.data(), std::streamsize(head.size()));
  std::uint64_t body = 0;
  std::vector<IndexEntry> out_idx;
  for (auto& e : idx) {
    auto fx = fixes_of(zstd_decompress(blob(e)), e.nfix, e.t0);
    st.fixes += fx.size();
    bool changed = false;
    for (auto& f : fx) {
      if (f.alt_src == AltSource::BaroRaw)
        if (auto off = region.at(f.lat, f.lon, f.t)) f.alt_m += *off * float(FT), f.alt_src = AltSource::BaroRegion, changed = true;
      st.baro_track += f.alt_src == AltSource::BaroTrack;
      st.baro_region += f.alt_src == AltSource::BaroRegion;
      st.baro_raw += f.alt_src == AltSource::BaroRaw;
    }
    IndexEntry ne = e;
    ne.offset = body;
    std::string c;
    if (changed) {
      Track tr = track_of(e);
      tr.fixes = std::move(fx);
      c = zstd_compress(fixes_blob(tr), 3);
    } else {
      c = std::string(blob(e));
    }
    ne.clen = std::uint32_t(c.size());
    of.write(c.data(), std::streamsize(c.size()));
    body += c.size();
    out_idx.push_back(ne);
  }
  of.seekp(12);
  of.write(reinterpret_cast<const char*>(out_idx.data()), std::streamsize(out_idx.size() * sizeof(IndexEntry)));
  of.close();
  if (!of) throw std::runtime_error("cannot write " + tmp_out.string());
  fs::rename(tmp_out, out);
  day_coverage(day, true);
  atomic_write(adsb_dir() / (day + ".used"), "");
  // A bounded store (a server's volume): keep the GEOLOC_ADSB_KEEP_DAYS days used last.
  if (const int keep = std::atoi(env("GEOLOC_ADSB_KEEP_DAYS").c_str()); keep > 0) {
    auto used = [](const std::string& d) {
      std::error_code ec;
      auto t = fs::last_write_time(adsb_dir() / (d + ".used"), ec);
      return ec ? fs::last_write_time(day_path(d)) : t;
    };
    auto days = stored_days();
    std::sort(days.begin(), days.end(), [&](auto& a, auto& b) { return used(a) > used(b); });
    for (std::size_t i = std::size_t(keep); i < days.size(); ++i)
      for (const char* ext : {".trk", ".cov", ".used"}) fs::remove(adsb_dir() / (days[i] + ext));
  }
  st.bytes_out = head.size() + body;
  sp = MappedFile();
  fs::remove(spill_path);
  return st;
}

std::vector<Track> load_tracks(double t0, double t1, const std::optional<Bbox>& bbox) {
  std::vector<Track> out;
  // A day's archive holds that UTC day's traces; a window near midnight needs both.
  for (i64 d = floor_div(i64(std::floor(t0)), 86400) - 1; d <= floor_div(i64(std::floor(t1)), 86400); ++d) {
    const std::string day = iso_date(d * 86400);
    if (!has_day(day)) continue;
    DayFile df(day_path(day));
    {
      std::error_code ec;  // last use, for a bounded store's eviction
      fs::last_write_time(adsb_dir() / (day + ".used"), fs::file_time_type::clock::now(), ec);
    }
    for (auto& e : df.idx) {
      if (e.t1 < t0 - 300 || e.t0 > t1 + 300) continue;
      if (bbox && (e.east < bbox->west || e.west > bbox->east || e.north < bbox->south || e.south > bbox->north)) continue;
      Track tr = track_of(e);
      if (tr.type.empty())  // stored before the registry knew it
        if (auto t = AircraftDb::get().type(tr.icao); !t.empty())
          if (auto ti = TypeDb::get().find(t)) tr.type = t, tr.info = *ti;
      auto fx = df.fixes(e);
      // Fixes in the window plus one either side, inside the box.
      std::size_t a = std::size_t(std::lower_bound(fx.begin(), fx.end(), t0, [](const Fix& f, double t) { return f.t < t; }) - fx.begin());
      std::size_t b = std::size_t(std::upper_bound(fx.begin(), fx.end(), t1, [](double t, const Fix& f) { return t < f.t; }) - fx.begin());
      if (a > 0) --a;
      if (b < fx.size()) ++b;
      for (std::size_t i = a; i < b; ++i)
        if (!bbox || bbox->contains(fx[i].lat, fx[i].lon)) tr.fixes.push_back(fx[i]);
      if (!tr.fixes.empty()) out.push_back(std::move(tr));
    }
  }
  return out;
}

std::vector<Track> split_tracks(const std::vector<Track>& tracks, double max_gap_s) {
  std::vector<Track> out;
  for (auto& tr : tracks) {
    Track cur = tr;
    cur.fixes.clear();
    for (auto& f : tr.fixes) {
      if (!cur.fixes.empty() && (f.leg_start || f.t - cur.fixes.back().t > max_gap_s)) {
        if (cur.fixes.size() >= 2) out.push_back(cur);
        cur.fixes.clear();
      }
      if (cur.fixes.empty() || f.t > cur.fixes.back().t) cur.fixes.push_back(f);
    }
    if (cur.fixes.size() >= 2) out.push_back(std::move(cur));
  }
  return out;
}

}  // namespace geo::air
