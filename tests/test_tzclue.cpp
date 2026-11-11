// Time-zone clue: metadata clocks against the hum's UTC.
#include <gtest/gtest.h>

#include <algorithm>
#include <cstdlib>
#include <fstream>

#include "audio.hpp"
#include "helpers.hpp"
#include "tzclue.hpp"
#include "util/time.hpp"

using namespace geo;
using tzclue::Kind;

namespace {

double utc(const char* s) { return double(*parse_iso_utc(s)); }

audio::FileClues tags(std::initializer_list<std::pair<const char*, const char*>> kv) {
  audio::FileClues c;
  for (auto& [k, v] : kv) c.all_tags.emplace(k, v);
  return c;
}

bool has_zone(const tzclue::Result& r, const std::string& tz) {
  return std::any_of(r.zones.begin(), r.zones.end(), [&](auto& z) { return z.tz == tz; });
}

bool has_country(const tzclue::Result& r, const std::string& c) {
  for (auto& z : r.zones)
    if (std::find(z.countries.begin(), z.countries.end(), c) != z.countries.end()) return true;
  return false;
}

}  // namespace

TEST(TzClue, ParseStamp) {
  auto w = tzclue::parse_stamp("2024-05-03T14:22:11+0200");
  ASSERT_TRUE(w);
  EXPECT_TRUE(w->has_offset);
  EXPECT_EQ(w->offset_s, 7200);
  EXPECT_EQ(w->utc(), utc("2024-05-03T12:22:11Z"));

  w = tzclue::parse_stamp("2024:05:03 14:22:11+02:00");  // EXIF style
  ASSERT_TRUE(w);
  EXPECT_EQ(w->offset_s, 7200);

  w = tzclue::parse_stamp("Fri May  3 14:22:11 2024");  // asctime (AVI IDIT)
  ASSERT_TRUE(w);
  EXPECT_FALSE(w->has_offset);
  EXPECT_EQ(w->naive(), utc("2024-05-03 14:22:11"));

  w = tzclue::parse_stamp("2024-05-03 14-22-11");  // BWF with dashes
  ASSERT_TRUE(w);
  EXPECT_EQ(w->naive(), utc("2024-05-03 14:22:11"));

  EXPECT_TRUE(tzclue::parse_stamp("2024-05-03 14:22"));        // ID3v2.3 TYER+TDAT+TIME
  EXPECT_FALSE(tzclue::parse_stamp("2024-05-03"));             // no time of day
  EXPECT_FALSE(tzclue::parse_stamp("2024"));
  EXPECT_FALSE(tzclue::parse_stamp("yesterday at noon-ish"));
}

TEST(TzClue, ExplicitOffsetSummerEurope) {
  // An iPhone in May: UTC+2 is CEST, the core of Continental Europe.
  const double t = utc("2024-05-03T12:22:00Z");
  auto r = tzclue::infer(tags({{"com.apple.quicktime.creationdate", "2024-05-03T14:22:11+0200"}}), "continental_europe", t, 60);
  ASSERT_EQ(r.offsets, std::vector<int>{7200});
  ASSERT_EQ(r.evidence.size(), 1u);
  EXPECT_TRUE(r.evidence[0].used);
  EXPECT_NEAR(r.evidence[0].residual_s, 11, 1e-9);
  EXPECT_TRUE(has_zone(r, "Europe/Berlin"));
  EXPECT_TRUE(has_zone(r, "Europe/Paris"));
  EXPECT_FALSE(has_zone(r, "Europe/Athens"));   // UTC+3 in summer
  EXPECT_FALSE(has_zone(r, "Europe/Lisbon"));   // UTC+1 in summer
  EXPECT_FALSE(has_zone(r, "Europe/Istanbul"));  // UTC+3
  EXPECT_GT(r.km2, 0);
  EXPECT_LT(r.km2, r.grid_km2);
}

TEST(TzClue, LocalTimeWinterEurope) {
  // A field recorder's BWF stamp, two hours ahead of the hum in January: EET.
  const double t = utc("2024-01-15T08:00:00Z");
  auto r = tzclue::infer(tags({{"date", "2024-01-15"}, {"creation_time", "10:00:40"}}), "continental_europe", t, 600);
  ASSERT_EQ(r.offsets, std::vector<int>{7200});
  EXPECT_TRUE(has_country(r, "GR"));
  EXPECT_TRUE(has_country(r, "RO"));
  EXPECT_TRUE(has_country(r, "BG"));
  EXPECT_FALSE(has_country(r, "DE"));
  EXPECT_FALSE(has_country(r, "TR"));  // UTC+3 all year since 2016
  EXPECT_FALSE(has_zone(r, "Europe/Kaliningrad"));  // a separate grid: no border sliver may leak in
  // A clock set by hand that missed DST: CET zones show +2 in summer; Turkey and the
  // Maghreb have no DST and must not be offered.
  auto dst = std::find_if(r.notes.begin(), r.notes.end(), [](auto& n) { return n.find("missed the last DST change") != std::string::npos; });
  ASSERT_NE(dst, r.notes.end());
  EXPECT_NE(dst->find("DE"), std::string::npos);
  EXPECT_EQ(dst->find("TR"), std::string::npos);
  EXPECT_EQ(dst->find("DZ"), std::string::npos);
  EXPECT_EQ(dst->find("MA"), std::string::npos);
}

