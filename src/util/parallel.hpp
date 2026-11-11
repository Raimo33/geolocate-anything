// Spreading independent work over the cores. GEOLOC_THREADS caps the threads (default:
// every core); results are written by index, so they never depend on the thread count.
#pragma once

#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <exception>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "util/files.hpp"

namespace geo {

// Threads for CPU work: `requested` if > 0, else $GEOLOC_THREADS, else every core.
inline int worker_threads(int requested = 0) {
  if (requested > 0) return requested;
  const std::string t = env("GEOLOC_THREADS");
  if (!t.empty()) return std::clamp(std::atoi(t.c_str()), 1, 256);
  return std::max(1, int(std::thread::hardware_concurrency()));
}

// fn(i) for i in [0, n), on up to `threads` threads (0: worker_threads()), each taking
// the next index not yet started. The first exception is rethrown once all have stopped.
template <class F>
void parallel_for(std::size_t n, F&& fn, int threads = 0) {
  const int t = std::max(1, std::min<int>(worker_threads(threads), int(std::min<std::size_t>(n, 256))));
  if (t <= 1) {
    for (std::size_t i = 0; i < n; ++i) fn(i);
    return;
  }
  std::atomic<std::size_t> next{0};
  std::exception_ptr err;
  std::mutex mu;
  auto work = [&] {
    for (std::size_t i; (i = next.fetch_add(1)) < n;) {
      try {
        fn(i);
      } catch (...) {
        std::lock_guard lk(mu);
        if (!err) err = std::current_exception();
        next = n;
      }
    }
  };
  {
    std::vector<std::jthread> pool;
    for (int k = 0; k < t; ++k) pool.emplace_back(work);
  }
  if (err) std::rethrow_exception(err);
}

}  // namespace geo
