// Precompiled header (CMake target_precompile_headers, GEOLOC_PCH): the heavy headers
// almost every translation unit pulls in, parsed once per target instead of per file.
// Sources still include what they use; this only changes how fast they compile.
#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include <boost/asio.hpp>
#include <boost/json.hpp>
