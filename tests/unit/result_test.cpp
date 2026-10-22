#include "forgedb/result.h"

#include <gtest/gtest.h>

#include <memory>
#include <string>
#include <utility>

#include "forgedb/status.h"

namespace forgedb {
namespace {

TEST(ResultTest, HoldsValue) {
  const Result<int> r = 42;
  ASSERT_TRUE(r.ok());
  EXPECT_EQ(r.value(), 42);
  EXPECT_EQ(*r, 42);
  EXPECT_TRUE(r.status().ok());
}

TEST(ResultTest, HoldsError) {
  const Result<int> r = Status::NotFound("key");
  EXPECT_FALSE(r.ok());
  EXPECT_EQ(r.status(), Status::NotFound("key"));
}

TEST(ResultTest, ConvertsFromConvertibleValue) {
  // const char* -> std::string, the converting constructor at work.
  const Result<std::string> r = "hello";
  ASSERT_TRUE(r.ok());
  EXPECT_EQ(*r, "hello");
  EXPECT_EQ(r->size(), 5U);
}

TEST(ResultTest, SupportsMoveOnlyTypes) {
  Result<std::unique_ptr<int>> r = std::make_unique<int>(7);
  ASSERT_TRUE(r.ok());
  const std::unique_ptr<int> owned = std::move(r).value();
  ASSERT_NE(owned, nullptr);
  EXPECT_EQ(*owned, 7);
}

TEST(ResultTest, ModifiableThroughValue) {
  Result<std::string> r = std::string("ab");
  r.value() += "c";
  EXPECT_EQ(*r, "abc");
}

Result<int> Parse(bool fail) {
  if (fail) {
    return Status::InvalidArgument("not a number");
  }
  return 5;
}

Result<int> Doubled(bool fail) {
  FORGEDB_ASSIGN_OR_RETURN(const int value, Parse(fail));
  return value * 2;
}

TEST(ResultTest, AssignOrReturnPropagates) {
  const Result<int> good = Doubled(false);
  ASSERT_TRUE(good.ok());
  EXPECT_EQ(*good, 10);

  const Result<int> bad = Doubled(true);
  EXPECT_EQ(bad.status().code(), Status::Code::kInvalidArgument);
}

TEST(ResultDeathTest, ValueOfErrorTerminates) {
  // FORGEDB_CHECK is active in every build type.
  const Result<int> r = Status::Corruption("x");
  EXPECT_DEATH({ static_cast<void>(r.value()); }, "FORGEDB_CHECK failed: ok\\(\\)");
}

TEST(ResultDeathTest, OkStatusIsRejected) {
#ifdef FORGEDB_ENABLE_ASSERTS
  EXPECT_DEATH({ const Result<int> r = Status::OK(); }, "FORGEDB_CHECK failed");
#else
  // Without assertions an OK Status becomes an Internal error, never a value.
  const Result<int> r = Status::OK();
  EXPECT_EQ(r.status().code(), Status::Code::kInternal);
#endif
}

}  // namespace
}  // namespace forgedb
