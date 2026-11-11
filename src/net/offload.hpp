// CPU work off the event loop, in the same process: run_in_thread(fn) runs fn on a
// thread of its own and resumes the awaiting coroutine when it returns (or rethrows
// what it threw). The loop stays single-threaded (BOOST_ASIO_DISABLE_THREADS): the
// thread touches nothing of Asio and signals completion through a pipe the loop reads.
// fn must not touch loop state; what it captures by reference must outlive the await.
#pragma once

#include <fcntl.h>
#include <unistd.h>

#include <cerrno>
#include <exception>
#include <optional>
#include <system_error>
#include <thread>
#include <type_traits>

#include "net/http.hpp"

namespace geo::net {

template <class F>
asio::awaitable<std::invoke_result_t<F&>> run_in_thread(F fn) {
  using R = std::invoke_result_t<F&>;
  int fds[2];
  if (::pipe2(fds, O_CLOEXEC) != 0) throw std::system_error(errno, std::generic_category(), "pipe2");
  asio::posix::stream_descriptor rd(co_await asio::this_coro::executor, fds[0]);
  std::optional<R> out;
  std::exception_ptr err;
  std::thread th([&out, &err, &fn, wfd = fds[1]] {
    try {
      out.emplace(fn());
    } catch (...) {
      err = std::current_exception();
    }
    const char c = 1;
    while (::write(wfd, &c, 1) < 0 && errno == EINTR) {
    }
    ::close(wfd);
  });
  struct Join {  // the thread writes into this frame: never leave it running
    std::thread& t;
    ~Join() {
      if (t.joinable()) t.join();
    }
  } join{th};
  char c = 0;
  boost::system::error_code ec;
  co_await rd.async_read_some(asio::buffer(&c, 1), asio::redirect_error(asio::use_awaitable, ec));
  th.join();
  if (err) std::rethrow_exception(err);
  co_return std::move(*out);
}

}  // namespace geo::net
