// JSON (Boost.JSON) helpers. dump() writes the layout Python's json.dumps uses
// (indent, optional sorted keys, ASCII-only), so manifests stay byte-compatible with
// files already in the store and the bucket. Non-finite numbers become null.
#pragma once

#include <boost/json.hpp>
#include <cmath>
#include <optional>
#include <string>
#include <string_view>

namespace geo {

namespace json = boost::json;

struct DumpOptions {
  int indent = -1;  // < 0: compact (separators "," ":")
  bool sort_keys = false;
  bool ascii = true;
};
std::string dump(const json::value& v, DumpOptions o = {});

// Shortest round-trip repr of a double, Python style ("1.0", "1e-05", "0.30000000000000004").
std::string py_float(double x);

inline json::value num_or_null(double x) { return std::isfinite(x) ? json::value(x) : json::value(nullptr); }

json::value parse_json(std::string_view text);  // throws std::runtime_error with context

// Accessors that tolerate missing keys / wrong types.
const json::value* jget(const json::value& v, std::string_view key);
std::string jstr(const json::value& v, std::string_view key, std::string_view fallback = {});
double jnum(const json::value& v, std::string_view key, double fallback = NAN);
std::int64_t jint(const json::value& v, std::string_view key, std::int64_t fallback = 0);
bool jbool(const json::value& v, std::string_view key, bool fallback = false);
double as_number(const json::value& v, double fallback = NAN);

}  // namespace geo