TEST(TzClue, UtcClockGivesNoClue) {
  const double t = utc("2024-01-15T08:00:00Z");
  auto r = tzclue::infer(tags({{"creation_time", "2024-01-15T08:00:03.000000Z"}}), "continental_europe", t, 600);
  EXPECT_TRUE(r.offsets.empty());
  EXPECT_TRUE(r.zones.empty());
  ASSERT_EQ(r.evidence.size(), 1u);
  EXPECT_FALSE(r.evidence[0].used);
  EXPECT_EQ(r.evidence[0].offset_s, 0);
}

TEST(TzClue, AustraliaSplitsByDst) {
  // January is summer: NSW/VIC/TAS +11, SA +10:30, Queensland (no DST) +10.
  const double t = utc("2024-01-15T02:00:00Z");
  auto r = tzclue::infer(tags({{"com.apple.quicktime.creationdate", "2024-01-15T12:00:00+1000"}}), "australia_nem", t, 60);
  ASSERT_EQ(r.offsets, std::vector<int>{36000});
  EXPECT_TRUE(has_zone(r, "Australia/Brisbane"));
  EXPECT_FALSE(has_zone(r, "Australia/Sydney"));
  EXPECT_FALSE(has_zone(r, "Australia/Adelaide"));
  // July: no DST anywhere, NSW/VIC/QLD all +10.
  const double w = utc("2024-07-15T02:00:00Z");
  auto r2 = tzclue::infer(tags({{"com.apple.quicktime.creationdate", "2024-07-15T12:00:00+1000"}}), "australia_nem", w, 60);
  EXPECT_TRUE(has_zone(r2, "Australia/Brisbane"));
  EXPECT_TRUE(has_zone(r2, "Australia/Sydney"));
  EXPECT_FALSE(has_zone(r2, "Australia/Adelaide"));  // +9:30
}

// ---------------------------------------------------------------- real files

namespace {
bool have_ffmpeg() { return std::system("ffmpeg -version >/dev/null 2>&1") == 0; }

std::string make(const test::fs::path& dir, const std::string& name, const std::string& args) {
  const std::string out = (dir / name).string();
  const std::string cmd = "ffmpeg -y -loglevel error -f lavfi -i sine=f=50:d=1 " + args + " '" + out + "' >/dev/null 2>&1";
  return std::system(cmd.c_str()) == 0 ? out : std::string();
}
}  // namespace

TEST(TzClue, ContainerFormats) {
  if (!have_ffmpeg()) GTEST_SKIP() << "ffmpeg not installed";
  auto dir = test::make_temp_dir("tzclue");
  struct Case {
    const char *name, *args, *key;
    Kind kind;
  };
  const Case cases[] = {
      {"a.mov", "-movflags use_metadata_tags -metadata com.apple.quicktime.creationdate=2024-05-03T14:22:11+0200",
       "com.apple.quicktime.creationdate", Kind::Explicit},
      {"a.mp4", "-metadata date=2024-05-03T14:22:11+0200", "date", Kind::Explicit},
      {"b.mp4", "-metadata creation_time=2024-05-03T14:22:11Z", "creation_time", Kind::Utc},
      {"a.mkv", "-metadata DATE_RECORDED=\"2024-05-03 14:22:11\"", "date_recorded", Kind::Local},
      {"b.mkv", "-metadata creation_time=2024-05-03T14:22:11Z", "creation_time", Kind::Utc},
      {"a.wav", "-write_bext 1 -metadata origination_date=2024-05-03 -metadata origination_time=14:22:11", "bwf:origination", Kind::Local},
      {"a.mp3", "-metadata date=2024-05-03T14:22:11", "date", Kind::Local},
      {"a.flac", "-metadata DATE=2024-05-03T14:22:11+02:00", "date", Kind::Explicit},
      {"a.ogg", "-c:a libvorbis -metadata DATE=2024-05-03T14:22:11", "date", Kind::Local},
      {"a.m4a", "-metadata date=2024-05-03T14:22:11+0200", "date", Kind::Explicit},
  };
  for (auto& c : cases) {
    auto path = make(dir, c.name, c.args);
    if (path.empty()) {
      ADD_FAILURE() << c.name << ": ffmpeg could not write it";
      continue;
    }
    auto st = tzclue::stamps(audio::file_clues(path));
    auto it = std::find_if(st.begin(), st.end(), [&](auto& s) { return s.key == c.key; });
    ASSERT_NE(it, st.end()) << c.name;
    EXPECT_EQ(it->kind, c.kind) << c.name;
    EXPECT_EQ(it->w.naive(), utc("2024-05-03 14:22:11")) << c.name;
  }
  std::error_code ec;
  test::fs::remove_all(dir, ec);
}

