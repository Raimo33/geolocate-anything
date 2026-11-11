#include "util/text.hpp"

#include <algorithm>
#include <cctype>
#include <cstdio>

namespace geo {

std::string_view trim(std::string_view s) {
  while (!s.empty() && (std::isspace(static_cast<unsigned char>(s.front())) || s.front() == '"')) s.remove_prefix(1);
  while (!s.empty() && (std::isspace(static_cast<unsigned char>(s.back())) || s.back() == '"')) s.remove_suffix(1);
  return s;
}

std::string lower(std::string_view s) {
  std::string o(s);
  for (auto& c : o) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return o;
}

bool iequals(std::string_view a, std::string_view b) {
  return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin(), [](char x, char y) {
           return std::tolower(static_cast<unsigned char>(x)) == std::tolower(static_cast<unsigned char>(y));
         });
}

bool ends_with_ci(std::string_view s, std::string_view suffix) {
  return s.size() >= suffix.size() && iequals(s.substr(s.size() - suffix.size()), suffix);
}

std::vector<std::string_view> split(std::string_view s, char sep) {
  std::vector<std::string_view> out;
  std::size_t a = 0;
  for (;;) {
    auto b = s.find(sep, a);
    if (b == std::string_view::npos) {
      out.push_back(s.substr(a));
      return out;
    }
    out.push_back(s.substr(a, b - a));
    a = b + 1;
  }
}

std::string join(const std::vector<std::string>& v, std::string_view sep) {
  std::string out;
  for (std::size_t i = 0; i < v.size(); ++i) {
    if (i) out += sep;
    out += v[i];
  }
  return out;
}

std::optional<double> to_double(std::string_view s) {
  s = trim(s);
  if (s.empty()) return std::nullopt;
  if (s.front() == '+') s.remove_prefix(1);
  double v;
  auto r = std::from_chars(s.data(), s.data() + s.size(), v);
  if (r.ec != std::errc() || r.ptr != s.data() + s.size()) {
    if (iequals(s, "nan")) return NAN;
    return std::nullopt;
  }
  return v;
}

std::optional<long long> to_int(std::string_view s) {
  s = trim(s);
  long long v;
  auto r = std::from_chars(s.data(), s.data() + s.size(), v);
  if (r.ec != std::errc() || r.ptr != s.data() + s.size()) return std::nullopt;
  return v;
}

std::string pct0(double frac) {
  char b[32];
  std::snprintf(b, sizeof b, "%.0f%%", frac * 100.0);
  return b;
}

std::string fmt_num(double x, const char* spec) {
  if (!std::isfinite(x)) return "n/a";
  char b[64];
  std::snprintf(b, sizeof b, spec, x);
  return b;
}

std::string url_encode(std::string_view s, bool keep_slash) {
  static const char* hex = "0123456789ABCDEF";
  std::string o;
  for (unsigned char c : s) {
    if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~' || (keep_slash && c == '/')) {
      o += static_cast<char>(c);
    } else {
      o += '%';
      o += hex[c >> 4];
      o += hex[c & 15];
    }
  }
  return o;
}

}  // namespace geo
