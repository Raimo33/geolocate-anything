// Offline tests for the downloaders' parsing, time handling and fetch bookkeeping.
#include <zlib.h>

#include <fstream>

#include "helpers.hpp"
#include "net/zip.hpp"
#include "refs/aemo_annual.hpp"
#include "refs/parsers.hpp"
#include "refs/sheets.hpp"
#include "refs/sources.hpp"
#include "util/files.hpp"

using namespace geo;
using test::utc;

static std::vector<double> to_utc(const std::vector<std::string>& wall, const char* tz) {
  LocalToUtc conv(tz);
  std::vector<double> out;
  for (auto& w : wall) out.push_back(conv(parse_iso_datetime(w)->naive()));
  return out;
}

TEST(LocalTime, AutumnRepeatedHourInOrder) {
  // Helsinki 2025-10-26: 04:00 EEST -> 03:00 EET, so 03:00..03:59 is logged twice.
  auto got = to_utc({"2025-10-26 02:59:59", "2025-10-26 03:00:00", "2025-10-26 03:59:59", "2025-10-26 03:00:00", "2025-10-26 03:59:59",
                     "2025-10-26 04:00:00"},
                    "Europe/Helsinki");
  std::vector<double> want;
  for (auto s : {"2025-10-25 23:59:59", "2025-10-26 00:00:00", "2025-10-26 00:59:59", "2025-10-26 01:00:00", "2025-10-26 01:59:59",
                 "2025-10-26 02:00:00"})
    want.push_back(utc(s));
  EXPECT_EQ(got, want);
}

// A stored (uncompressed) zip, written by hand.
static std::string make_zip(const std::vector<std::pair<std::string, std::string>>& files) {
  std::string out, cd;
  auto u16 = [](std::string& s, unsigned v) { s.push_back(char(v & 255)), s.push_back(char(v >> 8 & 255)); };
  auto u32 = [&](std::string& s, unsigned long v) { u16(s, unsigned(v & 0xFFFF)), u16(s, unsigned(v >> 16)); };
  for (auto& [name, data] : files) {
    unsigned long crc = crc32(0, reinterpret_cast<const Bytef*>(data.data()), uInt(data.size()));
    unsigned long off = out.size();
    u32(out, 0x04034b50), u16(out, 20), u16(out, 0), u16(out, 0), u16(out, 0), u16(out, 0), u32(out, crc), u32(out, data.size()),
        u32(out, data.size()), u16(out, unsigned(name.size())), u16(out, 0);
    out += name + data;
    u32(cd, 0x02014b50), u16(cd, 20), u16(cd, 20), u16(cd, 0), u16(cd, 0), u16(cd, 0), u16(cd, 0), u32(cd, crc), u32(cd, data.size()),
        u32(cd, data.size()), u16(cd, unsigned(name.size())), u16(cd, 0), u16(cd, 0), u16(cd, 0), u16(cd, 0), u32(cd, 0), u32(cd, off);
    cd += name;
  }
  unsigned long cd_off = out.size();
  out += cd;
  u32(out, 0x06054b50), u16(out, 0), u16(out, 0), u16(out, unsigned(files.size())), u16(out, unsigned(files.size())), u32(out, cd.size()),
      u32(out, cd_off), u16(out, 0);
  return out;
}

