// Child processes on the event loop: the server runs analyses and downloads as
// separate `geoloc` processes (CPU work stays off the loop, memory goes back to the OS
// when they exit, a crash or OOM kills only that job). stdout is read asynchronously.
#pragma once

#include <functional>
#include <string>
#include <string_view>
#include <vector>

#include "net/http.hpp"

namespace geo::net {

struct ChildResult {
  int exit_code = -1;
  std::string out;  // stdout (when no line callback was given)
};

std::string self_exe();
// Runs argv (argv[0] = program path); on_line receives each stdout line as it arrives.
// on_start receives the child's pid (to kill it early).
asio::awaitable<ChildResult> run_child(std::vector<std::string> argv, std::function<void(std::string_view)> on_line = {},
                                       std::function<void(int pid)> on_start = {});

}  // namespace geo::net
