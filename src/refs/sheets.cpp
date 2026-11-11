#include "refs/sheets.hpp"

#include <cstring>
#include <map>
#include <optional>
#include <stdexcept>

#include "net/zip.hpp"
#include "util/text.hpp"

namespace geo::refs::sheets {

namespace {

std::string_view as_view(std::span<const std::byte> b) { return {reinterpret_cast<const char*>(b.data()), b.size()}; }

std::string squash(std::string_view s) {  // case- and space-insensitive key
  std::string out;
  for (char c : s)
    if (c != ' ' && c != '_' && c != '\t') out += char(std::tolower(static_cast<unsigned char>(c)));
  return out;
}

const zip::Entry* member(const zip::Archive& z, std::string_view name) {
  for (auto& e : z.entries())
    if (iequals(e.name, name)) return &e;
  return nullptr;
}

std::string pick(const std::vector<std::string>& names, const std::vector<std::string>& prefer) {
  if (names.empty()) throw std::runtime_error("workbook has no sheets");
  for (auto& p : prefer)
    for (auto& n : names)
      if (squash(n) == squash(p)) return n;
  return names.front();
}

// ---------------------------------------------------------------- XML helpers (xlsx, rels)

std::string xml_unescape(std::string_view s) {
  std::string out;
  out.reserve(s.size());
  for (std::size_t i = 0; i < s.size(); ++i) {
    if (s[i] != '&') {
      out += s[i];
      continue;
    }
    auto semi = s.find(';', i);
    if (semi == std::string_view::npos) {
      out += s[i];
      continue;
    }
    auto ent = s.substr(i + 1, semi - i - 1);
    if (ent == "amp") out += '&';
    else if (ent == "lt") out += '<';
    else if (ent == "gt") out += '>';
    else if (ent == "quot") out += '"';
    else if (ent == "apos") out += '\'';
    else if (!ent.empty() && ent[0] == '#') {
      long cp = ent.size() > 1 && (ent[1] == 'x' || ent[1] == 'X') ? std::strtol(std::string(ent.substr(2)).c_str(), nullptr, 16)
                                                                   : std::strtol(std::string(ent.substr(1)).c_str(), nullptr, 10);
      if (cp < 0x80) out += char(cp);
      else out += '?';
    } else {
      out.append(s.substr(i, semi - i + 1));
    }
    i = semi;
  }
  return out;
}

// Value of attribute `name` inside the tag text `tag` ("<c r=\"A1\" t=\"s\">").
std::optional<std::string_view> attr(std::string_view tag, std::string_view name) {
  for (std::size_t p = 0; (p = tag.find(name, p)) != std::string_view::npos; p += name.size()) {
    if (p > 0 && tag[p - 1] != ' ' && tag[p - 1] != '\t' && tag[p - 1] != '\n' && tag[p - 1] != '\r') continue;
    std::size_t q = p + name.size();
    while (q < tag.size() && tag[q] == ' ') ++q;
    if (q >= tag.size() || tag[q] != '=') continue;
    ++q;
    while (q < tag.size() && tag[q] == ' ') ++q;
    if (q >= tag.size() || (tag[q] != '"' && tag[q] != '\'')) continue;
    char quote = tag[q];
    auto end = tag.find(quote, q + 1);
    if (end == std::string_view::npos) return std::nullopt;
    return tag.substr(q + 1, end - q - 1);
  }
  return std::nullopt;
}

// Each <tag ...> (or <tag .../>) element's opening-tag text, in order.
template <class F>
void each_tag(std::string_view xml, std::string_view tag, F&& f) {
  const std::string open = "<" + std::string(tag);
  for (std::size_t p = 0; (p = xml.find(open, p)) != std::string_view::npos;) {
    std::size_t after = p + open.size();
    if (after < xml.size() && xml[after] != ' ' && xml[after] != '>' && xml[after] != '/') {
      p = after;
      continue;
    }
    auto end = xml.find('>', after);
    if (end == std::string_view::npos) return;
    f(xml.substr(p, end - p + 1), end + 1);
    p = end + 1;
  }
}

std::map<std::string, std::string> read_rels(const zip::Archive& z, std::string_view path) {
  std::map<std::string, std::string> out;
  if (auto* e = member(z, path)) {
    std::string xml = z.read_all(*e);
    each_tag(xml, "Relationship", [&](std::string_view t, std::size_t) {
      auto id = attr(t, "Id"), target = attr(t, "Target");
      if (id && target) out[std::string(*id)] = std::string(*target);
    });
  }
  return out;
}

std::string resolve_target(std::string target) {  // relative to xl/
  if (target.starts_with("/")) return target.substr(1);
  if (target.starts_with("xl/")) return target;
  return "xl/" + target;
}

// ---------------------------------------------------------------- BIFF12 (xlsb)

class Biff {
 public:
  explicit Biff(std::string_view d) : d_(d) {}
  bool next(std::uint32_t& id, std::string_view& body) {
    if (p_ >= d_.size()) return false;
    id = 0;  // record type as pyxlsb and [MS-XLSB] list it: the raw bytes, little-endian
    for (int i = 0; i < 2; ++i) {
      if (p_ >= d_.size()) return false;
      auto b = static_cast<unsigned char>(d_[p_++]);
      id |= std::uint32_t(b) << (8 * i);
      if (!(b & 0x80)) break;
    }
    std::uint32_t len = 0;
    for (int i = 0; i < 4; ++i) {
      if (p_ >= d_.size()) return false;
      auto b = static_cast<unsigned char>(d_[p_++]);
      len |= std::uint32_t(b & 0x7F) << (7 * i);
      if (!(b & 0x80)) break;
    }
    if (len > d_.size() - p_) throw std::runtime_error("xlsb: record runs past the end of its part");
    body = d_.substr(p_, len);
    p_ += len;
    return true;
  }