// ---- hand-built Excel binary workbooks ([MS-XLSB] records: id, 7-bit length, body)
namespace xlsb {
std::string rec(unsigned id, const std::string& body) {
  std::string r;
  r += char(id & 0xFF);
  if (id > 0xFF) r += char(id >> 8);
  std::size_t n = body.size();
  do {
    unsigned char b = n & 0x7F;
    n >>= 7;
    r += char(n ? b | 0x80 : b);
  } while (n);
  return r + body;
}
std::string u32(std::uint32_t v) { return std::string(reinterpret_cast<const char*>(&v), 4); }
std::string f64(double v) { return std::string(reinterpret_cast<const char*>(&v), 8); }
std::string wide(const std::string& s) {
  std::string r = u32(std::uint32_t(s.size()));
  for (char c : s) r += c, r += '\0';
  return r;
}
std::string cell(std::uint32_t col) { return u32(col) + u32(0); }  // column, style + flags
std::string real(std::uint32_t col, double v) { return rec(0x05, cell(col) + f64(v)); }
std::string rk_int(std::uint32_t col, std::int32_t v, bool x100) { return rec(0x02, cell(col) + u32(std::uint32_t(v) << 2 | 2 | (x100 ? 1 : 0))); }
std::string isst(std::uint32_t col, std::uint32_t i) { return rec(0x07, cell(col) + u32(i)); }
std::string fmla(std::uint32_t col, double v) { return rec(0x09, cell(col) + f64(v) + std::string(2, '\0') + u32(0)); }
std::string row(std::uint32_t r) { return rec(0x00, u32(r) + u32(0) + std::string(9, '\0')); }
std::string workbook(const std::string& sheet_xml_body, const std::vector<std::string>& more_strings = {}) {
  std::string wb = rec(0x0183, "") + rec(0x018F, "") + rec(0x019C, u32(0) + u32(1) + wide("rId1") + wide("Frequency data")) + rec(0x0190, "") +
                   rec(0x0184, "");
  std::string rels =
      R"(<?xml version="1.0"?><Relationships xmlns="x"><Relationship Id="rId1" Type="t" Target="worksheets/sheet1.bin"/></Relationships>)";
  std::string sst = rec(0x019F, u32(1 + more_strings.size()) + u32(1 + more_strings.size())) + rec(0x13, std::string(1, '\0') + wide("Time"));
  for (auto& x : more_strings) sst += rec(0x13, std::string(1, '\0') + wide(x));
  sst += rec(0x01A0, "");
  return make_zip({{"xl/workbook.bin", wb}, {"xl/_rels/workbook.bin.rels", rels}, {"xl/sharedStrings.bin", sst},
                   {"xl/worksheets/sheet1.bin", rec(0x0181, "") + rec(0x0191, "") + sheet_xml_body + rec(0x0192, "") + rec(0x0182, "")}});
}
}  // namespace xlsb

static std::span<const std::byte> bytes_of(const std::string& s) { return std::as_bytes(std::span(s.data(), s.size())); }

TEST(Sheets, XlsbCellsOfEveryKind) {
  using namespace xlsb;
  std::string body = row(0) + isst(0, 0) + real(1, 44927) + rk_int(2, 44928, false) + row(1) + real(0, 0.0) + rk_int(1, 5001, true) +
                     fmla(2, 49.99);
  auto wb = workbook(body);
  auto info = refs::sheets::inspect(bytes_of(wb));
  EXPECT_EQ(info.format, "xlsb");
  ASSERT_EQ(info.sheets.size(), 1u);
  EXPECT_EQ(info.sheets[0], "Frequency data");
  std::vector<std::tuple<unsigned, unsigned, double, std::string>> got;
  refs::sheets::read(bytes_of(wb), {}, [&](std::uint32_t r, std::uint32_t c, const refs::sheets::Cell& v) { got.emplace_back(r, c, v.num, v.str); });
  ASSERT_EQ(got.size(), 6u);
  EXPECT_EQ(std::get<3>(got[0]), "Time");
  EXPECT_DOUBLE_EQ(std::get<2>(got[1]), 44927);
  EXPECT_DOUBLE_EQ(std::get<2>(got[2]), 44928);  // integer RK
  EXPECT_EQ(std::get<0>(got[3]), 1u);
  EXPECT_DOUBLE_EQ(std::get<2>(got[4]), 50.01);  // RK integer / 100
  EXPECT_DOUBLE_EQ(std::get<2>(got[5]), 49.99);  // cached formula result
}

