// ZIP archives (central directory + zlib inflate, ZIP64-aware) from memory or a memory
// map, members inflated in chunks to a callback (constant memory), plus remote ZIPs
// read with HTTP range requests (only the members needed are downloaded, coalesced
// into large requests). 7z archives go through libarchive, streaming member by member.
#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "net/http.hpp"

namespace geo::zip {

namespace asio = boost::asio;

struct Entry {
  std::string name;
  std::uint64_t comp_size = 0, size = 0, local_offset = 0;
  std::uint16_t method = 0;  // 0 stored, 8 deflate
  std::uint32_t crc = 0;     // CRC-32 of the uncompressed bytes (checked on read)
};

using Sink = std::function<void(std::string_view)>;

bool is_zip(std::string_view head);
bool is_7z(std::string_view head);

// A gzip stream inflated as its compressed bytes arrive (constant memory).
class Gunzip {
 public:
  explicit Gunzip(Sink sink);
  ~Gunzip();
  Gunzip(const Gunzip&) = delete;
  Gunzip& operator=(const Gunzip&) = delete;
  void feed(std::string_view compressed);
  void finish();  // throws when the stream ended early

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

// Central directory of a whole archive in memory.
std::vector<Entry> entries(std::span<const std::byte> archive);
// Inflate a member; `data` starts at the member's local header. Throws when the member is
// cut short or its size or CRC-32 does not match the central directory.
void read(std::span<const std::byte> data, const Entry& e, const Sink& sink);
std::string read_all(std::span<const std::byte> data, const Entry& e);

// Convenience over a whole in-memory archive.
class Archive {
 public:
  explicit Archive(std::span<const std::byte> bytes) : bytes_(bytes), entries_(zip::entries(bytes)) {}
  explicit Archive(std::string_view bytes) : Archive(std::as_bytes(std::span(bytes.data(), bytes.size()))) {}
  const std::vector<Entry>& entries() const { return entries_; }
  void read(const Entry& e, const Sink& sink) const { zip::read(bytes_.subspan(e.local_offset), e, sink); }
  std::string read_all(const Entry& e) const { return zip::read_all(bytes_.subspan(e.local_offset), e); }
  const Entry* find_if(const std::function<bool(const Entry&)>& f) const;

 private:
  std::span<const std::byte> bytes_;
  std::vector<Entry> entries_;
};

// Remote archive read with range requests.
class Remote {
 public:
  Remote(net::Http& http, std::string url) : http_(http), url_(std::move(url)) {}
  asio::awaitable<void> open();  // resolve redirects, read the central directory
  const std::vector<Entry>& entries() const { return entries_; }
  // Fetch the given members (coalesced into requests of up to max_bytes, `parallel` at
  // a time) and call fn(entry, bytes starting at its local header) for each, in order.
  asio::awaitable<void> for_each(std::vector<Entry> wanted, const std::function<void(const Entry&, std::span<const std::byte>)>& fn,
                                 std::uint64_t max_bytes = 32 << 20, int parallel = 2);
  // [begin, end) of a stored (uncompressed) member's bytes in the archive, for reading
  // parts of it with range requests on url().
  asio::awaitable<std::pair<std::uint64_t, std::uint64_t>> stored_span(const Entry& e);
  const std::string& url() const { return resolved_.empty() ? url_ : resolved_; }

 private:
  asio::awaitable<std::string> range(std::uint64_t a, std::uint64_t b);  // [a, b)
  net::Http& http_;
  std::string url_, resolved_;
  std::uint64_t size_ = 0;
  std::vector<Entry> entries_;
};

// Every regular member of a 7z (or any libarchive format) file, streamed in chunks:
// begin(name) / data(chunk) / end() per member.
struct MemberSink {
  std::function<bool(const std::string& name)> begin;  // false: skip this member
  std::function<void(std::string_view)> data;
  std::function<void()> end;
};
int for_each_libarchive(const std::string& path, const MemberSink& sink);

}  // namespace geo::zip
