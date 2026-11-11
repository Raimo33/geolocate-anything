// Cells of a spreadsheet: Excel binary (.xlsb), Office Open XML (.xlsx), Excel 97-2003
// (.xls, BIFF8) or CSV, from
// memory. Only what reading data tables needs: numbers and strings by (row, column),
// both 0-based, in file order. Formats, styles and formulas' text are ignored; a
// formula cell yields its cached value. AEMO publishes its yearly frequency data as
// .xlsb workbooks; ERCOT its 2012 frequency data as .xls.
#pragma once

#include <cmath>
#include <cstdint>
#include <functional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace geo::refs::sheets {

struct Cell {
  double num = NAN;  // set for numbers (dates and times are Excel serial numbers)
  std::string str;   // set for strings
  bool is_num() const { return std::isfinite(num); }
};
using CellFn = std::function<void(std::uint32_t row, std::uint32_t col, const Cell&)>;

struct Workbook {
  std::string format;               // "xlsb", "xlsx", "xls" or "csv"
  std::vector<std::string> sheets;  // names, in workbook order (one unnamed sheet for CSV)
};

// What a file holds. Accepts a workbook, CSV text, or a zip holding one of those.
Workbook inspect(std::span<const std::byte> file);
// Every cell of a sheet, chosen as the first of `prefer` that a sheet name equals
// (case- and space-insensitive), else the first sheet. Returns the sheet read.
std::string read(std::span<const std::byte> file, const std::vector<std::string>& prefer, const CellFn& fn);

// "A1"-style column letters to a 0-based index ("A" = 0, "AB" = 27).
std::uint32_t column_index(std::string_view letters);

}  // namespace geo::refs::sheets
