#pragma once

#include <string_view>

namespace geo::data {
std::string_view grids_json();
std::string_view grid_areas_json();
std::string_view tz_areas_json();  // grid areas cut along time-zone borders (scripts/build_tz_areas.py)
std::string_view dashboard_html();
}  // namespace geo::data
