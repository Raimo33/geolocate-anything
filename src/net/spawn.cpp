#include "net/spawn.hpp"

#include <fcntl.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>

#include <stdexcept>

extern char** environ;

namespace geo::net {

std::string self_exe() {
  char buf[4096];
  ssize_t n = ::readlink("/proc/self/exe", buf, sizeof buf - 1);
  if (n <= 0) throw std::runtime_error("cannot find the geoloc executable");
  return std::string(buf, static_cast<std::size_t>(n));
}

asio::awaitable<ChildResult> run_child(std::vector<std::string> argv, std::function<void(std::string_view)> on_line,
                                       std::function<void(int pid)> on_start) {
  int fds[2];
  if (::pipe2(fds, O_CLOEXEC) != 0) throw std::runtime_error("pipe failed");
  posix_spawn_file_actions_t fa;
  posix_spawn_file_actions_init(&fa);
  posix_spawn_file_actions_adddup2(&fa, fds[1], 1);  // dup2 clears CLOEXEC on fd 1
  std::vector<char*> args;
  for (auto& a : argv) args.push_back(a.data());
  args.push_back(nullptr);
  pid_t pid = 0;
  int rc = ::posix_spawn(&pid, argv[0].c_str(), &fa, nullptr, args.data(), environ);
  posix_spawn_file_actions_destroy(&fa);
  ::close(fds[1]);
  if (rc != 0) {
    ::close(fds[0]);
    throw std::runtime_error("cannot start " + argv[0]);
  }
  if (on_start) on_start(int(pid));
  auto ex = co_await asio::this_coro::executor;
  asio::posix::stream_descriptor rd(ex, fds[0]);
  ChildResult res;
  std::string pending;
  char buf[1 << 16];
  for (;;) {
    boost::system::error_code ec;
    std::size_t n = co_await rd.async_read_some(asio::buffer(buf), asio::redirect_error(asio::use_awaitable, ec));
    if (n) {
      if (!on_line) {
        res.out.append(buf, n);
      } else {
        pending.append(buf, n);
        std::size_t a = 0, nl;
        while ((nl = pending.find('\n', a)) != std::string::npos) {
          on_line(std::string_view(pending).substr(a, nl - a));
          a = nl + 1;
        }
        pending.erase(0, a);
      }
    }
    if (ec) break;  // EOF (or error): the child closed its stdout
  }
  if (on_line && !pending.empty()) on_line(pending);
  // Reap without blocking the loop.
  asio::steady_timer t(ex);
  for (int delay = 5;; delay = std::min(delay * 2, 500)) {
    int status = 0;
    pid_t w = ::waitpid(pid, &status, WNOHANG);
    if (w == pid) {
      res.exit_code = WIFEXITED(status) ? WEXITSTATUS(status) : 128 + WTERMSIG(status);
      break;
    }
    if (w < 0) break;
    t.expires_after(std::chrono::milliseconds(delay));
    co_await t.async_wait(asio::use_awaitable);
  }
  co_return res;
}

}  // namespace geo::net
