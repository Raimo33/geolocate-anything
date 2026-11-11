#include <set>

#include "data.hpp"
#include "grids.hpp"
#include "helpers.hpp"
#include "refs/sources.hpp"
#include "util/json.hpp"

using namespace geo;

TEST(Grids, Schema) {
  auto raw = parse_json(data::grids_json());
  EXPECT_EQ(jint(raw, "version"), 2);
  std::set<std::string> ids;
  for (auto& g : raw.at("grids").get_array()) {
    std::string id = jstr(g, "id");
    EXPECT_TRUE(ids.insert(id).second) << id;
    double nom = jnum(g, "nominal_hz");
    EXPECT_TRUE(nom == 50 || nom == 60) << id;
    std::string ref = jstr(g, "reference", "tracked");
    EXPECT_TRUE(ref == "tracked" || ref == "none") << id;
    ASSERT_TRUE(jget(g, "areas") && !g.at("areas").get_array().empty()) << id;
    for (auto& a : g.at("areas").get_array()) {
      EXPECT_FALSE(a.at("include").get_array().empty()) << id;
      std::string st = jstr(a, "status");
      EXPECT_TRUE(st == "synchronous" || st == "dc_linked" || st == "isolated") << id;
      EXPECT_FALSE(jstr(a, "source").empty()) << id << ": every area cites its source";
      std::string from = jstr(a, "from"), to = jstr(a, "to");
      if (!from.empty()) EXPECT_TRUE(parse_iso_utc(from)) << id;
      if (!to.empty()) EXPECT_TRUE(parse_iso_utc(to)) << id;
      if (!from.empty() && !to.empty()) EXPECT_LT(from, to) << id;
    }
  }
}

TEST(Grids, EverySourceGridExistsAndReferenceFlagMatchesSources) {
  const auto& t = grids::load_grids();
  std::set<std::string> covered;
  for (auto& s : refs::sources())
    for (auto& g : s.grids) covered.insert(g);
  for (auto& g : covered) EXPECT_TRUE(t.find(g)) << g;
  for (auto& g : t.grids) EXPECT_EQ(g.tracked(), covered.count(g.id) > 0) << g.id;
}

TEST(Grids, AreasFileMatchesTable) {
  // grid_areas.json is built from grids.json: one feature per area, same dates and status.
  auto raw = parse_json(data::grids_json());
  auto areas = parse_json(data::grid_areas_json());
  std::multiset<std::string> want, got;
  for (auto& g : raw.at("grids").get_array())
    for (auto& a : g.at("areas").get_array())
      want.insert(jstr(g, "id") + "|" + jstr(a, "from") + "|" + jstr(a, "to") + "|" + jstr(a, "status"));
  for (auto& f : areas.at("features").get_array()) {
    const auto& p = f.at("properties");
    got.insert(jstr(p, "grid") + "|" + jstr(p, "from") + "|" + jstr(p, "to") + "|" + jstr(p, "status"));
  }
  EXPECT_EQ(got, want) << "grid_areas.json is stale";
}

TEST(Grids, CountriesAreDerivedAndDated) {
  const auto& t = grids::load_grids();
  auto has = [](const std::vector<std::string>& v, const char* c) { return std::find(v.begin(), v.end(), c) != v.end(); };
  const auto& ce = t.get("continental_europe");
  EXPECT_TRUE(has(ce.countries_at("2026-01-01"), "EE"));
  EXPECT_FALSE(has(ce.countries_at("2024-01-01"), "EE"));
  EXPECT_TRUE(has(t.get("ips_ups").countries_at("2024-06-01"), "EE"));
  EXPECT_FALSE(has(t.get("ips_ups").countries_at("2026-01-01"), "EE"));
  EXPECT_TRUE(has(ce.countries_at("2022-03-16"), "UA"));
  EXPECT_FALSE(has(ce.countries_at("2022-03-15"), "UA"));
  EXPECT_TRUE(has(ce.countries(), "DK-81"));
  EXPECT_TRUE(t.get("kaliningrad").countries_at("2024-01-01").empty());
  EXPECT_FALSE(t.get("baltic_island_2025").exists_at("2025-02-10"));
  EXPECT_EQ(t.get("shetland").status_at("2020-01-01"), "isolated");
  EXPECT_EQ(t.get("shetland").status_at("2025-01-01"), "dc_linked");
}

struct Point {
  double lat, lon;
  const char* when;
  const char* grid;
};
class KnownPoints : public ::testing::TestWithParam<Point> {};

TEST_P(KnownPoints, Placement) {
  auto p = GetParam();
  auto hits = grids::grids_at(p.lat, p.lon, p.when ? p.when : "");
  std::optional<std::string> got = hits.empty() ? std::nullopt : hits[0].grid;
  if (p.grid) EXPECT_EQ(got, std::string(p.grid)) << p.lat << "," << p.lon;
  else EXPECT_FALSE(got) << p.lat << "," << p.lon;
}

INSTANTIATE_TEST_SUITE_P(
    Places, KnownPoints,
    ::testing::Values(Point{35.17, 33.36, nullptr, "cyprus"}, Point{41.01, 28.98, "2012-01-01", "continental_europe"},
                      Point{39.57, 2.65, nullptr, "balearic"}, Point{38.91, 1.43, "2014-01-01", "ibiza_formentera"},
                      Point{38.91, 1.43, "2018-01-01", "balearic"}, Point{28.12, -15.43, nullptr, "gran_canaria"},
                      Point{28.47, -16.25, nullptr, "canary_islands_other"}, Point{32.65, -16.91, nullptr, "azores_madeira"},
                      Point{35.89, -5.32, nullptr, "ceuta_melilla"}, Point{33.57, -7.59, nullptr, "continental_europe"},
                      Point{62.01, -6.77, nullptr, "faroe"}, Point{61.55, -6.81, nullptr, "faroe_other"}, Point{64.15, -21.94, nullptr, "iceland"},
                      Point{35.68, 139.69, nullptr, "japan_east"}, Point{34.69, 135.50, nullptr, "japan_west"},
                      Point{35.10, 138.86, nullptr, "japan_east"}, Point{34.98, 138.38, nullptr, "japan_west"},
                      Point{37.92, 139.04, nullptr, "japan_east"}, Point{43.06, 141.35, nullptr, "hokkaido"}, Point{26.21, 127.68, nullptr, "okinawa"},
                      Point{38.02, 138.37, nullptr, "japan_islands_isolated"}, Point{37.57, 126.98, nullptr, "south_korea"},
                      Point{33.50, 126.53, nullptr, "jeju"}, Point{28.61, 77.21, nullptr, "india"}, Point{13.08, 80.27, "2013-06-01", "india_south"},
                      Point{13.08, 80.27, "2015-06-01", "india"}, Point{27.72, 85.32, nullptr, "india"},
                      Point{3.14, 101.69, nullptr, "malaysia_peninsular"}, Point{1.35, 103.82, nullptr, "malaysia_peninsular"},
                      Point{5.98, 116.07, nullptr, nullptr}, Point{-26.20, 28.05, nullptr, "southern_africa"},
                      Point{-25.97, 32.57, nullptr, "southern_africa"}, Point{-11.66, 27.48, nullptr, "southern_africa"}));

TEST(Placement, EverySourceKnowsWhereItMeasures) {
  for (auto& s : refs::sources())
    for (auto& g : s.grids) EXPECT_FALSE(refs::recorder_locations(s.name, g, nullptr).empty()) << s.name << "/" << g;
}

using AuditStore = test::TempStore;