 private:
  std::string_view d_;
  std::size_t p_ = 0;
};

std::uint32_t u32(std::string_view b, std::size_t at) {
  if (at + 4 > b.size()) throw std::runtime_error("xlsb: short record");
  std::uint32_t v;
  std::memcpy(&v, b.data() + at, 4);
  return v;
}
double f64(std::string_view b, std::size_t at) {
  if (at + 8 > b.size()) throw std::runtime_error("xlsb: short record");
  double v;
  std::memcpy(&v, b.data() + at, 8);
  return v;
}
double rk(std::uint32_t r) {
  double v;
  if (r & 0x02) {
    v = double(static_cast<std::int32_t>(r) >> 2);
  } else {
    std::uint64_t bits = std::uint64_t(r & 0xFFFFFFFCu) << 32;
    std::memcpy(&v, &bits, 8);
  }
  return (r & 0x01) ? v / 100 : v;
}
// XLWideString at `at`: 4-byte character count, then UTF-16LE. Non-ASCII becomes '?'.
std::string wide(std::string_view b, std::size_t& at) {
  std::uint32_t n = u32(b, at);
  at += 4;
  if (n == 0xFFFFFFFFu) return {};
  if (at + std::size_t(n) * 2 > b.size()) throw std::runtime_error("xlsb: string runs past its record");
  std::string out;
  out.reserve(n);
  for (std::uint32_t i = 0; i < n; ++i) {
    unsigned c = static_cast<unsigned char>(b[at + 2 * i]) | unsigned(static_cast<unsigned char>(b[at + 2 * i + 1])) << 8;
    out += c < 0x80 ? char(c) : '?';
  }
  at += std::size_t(n) * 2;
  return out;
}

// Cell records BLANK..FMLA_BOOL; ERR (0x0003) and BOOL (0x0004) carry no number or text and are skipped.
constexpr std::uint32_t ROW = 0x0000, BLANK = 0x0001, RK = 0x0002, REAL = 0x0005, ST = 0x0006,
                        ISST = 0x0007, FMLA_STR = 0x0008, FMLA_NUM = 0x0009, FMLA_BOOL = 0x000A, SI = 0x0013, SHEET = 0x019C;

std::vector<std::pair<std::string, std::string>> xlsb_sheets(const zip::Archive& z) {
  auto* wb = member(z, "xl/workbook.bin");
  if (!wb) throw std::runtime_error("xlsb: no xl/workbook.bin");
  std::string bytes = z.read_all(*wb);
  auto rels = read_rels(z, "xl/_rels/workbook.bin.rels");
  std::vector<std::pair<std::string, std::string>> out;
  Biff r(bytes);
  std::uint32_t id;
  std::string_view body;
  while (r.next(id, body)) {
    if (id != SHEET) continue;
    std::size_t at = 8;  // hsState, iTabID
    std::string rel = wide(body, at), name = wide(body, at);
    auto it = rels.find(rel);
    if (it != rels.end()) out.emplace_back(name, resolve_target(it->second));
  }
  return out;
}

void read_xlsb(const zip::Archive& z, const std::string& part, const CellFn& fn) {
  std::vector<std::string> sst;
  if (auto* e = member(z, "xl/sharedStrings.bin")) {
    std::string bytes = z.read_all(*e);
    Biff r(bytes);
    std::uint32_t id;
    std::string_view body;
    while (r.next(id, body))
      if (id == SI) {
        std::size_t at = 1;  // flags
        sst.push_back(wide(body, at));
      }
  }
  auto* e = member(z, part);
  if (!e) throw std::runtime_error("xlsb: no sheet part " + part);
  std::string bytes = z.read_all(*e);
  Biff r(bytes);
  std::uint32_t id, row = 0;
  std::string_view body;
  Cell c;
  while (r.next(id, body)) {
    if (id == ROW) {
      row = u32(body, 0);
      continue;
    }
    if (id < BLANK || id > FMLA_BOOL) continue;
    const std::uint32_t col = u32(body, 0);  // then 4 bytes of style and flags
    c.num = NAN;
    c.str.clear();
    std::size_t at = 8;
    switch (id) {
      case RK: c.num = rk(u32(body, 8)); break;
      case REAL:
      case FMLA_NUM: c.num = f64(body, 8); break;
      case ISST: {
        auto i = u32(body, 8);
        if (i < sst.size()) c.str = sst[i];
        break;
      }
      case ST:
      case FMLA_STR: c.str = wide(body, at); break;
      default: continue;  // blank, bool, error
    }
    fn(row, col, c);
  }
}

// ---------------------------------------------------------------- xlsx

std::vector<std::pair<std::string, std::string>> xlsx_sheets(const zip::Archive& z) {
  auto* wb = member(z, "xl/workbook.xml");
  if (!wb) throw std::runtime_error("xlsx: no xl/workbook.xml");
  std::string xml = z.read_all(*wb);
  auto rels = read_rels(z, "xl/_rels/workbook.xml.rels");
  std::vector<std::pair<std::string, std::string>> out;
  each_tag(xml, "sheet", [&](std::string_view t, std::size_t) {
    auto name = attr(t, "name"), rid = attr(t, "r:id");
    if (!name || !rid) return;
    auto it = rels.find(std::string(*rid));
    if (it != rels.end()) out.emplace_back(xml_unescape(*name), resolve_target(it->second));
  });
  return out;
}

// Text of every <t> inside [a, b) of xml, concatenated (rich-text runs).
std::string text_runs(std::string_view x) {
  std::string out;
  each_tag(x, "t", [&](std::string_view tag, std::size_t after) {
    if (tag.ends_with("/>")) return;
    auto end = x.find("</t>", after);
    if (end != std::string_view::npos) out += xml_unescape(x.substr(after, end - after));
  });
  return out;
}

void read_xlsx(const zip::Archive& z, const std::string& part, const CellFn& fn) {
  std::vector<std::string> sst;
  if (auto* e = member(z, "xl/sharedStrings.xml")) {
    std::string xml = z.read_all(*e);
    for (std::size_t p = 0; (p = xml.find("<si", p)) != std::string::npos;) {
      auto end = xml.find("</si>", p);
      if (end == std::string::npos) break;
      sst.push_back(text_runs(std::string_view(xml).substr(p, end - p)));
      p = end + 5;
    }
  }
  auto* e = member(z, part);
  if (!e) throw std::runtime_error("xlsx: no sheet part " + part);
  std::string xml = z.read_all(*e);
  std::string_view x = xml;
  std::uint32_t row = 0, next_col = 0;
  Cell c;
  for (std::size_t p = 0; p < x.size();) {
    auto lt = x.find('<', p);
    if (lt == std::string_view::npos) break;
    auto gt = x.find('>', lt);
    if (gt == std::string_view::npos) break;
    std::string_view tag = x.substr(lt, gt - lt + 1);
    p = gt + 1;
    if (tag.starts_with("<row") && (tag.size() > 4 && (tag[4] == ' ' || tag[4] == '>'))) {
      if (auto r = attr(tag, "r")) row = std::uint32_t(std::max(1L, std::strtol(std::string(*r).c_str(), nullptr, 10)) - 1);
      else ++row;
      next_col = 0;
      continue;
    }
    if (!(tag.starts_with("<c") && tag.size() > 2 && (tag[2] == ' ' || tag[2] == '>' || tag[2] == '/'))) continue;
    std::uint32_t col = next_col;
    if (auto r = attr(tag, "r")) {
      std::size_t k = 0;
      while (k < r->size() && std::isalpha(static_cast<unsigned char>((*r)[k]))) ++k;
      col = column_index(r->substr(0, k));
    }
    next_col = col + 1;
    if (tag.ends_with("/>")) continue;
    auto end = x.find("</c>", p);
    if (end == std::string_view::npos) break;
    std::string_view inner = x.substr(p, end - p);
    p = end + 4;
    std::string type(attr(tag, "t").value_or(""));
    c.num = NAN;
    c.str.clear();
    if (type == "inlineStr") {
      c.str = text_runs(inner);
    } else {
      auto v0 = inner.find("<v>");
      if (v0 == std::string_view::npos) continue;
      auto v1 = inner.find("</v>", v0);
      std::string v = xml_unescape(inner.substr(v0 + 3, v1 - v0 - 3));
      if (type == "s") {
        auto i = std::strtoul(v.c_str(), nullptr, 10);
        if (i < sst.size()) c.str = sst[i];
      } else if (type == "str") {
        c.str = v;
      } else if (type.empty() || type == "n") {
        if (auto d = to_double(v)) c.num = *d;
      } else {
        continue;  // b, e
      }
    }
    fn(row, col, c);
  }
}

// ---------------------------------------------------------------- CSV

void read_csv(std::string_view text, const CellFn& fn) {
  char sep = 0;
  std::uint32_t row = 0;
  Cell c;
  for (std::size_t a = 0; a < text.size();) {
    auto nl = text.find('\n', a);
    if (nl == std::string_view::npos) nl = text.size();
    std::string_view line = text.substr(a, nl - a);
    a = nl + 1;
    if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
    if (trim(line).empty()) continue;
    if (!sep) sep = line.find(';') != std::string_view::npos ? ';' : line.find(',') != std::string_view::npos ? ',' : '\t';
    auto f = split(line, sep);
    for (std::uint32_t col = 0; col < f.size(); ++col) {
      std::string_view v = trim(f[col]);
      if (v.size() >= 2 && v.front() == '"' && v.back() == '"') v = v.substr(1, v.size() - 2);
      if (v.empty()) continue;
      c.num = NAN;
      c.str.clear();
      if (auto d = to_double(v)) c.num = *d;
      else c.str = std::string(v);
      fn(row, col, c);
    }
    ++row;
  }
}

// ---------------------------------------------------------------- BIFF8 (xls, Excel 97-2003)
// An OLE2 compound file ([MS-CFB]) holding a "Workbook" stream of BIFF8 records
// ([MS-XLS]). ERCOT published its 2012 frequency data this way.

constexpr std::string_view CFB_MAGIC = "\xD0\xCF\x11\xE0\xA1\xB1\x1A\xE1";

std::uint16_t u16(std::string_view b, std::size_t at) {
  if (at + 2 > b.size()) throw std::runtime_error("xls: short record");
  return std::uint16_t(static_cast<unsigned char>(b[at]) | unsigned(static_cast<unsigned char>(b[at + 1])) << 8);
}

// The bytes of one stream of a compound file.
std::string cfb_stream(std::string_view f, std::string_view want) {
  if (f.size() < 512 || f.substr(0, 8) != CFB_MAGIC) throw std::runtime_error("xls: not an OLE2 compound file");
  const std::size_t ssz = std::size_t(1) << u16(f, 0x1E), mssz = std::size_t(1) << u16(f, 0x20);
  if (ssz < 128 || ssz > 65536 || mssz > ssz) throw std::runtime_error("xls: bad sector sizes");
  const std::uint32_t cutoff = u32(f, 0x38);
  auto sector = [&](std::uint32_t s) -> std::string_view {
    std::size_t at = (std::size_t(s) + 1) * ssz;
    if (at + ssz > f.size()) throw std::runtime_error("xls: sector past the end of the file");
    return f.substr(at, ssz);
  };
  // FAT sectors: 109 in the header, the rest in the DIFAT chain.
  std::vector<std::uint32_t> fat_secs;
  const std::uint32_t n_fat = u32(f, 0x2C);
  for (std::uint32_t i = 0; i < 109 && fat_secs.size() < n_fat; ++i) fat_secs.push_back(u32(f, 0x4C + 4 * i));
  for (std::uint32_t d = u32(f, 0x44), hops = 0; d < 0xFFFFFFFAu && fat_secs.size() < n_fat; ++hops) {
    if (hops > f.size() / ssz) throw std::runtime_error("xls: DIFAT loop");
    auto s = sector(d);
    for (std::size_t i = 0; i + 1 < ssz / 4 && fat_secs.size() < n_fat; ++i) fat_secs.push_back(u32(s, 4 * i));
    d = u32(s, ssz - 4);
  }
  std::vector<std::uint32_t> fat;
  for (auto s : fat_secs) {
    auto b = sector(s);
    for (std::size_t i = 0; i < ssz / 4; ++i) fat.push_back(u32(b, 4 * i));
  }
  auto chain = [&](std::uint32_t s, const std::vector<std::uint32_t>& table, auto&& piece) {
    for (std::size_t hops = 0; s < 0xFFFFFFFAu; ++hops) {
      if (s >= table.size() || hops > table.size()) throw std::runtime_error("xls: broken sector chain");
      piece(s);
      s = table[s];
    }
  };
  std::string dir;
  chain(u32(f, 0x30), fat, [&](std::uint32_t s) { dir.append(sector(s)); });
  auto entry_name = [&](std::size_t at) {
    std::string n;
    const std::size_t len = std::min<std::size_t>(u16(dir, at + 0x40), 64);
    for (std::size_t i = 0; i + 2 < len; i += 2) n += char(dir[at + i]);  // UTF-16, length counts the NUL
    return n;
  };
  std::string root_stream;
  bool have_root = false;
  for (std::size_t at = 0; at + 128 <= dir.size(); at += 128) {
    const auto type = static_cast<unsigned char>(dir[at + 0x42]);
    const std::uint32_t start = u32(dir, at + 0x74);
    const std::uint64_t size = u32(dir, at + 0x78);
    if (type == 5 && !have_root) {  // root entry: owns the mini stream
      chain(start, fat, [&](std::uint32_t s) { root_stream.append(sector(s)); });
      have_root = true;
      continue;
    }
    if (type != 2 || !iequals(entry_name(at), want)) continue;
    std::string out;
    if (size >= cutoff) {
      chain(start, fat, [&](std::uint32_t s) { out.append(sector(s)); });
    } else {
      std::vector<std::uint32_t> minifat;
      chain(u32(f, 0x3C), fat, [&](std::uint32_t s) {
        auto b = sector(s);
        for (std::size_t i = 0; i < ssz / 4; ++i) minifat.push_back(u32(b, 4 * i));
      });
      chain(start, minifat, [&](std::uint32_t s) {
        if ((std::size_t(s) + 1) * mssz > root_stream.size()) throw std::runtime_error("xls: mini sector past the mini stream");
        out.append(root_stream, std::size_t(s) * mssz, mssz);
      });
    }
    if (out.size() < size) throw std::runtime_error("xls: stream shorter than its directory entry");
    out.resize(std::size_t(size));
    return out;
  }
  throw std::runtime_error("xls: no " + std::string(want) + " stream");
}

std::string xls_workbook(std::span<const std::byte> file) {
  std::string_view f = as_view(file);
  try {
    return cfb_stream(f, "Workbook");
  } catch (const std::runtime_error&) {
    return cfb_stream(f, "Book");  // BIFF5 name
  }
}

// BIFF8 records: 2-byte id, 2-byte length, body.
struct Biff8 {
  std::string_view d;
  std::size_t p = 0;
  bool next(std::uint16_t& id, std::string_view& body) {
    if (p + 4 > d.size()) return false;
    id = u16(d, p);
    const std::size_t len = u16(d, p + 2);
    if (p + 4 + len > d.size()) throw std::runtime_error("xls: record runs past the end of the stream");
    body = d.substr(p + 4, len);
    p += 4 + len;
    return true;
  }
};

constexpr std::uint16_t X_BOUNDSHEET = 0x0085, X_SST = 0x00FC, X_CONTINUE = 0x003C, X_EOF = 0x000A, X_NUMBER = 0x0203, X_RK = 0x027E,
                        X_MULRK = 0x00BD, X_LABELSST = 0x00FD, X_FORMULA = 0x0006, X_BOF = 0x0809;

// The shared string table: an SST record and its CONTINUEs. A string's characters may run
// into the next CONTINUE, which then starts with a fresh flags byte (8- or 16-bit).
std::vector<std::string> xls_sst(const std::vector<std::string_view>& parts) {
  std::vector<std::string> out;
  std::size_t pi = 0, at = 8;  // skip total and unique counts
  auto avail = [&] {
    while (pi < parts.size() && at >= parts[pi].size()) ++pi, at = 0;
    return pi < parts.size();
  };
  auto byte = [&]() -> unsigned {
    if (!avail()) throw std::runtime_error("xls: SST runs out");
    return static_cast<unsigned char>(parts[pi][at++]);
  };
  auto skip = [&](std::size_t n) {
    while (n) {
      if (!avail()) throw std::runtime_error("xls: SST runs out");
      std::size_t k = std::min(n, parts[pi].size() - at);
      at += k, n -= k;
    }
  };
  const std::uint32_t unique = parts.empty() || parts[0].size() < 8 ? 0 : u32(parts[0], 4);
  while (out.size() < unique && avail()) {
    std::size_t cch = byte();
    cch |= std::size_t(byte()) << 8;
    unsigned flags = byte();
    std::size_t runs = 0, ext = 0;
    if (flags & 0x08) runs = byte(), runs |= std::size_t(byte()) << 8;
    if (flags & 0x04)
      for (int i = 0; i < 4; ++i) ext |= std::size_t(byte()) << (8 * i);
    std::string s;
    for (std::size_t i = 0; i < cch; ++i) {
      if (at >= parts[pi].size()) {  // characters continue in the next record, after a new flags byte
        if (!avail()) throw std::runtime_error("xls: SST runs out");
        flags = byte();
      }
      unsigned c = byte();
      if (flags & 0x01) c |= byte() << 8;
      s += c < 0x80 ? char(c) : '?';
    }
    skip(4 * runs + ext);
    out.push_back(std::move(s));
  }
  return out;
}

std::vector<std::pair<std::string, std::string>> xls_sheets(std::string_view wb) {
  std::vector<std::pair<std::string, std::string>> out;  // name, stream offset of its BOF
  Biff8 r{wb};
  std::uint16_t id;
  std::string_view body;
  while (r.next(id, body)) {
    if (id != X_BOUNDSHEET || body.size() < 8) continue;
    if (static_cast<unsigned char>(body[5]) != 0) continue;  // worksheets only (not charts, macros)
    const std::size_t cch = static_cast<unsigned char>(body[6]);
    const bool wide16 = static_cast<unsigned char>(body[7]) & 1;
    std::string name;
    for (std::size_t i = 0; i < cch && 8 + (i + 1) * (wide16 ? 2 : 1) <= body.size(); ++i) {
      unsigned c = wide16 ? u16(body, 8 + 2 * i) : static_cast<unsigned char>(body[8 + i]);
      name += c < 0x80 ? char(c) : '?';
    }
    out.emplace_back(name, std::to_string(u32(body, 0)));
  }
  return out;
}

void read_xls(std::string_view wb, const std::string& bof, const CellFn& fn) {
  std::vector<std::string_view> sst_parts;
  {
    Biff8 r{wb};
    std::uint16_t id;
    std::string_view body;
    bool in_sst = false;
    while (r.next(id, body)) {
      if (id == X_SST) in_sst = true, sst_parts = {body};
      else if (in_sst && id == X_CONTINUE) sst_parts.push_back(body);
      else if (in_sst) break;
      if (id == X_EOF && !in_sst) break;  // end of the workbook globals
    }
  }
  const auto sst = xls_sst(sst_parts);
  Biff8 r{wb, std::size_t(std::stoull(bof))};
  std::uint16_t id;
  std::string_view body;
  Cell c;
  auto emit = [&](std::uint32_t row, std::uint32_t col) { fn(row, col, c); };
  bool started = false;
  while (r.next(id, body)) {
    if (id == X_BOF) {
      if (started) break;
      started = true;
      continue;
    }
    if (id == X_EOF) break;
    c.num = NAN;
    c.str.clear();
    switch (id) {
      case X_NUMBER:
        c.num = f64(body, 6);
        emit(u16(body, 0), u16(body, 2));
        break;
      case X_RK:
        c.num = rk(u32(body, 6));
        emit(u16(body, 0), u16(body, 2));
        break;
      case X_MULRK: {
        const std::uint32_t row = u16(body, 0), first = u16(body, 2);
        for (std::size_t i = 0; 4 + 6 * i + 8 <= body.size(); ++i) {  // 6-byte cells, then the last column
          c.num = rk(u32(body, 4 + 6 * i + 2));
          emit(row, first + std::uint32_t(i));
        }
        break;
      }
      case X_LABELSST: {
        auto i = u32(body, 6);
        if (i < sst.size()) c.str = sst[i];
        emit(u16(body, 0), u16(body, 2));
        break;
      }
      case X_FORMULA:  // cached result: a number unless the last two bytes are 0xFFFF
        if (body.size() >= 14 && u16(body, 12) != 0xFFFF) {
          c.num = f64(body, 6);
          emit(u16(body, 0), u16(body, 2));
        }
        break;
      default: break;
    }
  }
}

enum class Kind { xlsb, xlsx, xls, csv };
struct Opened {
  Kind kind;
  std::optional<zip::Archive> zip;
  std::string inner;  // a workbook or CSV unpacked from an outer zip
  std::span<const std::byte> bytes;
};

Opened open(std::span<const std::byte> file) {
  Opened o;
  o.bytes = file;
  auto head = as_view(file.first(std::min<std::size_t>(file.size(), 8)));
  if (head == CFB_MAGIC) {
    o.kind = Kind::xls;
    o.inner = xls_workbook(file);
    return o;
  }
  if (!zip::is_zip(head)) {
    o.kind = Kind::csv;
    return o;
  }
  o.zip.emplace(file);
  if (member(*o.zip, "xl/workbook.bin")) {
    o.kind = Kind::xlsb;
    return o;
  }
  if (member(*o.zip, "xl/workbook.xml")) {
    o.kind = Kind::xlsx;
    return o;
  }
  // A zip around one workbook or CSV.
  for (auto& e : o.zip->entries())
    if (ends_with_ci(e.name, ".xlsb") || ends_with_ci(e.name, ".xlsx") || ends_with_ci(e.name, ".xls") || ends_with_ci(e.name, ".csv")) {
      o.inner = o.zip->read_all(e);
      o.zip.reset();
      Opened in = open(std::as_bytes(std::span(o.inner.data(), o.inner.size())));
      if (in.kind == Kind::xls) return in;  // in.inner is now the Workbook stream
      in.inner = std::move(o.inner);  // keep the bytes alive with the archive that views them
      in.bytes = std::as_bytes(std::span(in.inner.data(), in.inner.size()));
      if (in.kind != Kind::csv) in.zip.emplace(in.bytes);
      return in;
    }
  throw std::runtime_error("zip holds no workbook (.xlsb/.xlsx/.xls) or .csv");
}

}  // namespace

std::uint32_t column_index(std::string_view letters) {
  std::uint32_t v = 0;
  for (char ch : letters) v = v * 26 + std::uint32_t(std::toupper(static_cast<unsigned char>(ch)) - 'A' + 1);
  return v ? v - 1 : 0;
}

Workbook inspect(std::span<const std::byte> file) {
  auto o = open(file);
  Workbook w;
  if (o.kind == Kind::csv) {
    w.format = "csv";
    w.sheets = {""};
    return w;
  }
  if (o.kind == Kind::xls) {
    w.format = "xls";
    for (auto& [name, bof] : xls_sheets(o.inner)) w.sheets.push_back(name);
    return w;
  }
  w.format = o.kind == Kind::xlsb ? "xlsb" : "xlsx";
  for (auto& [name, part] : o.kind == Kind::xlsb ? xlsb_sheets(*o.zip) : xlsx_sheets(*o.zip)) w.sheets.push_back(name);
  return w;
}

std::string read(std::span<const std::byte> file, const std::vector<std::string>& prefer, const CellFn& fn) {
  auto o = open(file);
  if (o.kind == Kind::csv) {
    read_csv(as_view(o.bytes), fn);
    return "";
  }
  auto sheets = o.kind == Kind::xls ? xls_sheets(o.inner) : o.kind == Kind::xlsb ? xlsb_sheets(*o.zip) : xlsx_sheets(*o.zip);
  std::vector<std::string> names;
  for (auto& s : sheets) names.push_back(s.first);
  const std::string name = pick(names, prefer);
  for (auto& [n, part] : sheets)
    if (n == name) {
      if (o.kind == Kind::xls) read_xls(o.inner, part, fn);
      else if (o.kind == Kind::xlsb) read_xlsb(*o.zip, part, fn);
      else read_xlsx(*o.zip, part, fn);
      break;
    }
  return name;
}

}  // namespace geo::refs::sheets