TEST(AemoAnnual, DayColumnsInNemTime) {
  using namespace xlsb;
  // Columns: Time | 2023-01-01 | 2023-01-02; rows 00:00:00 and 00:00:04 (NEM time).
  std::string body = row(0) + isst(0, 0) + real(1, 44927) + real(2, 44928);
  body += row(1) + real(0, 0.0) + real(1, 50.01) + real(2, 49.99);
  body += row(2) + real(0, 4.0 / 86400) + real(1, 50.03) + real(2, 0.0);  // 0 Hz: not a frequency
  auto a = refs::parse_aemo_annual(bytes_of(xlsb::workbook(body)));
  EXPECT_EQ(a.layout, "day columns");
  EXPECT_EQ(a.samples, 3u);
  EXPECT_EQ(a.rejected, 1u);
  EXPECT_DOUBLE_EQ(double(a.first), utc("2022-12-31 14:00:00"));  // 2023-01-01 00:00 NEM time
  ASSERT_TRUE(a.months.count("2022-12"));
  ASSERT_TRUE(a.months.count("2023-01"));
  auto [d0, d1] = month_bounds("2022-12");
  auto& dec = a.months["2022-12"];
  const auto i = std::size_t(utc("2022-12-31 14:00:00") - double(d0));
  EXPECT_NEAR(dec[i], 0.01, 1e-6);
  EXPECT_NEAR(dec[i + 2], 0.02, 1e-6);  // 4 s samples interpolated
  EXPECT_NEAR(dec[i + 4], 0.03, 1e-6);
  auto [j0, j1] = month_bounds("2023-01");
  EXPECT_NEAR(a.months["2023-01"][std::size_t(utc("2023-01-01 14:00:00") - double(j0))], -0.01, 1e-6);
}

static std::vector<float> zeros(const std::string& m, float v = 0.0f) {
  auto [a, b] = month_bounds(m);
  return std::vector<float>(std::size_t(b - a), v);
}

class Fetch : public test::TempStore {
 protected:
  void TearDown() override {
    refs::testing::remove_fake_sources();
    TempStore::TearDown();
  }
};

TEST_F(Fetch, WritesSpillRefetchesStaleAndPartial) {
  auto [t0, t1] = month_bounds("2019-03");
  std::vector<std::string> calls;
  refs::testing::add_fake_source(
      {"x", {"nordic"}, "test", "test", false, 4},
      [&](const std::string&, const std::string& month) {
        calls.push_back(month);
        auto [a, b] = month_bounds(month);
        refs::testing::FakeMonth f;
        f.values = zeros(month, 0.01f);
        for (std::size_t i = f.values.size() - 3600; i < f.values.size(); ++i) f.values[i] = NAN;  // last hour is in next month's file
        f.meta = {{"source", "x"}, {"format", 2}, {"partial", month == "2019-04"}};
        f.spill.push_back({a - 3600, std::vector<float>(3600, 0.02f)});
        return f;
      },
      [](const std::string&) { return std::vector<std::string>{}; }, 2);
  store::save_month("nordic", "x", "2019-02", zeros("2019-02"), {{"format", 1}});  // an old-format month is stale
  EXPECT_EQ(refs::stale_months("nordic", "x"), (std::vector<std::string>{"2019-02"}));
  refs::fetch("x", "nordic", {"2019-03"});
  auto tail = store::read("nordic", t0 - 3600, t0, {"x"});
  for (float v : tail) ASSERT_NEAR(v, 0.02, 1e-4);  // 2019-03's spill filled February's last hour
  EXPECT_EQ(jnum(*store::month_info("nordic", "x", "2019-02"), "coverage"), 1.0);
  refs::fetch("x", "nordic", {"2019-02", "2019-03", "2019-04"});
  EXPECT_EQ(calls, (std::vector<std::string>{"2019-03", "2019-02", "2019-04"}));  // 2019-02 stale, 2019-03 cached
  EXPECT_TRUE(jbool(*store::month_info("nordic", "x", "2019-04"), "partial"));
  tail = store::read("nordic", t0 - 3600, t0, {"x"});
  for (float v : tail) ASSERT_NEAR(v, 0.02, 1e-4);  // February's own data must not erase the spilled hour
  refs::fetch("x", "nordic", {"2019-04"});
  EXPECT_EQ(calls.back(), "2019-04");  // partial months are fetched again
}

