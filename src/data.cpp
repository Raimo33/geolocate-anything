// Data files compiled into the binary (#embed), so `geoloc` is one self-contained file.
#include "data.hpp"

namespace geo::data {

namespace {
constexpr unsigned char grids_json_bytes[] = {
#embed "../data/grids.json"
};
constexpr unsigned char grid_areas_bytes[] = {
#embed "../data/grid_areas.json"
};
constexpr unsigned char tz_areas_bytes[] = {
#embed "../data/tz_areas.json"
};
constexpr unsigned char dashboard_bytes[] = {
#embed "../data/dashboard.html"
};
}  // namespace

std::string_view grids_json() { return {reinterpret_cast<const char*>(grids_json_bytes), sizeof grids_json_bytes}; }
std::string_view grid_areas_json() { return {reinterpret_cast<const char*>(grid_areas_bytes), sizeof grid_areas_bytes}; }
std::string_view tz_areas_json() { return {reinterpret_cast<const char*>(tz_areas_bytes), sizeof tz_areas_bytes}; }
std::string_view dashboard_html() { return {reinterpret_cast<const char*>(dashboard_bytes), sizeof dashboard_bytes}; }

}  // namespace geo::data
