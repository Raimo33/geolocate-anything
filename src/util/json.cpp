#include "util/json.hpp"

#include <algorithm>
#include <charconv>
#include <stdexcept>
#include <vector>

namespace geo {

std::string py_float(double x) {
  if (!std::isfinite(x)) return "null";
  char buf[64];
  auto r = std::to_chars(buf, buf + sizeof buf, x);
  std::string s(buf, r.ptr);
  // Python repr: exponent form below 1e-4 and from 1e16; "e-05" style with 2 digits.
  auto e = s.find('e');
  double ax = std::fabs(x);
  if (e == std::string::npos && ax != 0 && (ax < 1e-4 || ax >= 1e16)) {
    r = std::to_chars(buf, buf + sizeof buf, x, std::chars_format::scientific);
    s.assign(buf, r.ptr);
    e = s.find('e');
  } else if (e != std::string::npos && ax >= 1e-4 && ax < 1e16) {
    r = std::to_chars(buf, buf + sizeof buf, x, std::chars_format::fixed);
    s.assign(buf, r.ptr);
    e = std::string::npos;
  }
  if (e != std::string::npos) {
    std::string mant = s.substr(0, e), ex = s.substr(e + 1);
    char sign = '+';
    if (!ex.empty() && (ex[0] == '-' || ex[0] == '+')) {
      sign = ex[0];
      ex.erase(0, 1);
    }
    while (ex.size() > 1 && ex[0] == '0') ex.erase(0, 1);
    if (ex.size() < 2) ex.insert(0, "0");
    return mant + "e" + sign + ex;
  }
  if (s.find('.') == std::string::npos) s += ".0";
  return s;
}

namespace {

void put_string(std::string& out, std::string_view s, bool ascii) {
  out += '"';
  for (std::size_t i = 0; i < s.size(); ++i) {
    unsigned char c = static_cast<unsigned char>(s[i]);
    switch (c) {
      case '"': out += "\\\""; continue;
      case '\\': out += "\\\\"; continue;
      case '\n': out += "\\n"; continue;
      case '\r': out += "\\r"; continue;
      case '\t': out += "\\t"; continue;
      case '\b': out += "\\b"; continue;
      case '\f': out += "\\f"; continue;
      default: break;
    }
    char b[16];
    if (c < 0x20) {
      std::snprintf(b, sizeof b, "\\u%04x", c);
      out += b;
    } else if (c < 0x80 || !ascii) {
      out += static_cast<char>(c);
    } else {
      // Decode one UTF-8 sequence and escape it (surrogate pairs above the BMP).
      std::uint32_t cp = 0;
      int extra = c >= 0xF0 ? 3 : c >= 0xE0 ? 2 : c >= 0xC0 ? 1 : 0;
      cp = c & (0x3F >> extra);
      for (int k = 0; k < extra && i + 1 < s.size(); ++k) cp = (cp << 6) | (static_cast<unsigned char>(s[++i]) & 0x3F);
      if (cp >= 0x10000) {
        cp -= 0x10000;
        std::snprintf(b, sizeof b, "\\u%04x\\u%04x", 0xD800 + (cp >> 10), 0xDC00 + (cp & 0x3FF));
      } else {
        std::snprintf(b, sizeof b, "\\u%04x", cp);
      }
      out += b;
    }
  }
  out += '"';
}

void put(std::string& out, const json::value& v, const DumpOptions& o, int depth) {
  auto newline = [&](int d) {
    if (o.indent < 0) return;
    out += '\n';
    out.append(static_cast<std::size_t>(o.indent * d), ' ');
  };
    const char* key_sep = o.indent < 0 ? ":" : ": ";
  switch (v.kind()) {
    case json::kind::null: out += "null"; break;
    case json::kind::bool_: out += v.get_bool() ? "true" : "false"; break;
    case json::kind::int64: out += std::to_string(v.get_int64()); break;
    case json::kind::uint64: out += std::to_string(v.get_uint64()); break;
    case json::kind::double_: out += py_float(v.get_double()); break;
    case json::kind::string: put_string(out, v.get_string(), o.ascii); break;
    case json::kind::array: {
      const auto& a = v.get_array();
      if (a.empty()) {
        out += "[]";
        break;
      }
      out += '[';
      for (std::size_t i = 0; i < a.size(); ++i) {
        if (i) out += ',';
        newline(depth + 1);
        put(out, a[i], o, depth + 1);
      }
      newline(depth);
      out += ']';
      break;
    }
    case json::kind::object: {
      const auto& obj = v.get_object();
      if (obj.empty()) {
        out += "{}";
        break;
      }
      std::vector<const json::key_value_pair*> kv;
      kv.reserve(obj.size());
      for (const auto& p : obj) kv.push_back(&p);
      if (o.sort_keys) std::sort(kv.begin(), kv.end(), [](auto* a, auto* b) { return a->key() < b->key(); });
      out += '{';
      for (std::size_t i = 0; i < kv.size(); ++i) {
        if (i) out += ',';
        newline(depth + 1);
        put_string(out, kv[i]->key(), o.ascii);
        out += key_sep;
        put(out, kv[i]->value(), o, depth + 1);
      }
      newline(depth);
      out += '}';
      break;
    }
  }
}

}  // namespace

std::string dump(const json::value& v, DumpOptions o) {
  std::string out;
  put(out, v, o, 0);
  return out;
}

json::value parse_json(std::string_view text) {
  boost::system::error_code ec;
  json::parse_options po;
  po.allow_invalid_utf8 = true;
  po.numbers = json::number_precision::precise;
  auto v = json::parse(text, ec, {}, po);
  if (ec) throw std::runtime_error("invalid JSON: " + ec.message());
  return v;
}

const json::value* jget(const json::value& v, std::string_view key) {
  if (!v.is_object()) return nullptr;
  return v.get_object().if_contains(key);
}

std::string jstr(const json::value& v, std::string_view key, std::string_view fallback) {
  auto* p = jget(v, key);
  return p && p->is_string() ? std::string(p->get_string()) : std::string(fallback);
}

double as_number(const json::value& v, double fallback) {
  if (v.is_double()) return v.get_double();
  if (v.is_int64()) return static_cast<double>(v.get_int64());
  if (v.is_uint64()) return static_cast<double>(v.get_uint64());
  return fallback;
}

double jnum(const json::value& v, std::string_view key, double fallback) {
  auto* p = jget(v, key);
  return p ? as_number(*p, fallback) : fallback;
}

std::int64_t jint(const json::value& v, std::string_view key, std::int64_t fallback) {
  auto* p = jget(v, key);
  if (!p) return fallback;
  if (p->is_int64()) return p->get_int64();
  if (p->is_uint64()) return static_cast<std::int64_t>(p->get_uint64());
  if (p->is_double()) return static_cast<std::int64_t>(p->get_double());
  return fallback;
}

bool jbool(const json::value& v, std::string_view key, bool fallback) {
  auto* p = jget(v, key);
  return p && p->is_bool() ? p->get_bool() : fallback;
}

}  // namespace geo