TEST_F(Fetch, RefreshPlanBackfillsWholeArchive) {
  auto none = [](const std::string&, const std::string&) -> refs::testing::FakeMonth { throw std::runtime_error("unused"); };
  // Hide the real sources of great_britain behind fakes by using a grid only fakes cover.
  refs::testing::add_fake_source({"live", {"testgrid"}, "t", "t", true, 4}, none,
                                 [](const std::string&) { return months_between("2026-05", "2026-12"); });
  refs::testing::add_fake_source({"old", {"testgrid"}, "t", "t", false, 4}, none,
                                 [](const std::string&) { return months_between("2014-01", "2014-04"); });
  store::save_month("testgrid", "live", "2026-08", zeros("2026-08"), {});
  store::save_month("testgrid", "live", "2026-11", zeros("2026-11"), {{"partial", true}});
  store::mark_unavailable("testgrid", "live", "2026-05", "none");  // old: not asked again
  store::mark_unavailable("testgrid", "live", "2026-10", "none");  // recent: may appear
  store::mark_unavailable("testgrid", "old", "2014-02", "none");
  store::save_month("testgrid", "old", "2014-03", zeros("2014-03"), {});
  auto plan = refs::refresh_plan("testgrid", utc("2026-11-05"));
  decltype(plan) want{{"live", {"2026-06", "2026-07", "2026-09", "2026-10", "2026-11"}}, {"old", {"2014-01", "2014-04"}}};
  EXPECT_EQ(plan, want);
}

TEST_F(Fetch, RescreenedMonthsAreFetchedAgain) {
  auto none = [](const std::string&, const std::string&) -> refs::testing::FakeMonth { throw std::runtime_error("unused"); };
  refs::testing::add_fake_source({"x", {"testgrid"}, "t", "t", false, 4}, none,
                                 [](const std::string&) { return months_between("2012-01", "2012-04"); }, 2, {{"2012-02", "2012-03", 3}});
  for (const char* m : {"2012-01", "2012-02"}) store::save_month("testgrid", "x", m, zeros(m), {{"format", 2}});
  store::mark_unavailable("testgrid", "x", "2012-03", "none", 2);  // fresh, but from before the rescreen
  store::mark_unavailable("testgrid", "x", "2012-04", "none", 2);  // outside the range: stands
  auto plan = refs::refresh_plan("testgrid", utc("2026-11-05"));
  EXPECT_EQ(plan, (decltype(plan){{"x", {"2012-02", "2012-03"}}}));
  store::save_month("testgrid", "x", "2012-02", zeros("2012-02"), {{"format", 3}});
  store::mark_unavailable("testgrid", "x", "2012-03", "none", 3);
  EXPECT_TRUE(refs::refresh_plan("testgrid", utc("2026-11-05")).empty());
}

TEST_F(Fetch, UnlistableSourceIsReportedAndItsStaleMonthsStillPlanned) {
  auto none = [](const std::string&, const std::string&) -> refs::testing::FakeMonth { throw std::runtime_error("unused"); };
  refs::testing::add_fake_source({"x", {"testgrid"}, "t", "t", true, 4}, none,
                                 [](const std::string&) -> std::vector<std::string> { throw std::runtime_error("page layout changed"); });
  store::save_month("testgrid", "x", "2026-10", zeros("2026-10"), {{"partial", true}});
  std::vector<std::string> problems;
  auto plan = refs::refresh_plan("testgrid", utc("2026-11-05"), &problems);
  EXPECT_EQ(plan, (decltype(plan){{"x", {"2026-10"}}}));
  ASSERT_EQ(problems.size(), 1u);
  EXPECT_NE(problems[0].find("x: cannot list months (page layout changed)"), std::string::npos);
}

