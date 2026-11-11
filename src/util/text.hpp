// String helpers.
#pragma once

#include <charconv>
#include <cmath>
#include <format>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace geo {

std::string_view trim(std::string_view s);
std::string lower(std::string_view s);
bool iequals(std::string_view a, std::string_view b);
bool ends_with_ci(std::string_view s, std::string_view suffix);
std::vector<std::string_view> split(std::string_view s, char sep);
std::string join(const std::vector<std::string>& v, std::string_view sep);

// Fast number parsing (std::from_chars); tolerates surrounding spaces/quotes.
std::optional<double> to_double(std::string_view s);
std::optional<long long> to_int(std::string_view s);

// Python-like "%" formatting of a fraction with no decimals ("50%").
std::string pct0(double frac);
// f"{x:spec}" for the specs used in reports, "n/a" when not finite.
std::string fmt_num(double x, const char* printf_spec);

std::string url_encode(std::string_view s, bool keep_slash = false);

}  // namespace geo
