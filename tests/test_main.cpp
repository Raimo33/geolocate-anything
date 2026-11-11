#include <gtest/gtest.h>

#include "helpers.hpp"

int main(int argc, char** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  // Never touch the user's real store.
  auto dir = geo::test::make_temp_dir("default");
  geo::test::use_cache(dir);
  int rc = RUN_ALL_TESTS();
  std::error_code ec;
  std::filesystem::remove_all(dir, ec);
  return rc;
}