TEST_F(Fetch, NesoSeamRefusesARepeatedHour) {
  // The month before ends on a ramp; a file whose first hour is that same ramp is an hour off.
  auto [a, b] = month_bounds("2026-11");
  std::vector<float> oct = zeros("2026-10");
  for (std::size_t i = 0; i < 3600; ++i) oct[oct.size() - 3600 + i] = float(i % 97) * 0.001f - 0.04f;
  std::vector<float> nov(std::size_t(b - a), 0.01f);
  EXPECT_THROW(refs::check_neso_seam("testgrid", "2026-11", nov), std::runtime_error);  // nothing to check against
  store::save_month("testgrid", "neso", "2026-10", oct, {});
  EXPECT_NO_THROW(refs::check_neso_seam("testgrid", "2026-11", nov));
  std::copy(oct.end() - 3600, oct.end(), nov.begin());
  EXPECT_THROW(refs::check_neso_seam("testgrid", "2026-11", nov), std::runtime_error);
  EXPECT_NO_THROW(refs::check_neso_seam("testgrid", "2026-03", nov));  // checked by hand: no month before needed
}

TEST_F(Fetch, OldUnavailableMarksAreAskedAgain) {
  auto none = [](const std::string&, const std::string&) -> refs::testing::FakeMonth { throw std::runtime_error("unused"); };
  auto months = [](const std::string&) { return months_between("2022-02", "2022-04"); };
  refs::testing::add_fake_source({"x", {"testgrid"}, "t", "t", false, 4}, none, months);
  store::save_month("testgrid", "x", "2022-02", zeros("2022-02"), {});
  store::save_month("testgrid", "x", "2022-04", zeros("2022-04"), {});
  store::mark_unavailable("testgrid", "x", "2022-03", "no file");
  EXPECT_TRUE(refs::refresh_plan("testgrid").empty());  // a fresh mark stands
  store::update_manifest("testgrid", "x", [](json::object& man) {
    man["unavailable"].as_object()["2022-03"].as_object()["checked_at"] = "2020-01-01T00:00:00+00:00";
  });
  auto plan = refs::refresh_plan("testgrid");
  EXPECT_EQ(plan, (decltype(plan){{"x", {"2022-03"}}}));
}

TEST_F(Fetch, FailedMonthIsReportedAndPlannedAgain) {
  refs::testing::add_fake_source(
      {"x", {"testgrid"}, "t", "t", false, 4},
      [](const std::string&, const std::string& month) {
        if (month == "2020-02") throw std::runtime_error("connection reset");
        refs::testing::FakeMonth f;
        f.values = zeros(month, 0.01f);
        return f;
      },
      [](const std::string&) { return months_between("2020-01", "2020-02"); });
  std::map<std::string, std::string> failed;
  EXPECT_EQ(refs::fetch("x", "testgrid", {"2020-01", "2020-02"}, false, &failed), (std::vector<std::string>{"2020-01"}));
  EXPECT_EQ(failed, (std::map<std::string, std::string>{{"2020-02", "connection reset"}}));
  auto man = store::read_manifest("testgrid", "x");
  EXPECT_FALSE(man.contains("unavailable") && man["unavailable"].as_object().contains("2020-02"));  // transient: not recorded
  auto plan = refs::refresh_plan("testgrid", utc("2026-11-05"));
  EXPECT_EQ(plan, (decltype(plan){{"x", {"2020-02"}}}));
}

