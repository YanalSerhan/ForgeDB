#include "forgedb/check.h"

#include <gtest/gtest.h>

namespace forgedb {
namespace {

// Returns false and records that it ran, to observe whether a check evaluated it.
[[maybe_unused]] bool CountEvaluation(int& counter) {
  ++counter;
  return false;
}

TEST(CheckTest, PassingCheckDoesNothing) {
  FORGEDB_CHECK(1 + 1 == 2);
  FORGEDB_DCHECK(1 + 1 == 2);
}

TEST(CheckDeathTest, FailingCheckReportsExpressionAndExits) {
  // The exit code is 3; the message names the failed expression.
  EXPECT_EXIT(FORGEDB_CHECK(1 + 1 == 3), ::testing::ExitedWithCode(3),
              "FORGEDB_CHECK failed: 1 \\+ 1 == 3");
}

TEST(CheckDeathTest, DcheckFollowsBuildConfiguration) {
#ifdef FORGEDB_ENABLE_ASSERTS
  EXPECT_DEATH(FORGEDB_DCHECK(false), "FORGEDB_CHECK failed: false");
#else
  int evaluations = 0;
  FORGEDB_DCHECK(CountEvaluation(evaluations));  // disabled: compiled, never evaluated
  EXPECT_EQ(evaluations, 0);
#endif
}

}  // namespace
}  // namespace forgedb
