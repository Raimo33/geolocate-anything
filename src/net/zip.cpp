#include "net/zip.hpp"

#include <archive.h>
#include <archive_entry.h>
#include <zlib.h>

#include <algorithm>
#include <cstring>
#include <map>
#include <stdexcept>

namespace geo::zip {

namespace {

template <class T>
T rd(const std::byte* p) {
  T v;
  std::memcpy(&v, p, sizeof v);
  return v;
}

constexpr std::uint32_t SIG_EOCD = 0x06054b50, SIG_EOCD64 = 0x06064b50, SIG_LOC64 = 0x07064b50, SIG_CEN = 0x02014b50,
                        SIG_LOCAL = 0x04034b50;

struct Eocd {
  std::uint64_t cd_offset = 0, cd_size = 0, n = 0;
};

// Find the end-of-central-directory record in `tail` (the last bytes of a file of
// total size `size`, starting at file offset `tail_off`).
Eocd find_eocd(std::span<const std::byte> tail, std::uint64_t tail_off) {
  if (tail.size() < 22) throw std::runtime_error("not a zip archive (too short)");
  for (std::size_t i = tail.size() - 22 + 1; i-- > 0;) {
    if (rd<std::uint32_t>(tail.data() + i) != SIG_EOCD) continue;
    Eocd e;
    e.n = rd<std::uint16_t>(tail.data() + i + 10);
    e.cd_size = rd<std::uint32_t>(tail.data() + i + 12);
    e.cd_offset = rd<std::uint32_t>(tail.data() + i + 16);
    if ((e.cd_offset == 0xFFFFFFFFu || e.n == 0xFFFF || e.cd_size == 0xFFFFFFFFu) && i >= 20 &&
        rd<std::uint32_t>(tail.data() + i - 20) == SIG_LOC64) {
      std::uint64_t z64 = rd<std::uint64_t>(tail.data() + i - 20 + 8);
      if (z64 < tail_off || z64 - tail_off + 56 > tail.size()) throw std::runtime_error("zip64 record outside the fetched tail");
      const std::byte* p = tail.data() + (z64 - tail_off);
      if (rd<std::uint32_t>(p) != SIG_EOCD64) throw std::runtime_error("bad zip64 end record");
      e.n = rd<std::uint64_t>(p + 32);
      e.cd_size = rd<std::uint64_t>(p + 40);
      e.cd_offset = rd<std::uint64_t>(p + 48);
    }
    return e;
  }
  throw std::runtime_error("not a zip archive (no end of central directory)");
}

std::vector<Entry> parse_cd(std::span<const std::byte> cd, std::uint64_t n) {
  std::vector<Entry> out;
  std::size_t p = 0;
  for (std::uint64_t k = 0; k < n; ++k) {
    if (p + 46 > cd.size() || rd<std::uint32_t>(cd.data() + p) != SIG_CEN) throw std::runtime_error("corrupt zip central directory");
    const std::byte* h = cd.data() + p;
    Entry e;
    e.method = rd<std::uint16_t>(h + 10);
    e.crc = rd<std::uint32_t>(h + 16);
    e.comp_size = rd<std::uint32_t>(h + 20);
    e.size = rd<std::uint32_t>(h + 24);
    const std::uint16_t nlen = rd<std::uint16_t>(h + 28), xlen = rd<std::uint16_t>(h + 30), clen = rd<std::uint16_t>(h + 32);
    e.local_offset = rd<std::uint32_t>(h + 42);
    if (p + 46 + nlen + xlen > cd.size()) throw std::runtime_error("corrupt zip central directory");
    e.name.assign(reinterpret_cast<const char*>(h + 46), nlen);
    // ZIP64 extra field: replaces the fields that are 0xFFFFFFFF, in this order.
    const std::byte* x = h + 46 + nlen;
    for (std::size_t q = 0; q + 4 <= xlen;) {
      std::uint16_t id = rd<std::uint16_t>(x + q), len = rd<std::uint16_t>(x + q + 2);
      if (id == 1) {
        std::size_t r = q + 4;
        if (e.size == 0xFFFFFFFFu && r + 8 <= q + 4 + len) e.size = rd<std::uint64_t>(x + r), r += 8;
        if (e.comp_size == 0xFFFFFFFFu && r + 8 <= q + 4 + len) e.comp_size = rd<std::uint64_t>(x + r), r += 8;
        if (e.local_offset == 0xFFFFFFFFu && r + 8 <= q + 4 + len) e.local_offset = rd<std::uint64_t>(x + r);
      }
      q += 4 + len;
    }
    out.push_back(std::move(e));
    p += 46 + nlen + xlen + clen;
  }
  return out;
}

std::size_t data_start(std::span<const std::byte> data) {
  if (data.size() < 30 || rd<std::uint32_t>(data.data()) != SIG_LOCAL) throw std::runtime_error("bad zip local header");
  return 30 + rd<std::uint16_t>(data.data() + 26) + rd<std::uint16_t>(data.data() + 28);
}

}  // namespace

bool is_zip(std::string_view h) { return h.size() >= 2 && h[0] == 'P' && h[1] == 'K'; }
bool is_7z(std::string_view h) { return h.size() >= 6 && std::memcmp(h.data(), "7z\xbc\xaf\x27\x1c", 6) == 0; }

std::vector<Entry> entries(std::span<const std::byte> a) {
  auto tail = a.subspan(a.size() > 65557 + 20 ? a.size() - 65557 - 20 : 0);
  Eocd e = find_eocd(tail, a.size() - tail.size());
  if (e.cd_offset + e.cd_size > a.size()) throw std::runtime_error("zip central directory out of range");
  return parse_cd(a.subspan(e.cd_offset, e.cd_size), e.n);
}

void read(std::span<const std::byte> data, const Entry& e, const Sink& sink) {
  const std::size_t start = data_start(data);
  if (start + e.comp_size > data.size()) throw std::runtime_error("truncated zip member " + e.name);
  auto comp = data.subspan(start, e.comp_size);
  std::uint64_t out_n = 0;
  uLong crc = crc32(0L, Z_NULL, 0);
  auto emit = [&](const char* p, std::size_t n) {
    for (std::size_t o = 0; o < n;) {  // crc32 takes 32-bit lengths
      const auto k = static_cast<uInt>(std::min<std::size_t>(n - o, 1u << 30));
      crc = crc32(crc, reinterpret_cast<const Bytef*>(p + o), k);
      o += k;
    }
    out_n += n;
    sink(std::string_view(p, n));
  };
  auto check = [&] {
    if (out_n != e.size) throw std::runtime_error("zip member " + e.name + " is " + std::to_string(out_n) + " bytes, not " + std::to_string(e.size));
    if (crc != e.crc) throw std::runtime_error("zip member " + e.name + " fails its CRC check");
  };
  if (e.method == 0) {
    constexpr std::size_t CH = 1 << 20;
    for (std::size_t o = 0; o < comp.size(); o += CH) emit(reinterpret_cast<const char*>(comp.data()) + o, std::min(CH, comp.size() - o));
    check();
    return;
  }
  if (e.method != 8) throw std::runtime_error("unsupported zip compression method " + std::to_string(e.method) + " in " + e.name);
  z_stream zs{};
  if (inflateInit2(&zs, -MAX_WBITS) != Z_OK) throw std::runtime_error("zlib init failed");
  std::string buf(1 << 18, '\0');
  zs.next_in = reinterpret_cast<Bytef*>(const_cast<std::byte*>(comp.data()));
  int rc = Z_OK;
  // avail_in is 32-bit: feed huge members in slices.
  std::size_t fed = 0;
  do {
    if (zs.avail_in == 0 && fed < comp.size()) {
      std::size_t n = std::min<std::size_t>(comp.size() - fed, 1u << 30);
      zs.next_in = reinterpret_cast<Bytef*>(const_cast<std::byte*>(comp.data() + fed));
      zs.avail_in = static_cast<uInt>(n);
      fed += n;
    }
    zs.next_out = reinterpret_cast<Bytef*>(buf.data());
    zs.avail_out = static_cast<uInt>(buf.size());
    rc = inflate(&zs, Z_NO_FLUSH);
    if (rc != Z_OK && rc != Z_STREAM_END) {
      inflateEnd(&zs);
      throw std::runtime_error("corrupt deflate data in " + e.name);
    }
    std::size_t got = buf.size() - zs.avail_out;
    if (got) emit(buf.data(), got);
    if (rc == Z_OK && zs.avail_in == 0 && fed >= comp.size() && got == 0) break;  // input used up before the stream's end
  } while (rc != Z_STREAM_END);
  inflateEnd(&zs);
  if (rc != Z_STREAM_END) throw std::runtime_error("zip member " + e.name + " is cut short (deflate stream not finished)");
  check();
}

struct Gunzip::Impl {
  z_stream zs{};
  Sink sink;
  bool ended = false;
  std::string buf = std::string(1 << 18, '\0');
};

Gunzip::Gunzip(Sink sink) : impl_(std::make_unique<Impl>()) {
  impl_->sink = std::move(sink);
  if (inflateInit2(&impl_->zs, 16 + MAX_WBITS) != Z_OK) throw std::runtime_error("zlib init failed");
}
Gunzip::~Gunzip() { inflateEnd(&impl_->zs); }

void Gunzip::feed(std::string_view in) {
  auto& z = *impl_;
  while (!in.empty()) {
    if (z.ended) {  // concatenated members (gzip allows them)
      if (inflateReset(&z.zs) != Z_OK) throw std::runtime_error("zlib reset failed");
      z.ended = false;
    }
    const std::size_t n = std::min<std::size_t>(in.size(), 1u << 30);
    z.zs.next_in = reinterpret_cast<Bytef*>(const_cast<char*>(in.data()));
    z.zs.avail_in = static_cast<uInt>(n);
    do {
      z.zs.next_out = reinterpret_cast<Bytef*>(z.buf.data());
      z.zs.avail_out = static_cast<uInt>(z.buf.size());
      int rc = inflate(&z.zs, Z_NO_FLUSH);
      if (rc != Z_OK && rc != Z_STREAM_END && rc != Z_BUF_ERROR) throw std::runtime_error("corrupt gzip data");
      if (std::size_t got = z.buf.size() - z.zs.avail_out) z.sink(std::string_view(z.buf.data(), got));
      if (rc == Z_STREAM_END) {
        z.ended = true;
        break;
      }
    } while (z.zs.avail_in > 0 || z.zs.avail_out == 0);
    const std::size_t used = n - z.zs.avail_in;
    in.remove_prefix(used);
    if (used == 0 && !z.ended) break;
  }
}

void Gunzip::finish() {
  if (!impl_->ended) throw std::runtime_error("gzip stream ended early");
}

std::string read_all(std::span<const std::byte> data, const Entry& e) {
  std::string out;
  out.reserve(static_cast<std::size_t>(e.size));
  read(data, e, [&](std::string_view c) { out.append(c); });
  return out;
}

const Entry* Archive::find_if(const std::function<bool(const Entry&)>& f) const {
  for (auto& e : entries_)
    if (f(e)) return &e;
  return nullptr;
}

// ---------------------------------------------------------------- remote

asio::awaitable<std::string> Remote::range(std::uint64_t a, std::uint64_t b) {
  std::string last;
  for (int attempt = 0; attempt < 4; ++attempt) {
    net::Request rq;
    rq.url = resolved_.empty() ? url_ : resolved_;
    rq.range = std::pair{a, b - 1};
    rq.decompress = false;
    auto r = co_await http_.fetch(std::move(rq));
    if (r.status == 206 && r.body.size() == b - a) co_return std::move(r.body);
    last = r.error.empty() ? "HTTP " + std::to_string(r.status) : r.error;
    // Network hiccup or an expired signed URL: resolve again and retry.
    co_await net::sleep_for(double(2 << attempt));
    try {
      co_await open();
    } catch (...) {
    }
  }
  throw std::runtime_error("range read failed: " + url_ + ": " + last);
}

asio::awaitable<void> Remote::open() {
  net::Request rq;
  rq.url = url_;
  rq.range = std::pair<std::uint64_t, std::uint64_t>{0, 0};
  rq.decompress = false;
  auto r = co_await http_.fetch(std::move(rq));
  auto cr = r.header("content-range");
  if (r.status != 206 || cr.empty()) throw std::runtime_error(url_ + ": server does not support range requests");
  resolved_ = r.effective_url;
  size_ = std::stoull(cr.substr(cr.find('/') + 1));
  if (!entries_.empty()) co_return;
  const std::uint64_t tail_len = std::min<std::uint64_t>(size_, 65557 + 20 + 4096);
  std::string tail = co_await range(size_ - tail_len, size_);
  auto ts = std::as_bytes(std::span(tail.data(), tail.size()));
  Eocd e = find_eocd(ts, size_ - tail_len);
  std::string cd;
  if (e.cd_offset >= size_ - tail_len && e.cd_offset + e.cd_size <= size_)
    cd = tail.substr(e.cd_offset - (size_ - tail_len), e.cd_size);
  else
    cd = co_await range(e.cd_offset, e.cd_offset + e.cd_size);
  entries_ = parse_cd(std::as_bytes(std::span(cd.data(), cd.size())), e.n);
}

asio::awaitable<std::pair<std::uint64_t, std::uint64_t>> Remote::stored_span(const Entry& e) {
  if (e.method != 0) throw std::runtime_error(e.name + ": member is compressed, cannot be read in parts");
  std::string h = co_await range(e.local_offset, std::min(size_, e.local_offset + 30));
  auto hb = std::as_bytes(std::span(h.data(), h.size()));
  if (hb.size() < 30 || rd<std::uint32_t>(hb.data()) != SIG_LOCAL) throw std::runtime_error("bad zip local header: " + e.name);
  std::uint64_t a = e.local_offset + 30 + rd<std::uint16_t>(hb.data() + 26) + rd<std::uint16_t>(hb.data() + 28);
  co_return std::pair{a, a + e.size};
}

asio::awaitable<void> Remote::for_each(std::vector<Entry> wanted, const std::function<void(const Entry&, std::span<const std::byte>)>& fn,
                                       std::uint64_t max_bytes, int parallel) {
  std::sort(wanted.begin(), wanted.end(), [](auto& a, auto& b) { return a.local_offset < b.local_offset; });
  // Each member's bytes end where the next member (in file order) begins.
  std::vector<std::uint64_t> offsets;
  for (auto& e : entries_) offsets.push_back(e.local_offset);
  std::sort(offsets.begin(), offsets.end());
  auto end_of = [&](const Entry& e) {
    auto it = std::upper_bound(offsets.begin(), offsets.end(), e.local_offset);
    return std::min(size_, it == offsets.end() ? size_ : *it);
  };
  struct Group {
    std::uint64_t a, b;
    std::vector<Entry> members;
  };
  std::vector<Group> groups;
  for (auto& e : wanted) {
    std::uint64_t a = e.local_offset, b = end_of(e);
    if (!groups.empty() && a >= groups.back().b && a - groups.back().b < (1 << 20) && b - groups.back().a <= max_bytes) {
      groups.back().b = b;
      groups.back().members.push_back(e);
    } else {
      groups.push_back({a, b, {e}});
    }
  }
  // Download up to `parallel` groups ahead, process them in order.
  std::map<std::size_t, std::string> ready;
  std::map<std::size_t, std::shared_ptr<net::AsyncEvent>> done;
  auto ex = co_await asio::this_coro::executor;
  std::size_t next_start = 0;
  std::exception_ptr failure;
  auto launch = [&](std::size_t gi) {
    auto ev = std::make_shared<net::AsyncEvent>(ex);
    done[gi] = ev;
    asio::co_spawn(
        ex,
        [this, gi, &groups, &ready]() -> asio::awaitable<void> { ready[gi] = co_await range(groups[gi].a, groups[gi].b); },
        [ev, &failure](std::exception_ptr e) {
          if (e && !failure) failure = e;
          ev->set();
        });
  };
  std::exception_ptr err;
  for (std::size_t gi = 0; gi < groups.size() && !err; ++gi) {
    while (next_start < groups.size() && next_start < gi + std::size_t(parallel)) launch(next_start++);
    co_await done[gi]->wait();
    if (failure) {
      err = failure;
      break;
    }
    std::string bytes = std::move(ready[gi]);
    ready.erase(gi);
    done.erase(gi);
    auto span = std::as_bytes(std::span(bytes.data(), bytes.size()));
    try {
      for (auto& m : groups[gi].members) fn(m, span.subspan(m.local_offset - groups[gi].a));
    } catch (...) {
      err = std::current_exception();
    }
  }
  // Downloads still running reference these locals: let them finish first.
  for (auto& [gi, ev] : done) co_await ev->wait();
  if (err) std::rethrow_exception(err);
}

// ---------------------------------------------------------------- libarchive

int for_each_libarchive(const std::string& path, const MemberSink& sink) {
  archive* a = archive_read_new();
  archive_read_support_format_all(a);
  archive_read_support_filter_all(a);
  if (archive_read_open_filename(a, path.c_str(), 1 << 20) != ARCHIVE_OK) {
    std::string err = archive_error_string(a) ? archive_error_string(a) : "unknown error";
    archive_read_free(a);
    throw std::runtime_error("cannot open archive " + path + ": " + err);
  }
  int n = 0;
  archive_entry* entry;
  int hr;
  while ((hr = archive_read_next_header(a, &entry)) == ARCHIVE_OK || hr == ARCHIVE_WARN) {
    if (archive_entry_filetype(entry) != AE_IFREG) continue;
    std::string name = archive_entry_pathname(entry) ? archive_entry_pathname(entry) : "";
    if (!sink.begin(name)) {
      archive_read_data_skip(a);
      continue;
    }
    const void* buf;
    std::size_t size;
    la_int64_t off;
    int rc;
    while ((rc = archive_read_data_block(a, &buf, &size, &off)) == ARCHIVE_OK) sink.data(std::string_view(static_cast<const char*>(buf), size));
    if (rc != ARCHIVE_EOF) {
      std::string err = archive_error_string(a) ? archive_error_string(a) : "read error";
      archive_read_free(a);
      throw std::runtime_error("corrupt archive " + path + ": " + err);
    }
    sink.end();
    ++n;
  }
  if (hr != ARCHIVE_EOF) {  // a cut-off archive must not pass for a complete one
    std::string err = archive_error_string(a) ? archive_error_string(a) : "read error";
    archive_read_free(a);
    throw std::runtime_error("corrupt archive " + path + ": " + err);
  }
  archive_read_free(a);
  return n;
}

}  // namespace geo::zip