TEST_F(Fetch, StoresOtherGridAndRemembersUnavailable) {
  refs::testing::add_fake_source(
      {"x", {"australia_nem", "australia_tas"}, "t", "t", false, 4},
      [](const std::string&, const std::string& month) {
        if (month == "2020-02") throw refs::Unavailable("nothing published");
        refs::testing::FakeMonth f;
        f.values = zeros(month, 0.0123456f);
        f.also.emplace_back("australia_tas", zeros(month, 0.0246912f), json::object{});
        return f;
      },
      [](const std::string&) { return std::vector<std::string>{}; });
  EXPECT_EQ(refs::fetch("x", "australia_nem", {"2020-01", "2020-02"}), (std::vector<std::string>{"2020-01"}));
  EXPECT_TRUE(refs::is_cached("x", "australia_tas", "2020-01"));
  auto man = store::read_manifest("australia_nem", "x");
  EXPECT_TRUE(man["unavailable"].as_object().contains("2020-02"));
  auto [a, b] = month_bounds("2020-01");
  auto nem = store::read("australia_nem", a, a + 2, {"x"}), tas = store::read("australia_tas", a, a + 2, {"x"});
  EXPECT_NEAR(nem[0], 0.012, 1e-6);  // the one lossy step: stored at 1 mHz
  EXPECT_NEAR(tas[1], 0.025, 1e-6);
}

TEST(Sources, JointGridsShareARefreshLane) {
  EXPECT_EQ(refs::refresh_lane("australia_nem"), refs::refresh_lane("australia_tas"));
  EXPECT_EQ(refs::refresh_lane("ireland"), "ireland");
  EXPECT_NE(refs::refresh_lane("great_britain"), refs::refresh_lane("southern_africa"));  // pmu_zenodo reads one file per grid
  EXPECT_EQ(refs::refresh_lane("us_eastern"), refs::refresh_lane("ips_ups"));  // one EarthScope account: 5 connections
  EXPECT_EQ(refs::refresh_lane("us_western"), refs::refresh_lane("ercot"));  // GEOLOC_US_SEISMIC_REFRESH, one account
}

// ---------------------------------------------------------------- legacy .xls (BIFF8 in an OLE2 compound file)

namespace xls {
std::string u16(std::uint16_t v) { return std::string{char(v & 0xFF), char(v >> 8)}; }
std::string u32(std::uint32_t v) { return u16(std::uint16_t(v)) + u16(std::uint16_t(v >> 16)); }
std::string f64(double v) {
  std::string s(8, '\0');
  std::memcpy(s.data(), &v, 8);
  return s;
}
std::string rec(std::uint16_t id, const std::string& body) { return u16(id) + u16(std::uint16_t(body.size())) + body; }
std::string rc(std::uint16_t r, std::uint16_t c) { return u16(r) + u16(c) + u16(0); }  // row, column, XF
std::string bof() { return rec(0x0809, u16(0x0600) + u16(0x0010) + std::string(12, '\0')); }
// A compound file with 512-byte sectors holding one "Workbook" stream (padded past the
// 4096-byte mini-stream cutoff so it lives in ordinary sectors).
std::string cfb(std::string wb) {
  wb.resize(std::max<std::size_t>(wb.size(), 4096 + 100), '\0');
  const std::uint32_t size = std::uint32_t(wb.size()), n = (size + 511) / 512;
  std::string h = "\xD0\xCF\x11\xE0\xA1\xB1\x1A\xE1" + std::string(16, '\0') + u16(0x3E) + u16(3) + u16(0xFFFE) + u16(9) + u16(6) +
                  std::string(6, '\0') + u32(0) + u32(1) + u32(1) + u32(0) + u32(4096) + u32(0xFFFFFFFE) + u32(0) + u32(0xFFFFFFFE) + u32(0);
  h += u32(0);  // DIFAT[0]: the FAT is sector 0
  while (h.size() < 512) h += u32(0xFFFFFFFF);
  std::string fat = u32(0xFFFFFFFD) + u32(0xFFFFFFFE);  // FAT sector, directory (one sector)
  for (std::uint32_t i = 0; i < n; ++i) fat += u32(i + 1 < n ? 3 + i : 0xFFFFFFFE);
  while (fat.size() < 512) fat += u32(0xFFFFFFFF);
  auto entry = [&](const std::string& name, char type, std::uint32_t start, std::uint32_t sz) {
    std::string e;
    for (char ch : name) e += std::string{ch, '\0'};
    e.resize(64, '\0');
    e += u16(std::uint16_t(2 * name.size() + 2)) + std::string{type, 1} + u32(0xFFFFFFFF) + u32(0xFFFFFFFF) + u32(type == 5 ? 1 : 0xFFFFFFFF);
    e.resize(0x74, '\0');
    return e + u32(start) + u32(sz) + u32(0);
  };
  std::string dir = entry("Root Entry", 5, 0xFFFFFFFE, 0) + entry("Workbook", 2, 2, size);
  dir.resize(512, '\0');
  wb.resize(std::size_t(n) * 512, '\0');
  return h + fat + dir + wb;
}
}  // namespace xls

