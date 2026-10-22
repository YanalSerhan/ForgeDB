#include "forgedb/status.h"

#include <gtest/gtest.h>

#include <string>
#include <utility>

namespace forgedb {
namespace {

TEST(StatusTest, DefaultIsOk) {
  const Status s;
  EXPECT_TRUE(s.ok());
  EXPECT_EQ(s.code(), Status::Code::kOk);
  EXPECT_TRUE(s.message().empty());
  EXPECT_EQ(s.ToString(), "OK");
  EXPECT_EQ(s, Status::OK());
}

TEST(StatusTest, FactoriesSetCode) {
  EXPECT_EQ(Status::NotFound("x").code(), Status::Code::kNotFound);
  EXPECT_EQ(Status::Corruption("x").code(), Status::Code::kCorruption);
  EXPECT_EQ(Status::NotSupported("x").code(), Status::Code::kNotSupported);
  EXPECT_EQ(Status::InvalidArgument("x").code(), Status::Code::kInvalidArgument);
  EXPECT_EQ(Status::IOError("x").code(), Status::Code::kIOError);
  EXPECT_EQ(Status::Busy("x").code(), Status::Code::kBusy);
  EXPECT_EQ(Status::Internal("x").code(), Status::Code::kInternal);
  EXPECT_FALSE(Status::NotFound("x").ok());
}

TEST(StatusTest, MessageJoinsDetail) {
  EXPECT_EQ(Status::IOError("C:\\db\\LOCK").message(), "C:\\db\\LOCK");
  EXPECT_EQ(Status::IOError("C:\\db\\LOCK", "Access is denied.").message(),
            "C:\\db\\LOCK: Access is denied.");
}

TEST(StatusTest, ToStringNamesTheCode) {
  EXPECT_EQ(Status::Corruption("bad block checksum").ToString(), "Corruption: bad block checksum");
  EXPECT_EQ(Status::NotFound("").ToString(), "NotFound");
}

TEST(StatusTest, CodeNamesAreDistinct) {
  EXPECT_EQ(CodeName(Status::Code::kOk), "OK");
  EXPECT_EQ(CodeName(Status::Code::kIOError), "IOError");
  EXPECT_EQ(CodeName(Status::Code::kInternal), "Internal");
}

TEST(StatusTest, CopyAndMovePreserveContents) {
  const Status original = Status::Busy("LOCK held by another process");
  // The copy is the point of this test.
  // NOLINTNEXTLINE(performance-unnecessary-copy-initialization)
  const Status copy = original;
  EXPECT_EQ(copy, original);

  Status source = Status::Busy("LOCK held by another process");
  const Status moved = std::move(source);
  EXPECT_EQ(moved, original);
}

TEST(StatusTest, EqualityComparesCodeAndMessage) {
  EXPECT_EQ(Status::NotFound("a"), Status::NotFound("a"));
  EXPECT_NE(Status::NotFound("a"), Status::NotFound("b"));
  EXPECT_NE(Status::NotFound("a"), Status::Corruption("a"));
}

Status FailIf(bool fail) { return fail ? Status::IOError("failed") : Status::OK(); }

Status TwoSteps(bool fail_first, bool fail_second, int& steps_done) {
  FORGEDB_RETURN_IF_ERROR(FailIf(fail_first));
  ++steps_done;
  FORGEDB_RETURN_IF_ERROR(FailIf(fail_second));
  ++steps_done;
  return Status::OK();
}

TEST(StatusTest, ReturnIfErrorStopsAtFirstError) {
  int steps = 0;
  EXPECT_TRUE(TwoSteps(false, false, steps).ok());
  EXPECT_EQ(steps, 2);

  steps = 0;
  EXPECT_EQ(TwoSteps(true, false, steps).code(), Status::Code::kIOError);
  EXPECT_EQ(steps, 0);

  steps = 0;
  EXPECT_EQ(TwoSteps(false, true, steps).code(), Status::Code::kIOError);
  EXPECT_EQ(steps, 1);
}

}  // namespace
}  // namespace forgedb
