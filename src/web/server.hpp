// Web dashboard: upload a file, see where (which grid) and when it was recorded.
//
// One thread, one event loop (Boost.Asio + Beast). Uploads stream straight to a
// temporary file through an incremental multipart parser. Analyses and reference
// refreshes run as child `geoloc` processes, so the loop never blocks, a heavy job
// gives its memory back when it ends, and a crash only fails that request.
#pragma once

#include <functional>
#include <string>
#include <string_view>

namespace geo::web {

void serve(const std::string& host, int port);

// Streaming multipart/form-data parser (exposed for tests).
class Multipart {
 public:
  struct Handler {
    // begin(name, filename) -> true to receive the part's data
    std::function<bool(const std::string& name, const std::string& filename)> begin;
    std::function<void(std::string_view)> data;
    std::function<void()> end;
  };
  Multipart(std::string_view boundary, Handler h);
  void feed(std::string_view chunk);  // throws std::runtime_error on malformed input
  bool done() const { return state_ == State::Done; }

 private:
  enum class State { Preamble, Headers, Data, AfterDelim, Done };
  void run();
  std::string delim_, buf_;
  Handler h_;
  State state_ = State::Preamble;
  bool want_ = false;
};

std::string boundary_of(std::string_view content_type);

}  // namespace geo::web
