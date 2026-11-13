// Asynchronous HTTP(S) on the Asio event loop: libcurl's multi interface drives every
// transfer from socket readiness and timer events (no threads, no blocking calls), and
// each request is awaited as a C++20 coroutine:
//
//     net::Http http(io);
//     auto r = co_await http.fetch({.url = "https://..."});
//
// Bodies can be streamed to a callback as they arrive (parse while downloading) or
// collected; uploads stream from a file. S3 requests are signed by curl (AWS SigV4).
#pragma once

#include <boost/asio.hpp>
#include <algorithm>
#include <cstdint>
#include <deque>
#include <exception>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace geo::net {

namespace asio = boost::asio;

inline constexpr const char* USER_AGENT = "geolocate-anything/0.2 (+https://github.com/raimo33/geolocate-anything)";

struct Request {
  std::string url;
  std::string method = "GET";
  std::vector<std::string> headers;
  std::optional<std::pair<std::uint64_t, std::uint64_t>> range;  // inclusive byte range (streamed only from a 206)
  // Streaming sink; return false to abort the transfer. Without it the body is collected.
  std::function<bool(std::string_view)> on_data;
  std::string body;         // request body (PUT/POST), small
  std::string upload_file;  // or stream the body from this file (PUT)
  std::string aws_sigv4;    // e.g. "aws:amz:auto:s3"; credentials in userpwd
  std::string userpwd;
  long connect_timeout_s = 30;
  long stall_timeout_s = 120;  // abort when under 1 byte/s for this long
  long total_timeout_s = 0;    // 0 = none (big downloads take as long as they take)
  bool decompress = true;      // ask for gzip/deflate and decode (not with ranges)
};

struct Response {
  long status = 0;
  std::string body;
  std::map<std::string, std::string> headers;  // lower-case names, final response only
  std::string effective_url;
  std::string error;  // transport error (empty when an HTTP status was received)
  bool ok() const { return error.empty() && status >= 200 && status < 300; }
  std::string header(const std::string& name) const {
    auto it = headers.find(name);
    return it == headers.end() ? std::string() : it->second;
  }
};

class Http {
 public:
  explicit Http(asio::io_context& io);
  ~Http();
  Http(const Http&) = delete;
  Http& operator=(const Http&) = delete;

  asio::awaitable<Response> fetch(Request req);
  // GET with retries (exponential backoff) on transport errors and 5xx; throws
  // std::runtime_error("download failed: ...") after the last attempt.
  asio::awaitable<Response> get(std::string url, int retries = 3, long total_timeout_s = 0);
  asio::io_context& io() { return io_; }

  struct Impl;

 private:
  asio::io_context& io_;
  std::unique_ptr<Impl> impl_;
};

asio::awaitable<void> sleep_for(double seconds);

// Asynchronous helpers for a single-threaded loop.
// A latch: wait() returns once set() was called. reset() re-arms it; only call it
// while nobody waits.
class AsyncEvent {
 public:
  explicit AsyncEvent(asio::any_io_executor ex) : timer_(ex, asio::steady_timer::time_point::max()) {}
  asio::awaitable<void> wait();
  void set();
  void reset();
  bool is_set() const { return set_; }

 private:
  asio::steady_timer timer_;
  bool set_ = false;
};

class AsyncSemaphore {
 public:
  AsyncSemaphore(asio::any_io_executor ex, int n) : ex_(ex), free_(n) {}
  asio::awaitable<void> acquire();
  void release();

 private:
  asio::any_io_executor ex_;
  int free_;
  std::vector<std::shared_ptr<AsyncEvent>> waiters_;
};

// Run `fn` (a coroutine) over items with at most `parallel` in flight. Every item is
// processed unless one throws: then the remaining queue is dropped and the first
// failure is rethrown once the in-flight items have finished.
template <class T, class F>
asio::awaitable<void> parallel_for(std::vector<T> items, int parallel, F fn) {
  auto ex = co_await asio::this_coro::executor;
  std::deque<T> q(std::make_move_iterator(items.begin()), std::make_move_iterator(items.end()));
  std::exception_ptr err;
  int running = std::min<int>(std::max(parallel, 1), int(q.size()));
  if (!running) co_return;
  AsyncEvent all_done(ex);
  auto worker = [&]() -> asio::awaitable<void> {
    while (!q.empty() && !err) {
      T it = std::move(q.front());
      q.pop_front();
      try {
        co_await fn(it);
      } catch (...) {
        if (!err) err = std::current_exception();
      }
    }
    if (--running == 0) all_done.set();
  };
  for (int i = 0, n = running; i < n; ++i) asio::co_spawn(ex, worker(), asio::detached);
  co_await all_done.wait();
  if (err) std::rethrow_exception(err);
}

// Asio is built without thread support (BOOST_ASIO_DISABLE_THREADS): its per-thread
// state is process-wide, so two event loops on two threads at once corrupt each other.
// run_sync / run_sync_void hold this lock (re-entrant: a loop may start a nested one on
// its own thread) so worker threads that need the network (DEM tiles during a search)
// take turns.
std::recursive_mutex& event_loop_mutex();

// Run one coroutine to completion on a fresh event loop (CLI entry points).
template <class T>
T run_sync(std::function<asio::awaitable<T>(asio::io_context&)> f) {
  std::lock_guard lk(event_loop_mutex());
  asio::io_context io;
  std::optional<T> out;
  std::exception_ptr err;
  asio::co_spawn(io, f(io), [&](std::exception_ptr e, T v) {
    if (e) err = e;
    else out.emplace(std::move(v));
  });
  io.run();
  if (err) std::rethrow_exception(err);
  return std::move(*out);
}
void run_sync_void(const std::function<asio::awaitable<void>(asio::io_context&)>& f);

}  // namespace geo::net
