// Build-wiring smoke test: proves the library, the generated header, the
// vcpkg GoogleTest package and ctest discovery all connect. Real unit tests
// start with the utilities in Phase 0.3.
#include "forgedb/version.h"

#include <gtest/gtest.h>

#include <string>

namespace forgedb {
namespace {

TEST(VersionTest, LibraryMatchesHeader) { EXPECT_EQ(VersionString(), kVersionString); }

TEST(VersionTest, StringIsMajorMinorPatch) {
  const std::string expected = std::to_string(kVersionMajor) + "." + std::to_string(kVersionMinor) +
                               "." + std::to_string(kVersionPatch);
  EXPECT_EQ(VersionString(), expected);
}

}  // namespace
}  // namespace forgedb
