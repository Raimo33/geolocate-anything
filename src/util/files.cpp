#include "util/files.hpp"

#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <stdexcept>

namespace geo {

std::optional<std::string> read_file(const fs::path& p) {
  int fd = ::open(p.c_str(), O_RDONLY | O_CLOEXEC);
  if (fd < 0) return std::nullopt;
  struct stat st{};
  std::string out;
  if (::fstat(fd, &st) == 0 && st.st_size > 0) out.reserve(static_cast<std::size_t>(st.st_size));
  char buf[1 << 16];
  for (;;) {
    ssize_t n = ::read(fd, buf, sizeof buf);
    if (n < 0) {
      ::close(fd);
      return std::nullopt;
    }
    if (n == 0) break;
    out.append(buf, static_cast<std::size_t>(n));
  }
  ::close(fd);
  return out;
}

static bool write_all(int fd, const void* data, std::size_t n) {
  auto* c = static_cast<const char*>(data);
  while (n) {
    ssize_t w = ::write(fd, c, n);
    if (w < 0 && errno == EINTR) continue;
    if (w < 0) return false;
    c += w;
    n -= static_cast<std::size_t>(w);
  }
  return true;
}

void atomic_write(const fs::path& p, std::span<const std::span<const std::byte>> parts) {
  if (!p.parent_path().empty()) fs::create_directories(p.parent_path());
  // Unique per process: two processes writing the same file (a refresh and an import)
  // must not truncate each other's half-written copy. The last rename wins whole.
  fs::path tmp = p;
  tmp += "." + std::to_string(::getpid()) + ".tmp";
  int fd = ::open(tmp.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
  if (fd < 0) throw std::runtime_error("cannot write " + tmp.string());
  bool ok = true;
  for (auto part : parts) ok = ok && write_all(fd, part.data(), part.size());
  ok = ::close(fd) == 0 && ok;
  if (!ok) {  // e.g. disk full: never put a short file in place
    std::error_code ec;
    fs::remove(tmp, ec);
    throw std::runtime_error("write failed: " + tmp.string());
  }
  fs::rename(tmp, p);
}

void atomic_write(const fs::path& p, std::string_view data) {
  std::span<const std::byte> one{reinterpret_cast<const std::byte*>(data.data()), data.size()};
  atomic_write(p, std::span<const std::span<const std::byte>>(&one, 1));
}

std::string env(const char* name, std::string_view fallback) {
  const char* v = std::getenv(name);
  return v && *v ? std::string(v) : std::string(fallback);
}

MappedFile::MappedFile(const fs::path& p) {
  int fd = ::open(p.c_str(), O_RDONLY | O_CLOEXEC);
  if (fd < 0) return;
  struct stat st{};
  if (::fstat(fd, &st) != 0) {
    ::close(fd);
    return;
  }
  size_ = static_cast<std::size_t>(st.st_size);
  if (size_) {
    void* m = ::mmap(nullptr, size_, PROT_READ, MAP_PRIVATE, fd, 0);
    if (m == MAP_FAILED) {
      ::close(fd);
      size_ = 0;
      return;
    }
    data_ = static_cast<const std::byte*>(m);
  }
  ::close(fd);
  ok_ = true;
}

MappedFile::~MappedFile() {
  if (data_) ::munmap(const_cast<std::byte*>(data_), size_);
}

TempFile::TempFile(std::string_view suffix, const fs::path& dir) {
  fs::path base = dir.empty() ? fs::temp_directory_path() : dir;
  fs::create_directories(base);
  std::string tmpl = (base / "geoloc-XXXXXX").string();
  tmpl += suffix;
  int fd = ::mkstemps(tmpl.data(), static_cast<int>(suffix.size()));
  if (fd < 0) throw std::runtime_error("cannot create a temporary file in " + base.string());
  ::close(fd);
  path = tmpl;
}

TempFile::~TempFile() {
  if (!path.empty()) {
    std::error_code ec;
    fs::remove(path, ec);
  }
}

}  // namespace geo
