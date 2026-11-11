// AEMO's yearly frequency workbooks ("Mainland frequency data 2019", "Tasmania frequency
// data 2023", ... on aemo.com.au, Frequency and time deviation monitoring page): 4 s
// samples in Hz, NEM time (UTC+10, no DST). aemo.com.au refuses automated downloads, so
// these are downloaded by hand and imported with `geoloc refs import-aemo`.
//
// Layout (checked on the 2010..2025 files, 2026-11): sheet "Frequency data", one column
// per month headed by its 1st (an Excel date), and a "Time" column labelling each row
// with the day of the month and the time ("17 - 12:00:04"); 31 x 21600 rows. Also
// accepted: one column per day headed by its date, one date-time column and one value
// column (the weekly reports, CSV exports), times as Excel serial numbers or text, and
// the workbook inside a zip.
#pragma once

#include <map>
#include <span>
#include <string>
#include <vector>

#include "util/time.hpp"

namespace geo::refs {

struct AemoAnnual {
  std::map<std::string, std::vector<float>> months;  // UTC month -> 1 s deviations (Hz, NaN = none)
  std::string format, sheet, layout;                  // "xlsb", "Frequency data", "day columns" | "date-time column"
  std::size_t samples = 0, rejected = 0;              // values kept; values not a plausible frequency
  i64 first = 0, last = 0;                            // UTC seconds of the first and last sample
};

// utc_offset_s: the file's clock minus UTC (NEM time: +36000). shift_s is added after
// conversion (a measured logging delay).
AemoAnnual parse_aemo_annual(std::span<const std::byte> file, int utc_offset_s = 36000, int shift_s = 0);

// Grid a file is for, from its name: Tasmania files name it, everything else is mainland.
std::string aemo_annual_grid(const std::string& filename);

}  // namespace geo::refs
