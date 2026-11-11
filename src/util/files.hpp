// Small file-system helpers: whole-file reads, atomic replacement, read-only mmap.
#pragma once

#include <cstddef>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>

namespace geo {

namespace fs = std::filesystem;

std::optional<std::string> read_file(const fs::path& p);
// Writes to "<p>.tmp" then renames: readers see the old or the new file, never half.
void atomic_write(const fs::path& p, std::string_view data);
void atomic_write(const fs::path& p, std::span<const std::span<const std::byte>> parts);
std::string env(const char* name, std::string_view fallback = {});

// Read-only memory map of a whole file (empty files map to an empty span).
class MappedFile {
 public:
  MappedFile() = default;
  explicit MappedFile(const fs::path& p);
  MappedFile(MappedFile&& o) noexcept { swap(o); }
  MappedFile& operator=(MappedFile&& o) noexcept {
    swap(o);
    return *this;
  }
  MappedFile(const MappedFile&) = delete;
  MappedFile& operator=(const MappedFile&) = delete;
  ~MappedFile();
  std::span<const std::byte> bytes() const { return {data_, size_}; }
  std::string_view view() const { return {reinterpret_cast<const char*>(data_), size_}; }
  bool ok() const { return ok_; }

 private:
  void swap(MappedFile& o) noexcept {
    std::swap(data_, o.data_);
    std::swap(size_, o.size_);
    std::swap(ok_, o.ok_);
  }
  const std::byte* data_ = nullptr;
  std::size_t size_ = 0;
  bool ok_ = false;
};

// A temporary file removed on destruction.
struct TempFile {
  fs::path path;
  explicit TempFile(std::string_view suffix = {}, const fs::path& dir = {});
  TempFile(TempFile&& o) noexcept : path(std::move(o.path)) { o.path.clear(); }
  TempFile(const TempFile&) = delete;
  ~TempFile();
};

}  // namespace geo