TEST(Zip, CutShortOrCorruptMembersAreRefused) {
  std::string data(5000, 'x');
  for (std::size_t i = 0; i < data.size(); ++i) data[i] = char('a' + i * 7 % 26);
  auto z = make_zip({{"a.csv", data}});
  zip::Archive ok(z);
  EXPECT_EQ(ok.read_all(ok.entries()[0]), data);
  std::string bad = z;
  bad[30 + 5 + 100] ^= 1;  // one byte of the stored member flipped
  zip::Archive b(bad);
  EXPECT_THROW(b.read_all(b.entries()[0]), std::runtime_error);
  // A deflated member whose stream stops early.
  z_stream zs{};
  deflateInit2(&zs, 6, Z_DEFLATED, -MAX_WBITS, 8, Z_DEFAULT_STRATEGY);
  std::string comp(10000, '\0');
  zs.next_in = reinterpret_cast<Bytef*>(data.data()), zs.avail_in = uInt(data.size());
  zs.next_out = reinterpret_cast<Bytef*>(comp.data()), zs.avail_out = uInt(comp.size());
  deflate(&zs, Z_FINISH);
  comp.resize(zs.total_out);
  deflateEnd(&zs);
  std::string cut = comp.substr(0, comp.size() / 2);
  std::string local = std::string("PK\x03\x04", 4) + std::string(26, '\0') + cut;
  zip::Entry e;
  e.name = "cut.csv", e.method = 8, e.comp_size = cut.size(), e.size = data.size(), e.crc = uint32_t(crc32(0, reinterpret_cast<const Bytef*>(data.data()), uInt(data.size())));
  EXPECT_THROW(zip::read_all(std::as_bytes(std::span(local.data(), local.size())), e), std::runtime_error);
}


// A KIT recorder whose clock ran late: rows move back by the offset at their stamp
// (linear between knots), rows before the first knot are dropped, and the single
// seconds the stretch leaves empty are filled from their neighbours.
TEST(EdrParser, ClockKnotsMoveRowsToUtc) {
  const i64 t0 = i64(utc("2019-04-01 00:00:00"));
  refs::EdrParser p({"2019-04"}, {}, {{t0 + 100, 20.0}, {t0 + 200, 30.0}});
  std::string csv = "Time;f50_EE;QI_EE\n";
  char line[64];
  for (int s = 0; s < 400; ++s) {
    std::snprintf(line, sizeof line, "2019-04-01 00:%02d:%02d;%d;0\n", s / 60, s % 60, s);
    csv += line;
  }
  p.feed(csv);
  auto v = p.finish().at("2019-04");
  EXPECT_TRUE(std::isnan(v[79]));                // stamp 99 is before the first knot: dropped
  EXPECT_FLOAT_EQ(v[80], 0.100f);                // stamp 100, 20 s late
  EXPECT_FLOAT_EQ(v[170], 0.200f);               // stamp 200, 30 s late
  EXPECT_FLOAT_EQ(v[369], 0.399f);               // held after the last knot
  for (int s = 80; s < 370; ++s) EXPECT_FALSE(std::isnan(v[std::size_t(s)])) << s;  // no holes left by the stretch
}
