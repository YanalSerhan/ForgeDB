#include "db/filename.h"

#include <cstdint>
#include <filesystem>

#include <gtest/gtest.h>

#include <optional>
#include <string>
#include <string_view>

namespace forgedb {
namespace {

TEST(FileNameTest, Names) {
  EXPECT_EQ(LogFileName("db", 5).generic_string(), "db/000005.log");
  EXPECT_EQ(TableFileName("db", 7).generic_string(), "db/000007.sst");
  EXPECT_EQ(DescriptorFileName("db", 3).generic_string(), "db/MANIFEST-000003");
  EXPECT_EQ(CurrentFileName("db").generic_string(), "db/CURRENT");
  EXPECT_EQ(LockFileName("db").generic_string(), "db/LOCK");
  EXPECT_EQ(LogFileName("db", 1234567).filename().generic_string(), "1234567.log")
      << "more than six digits: no padding";
  EXPECT_EQ(LogFileName("db", UINT64_MAX).filename().generic_string(), "18446744073709551615.log");
}

TEST(FileNameTest, EveryNameParsesBack) {
  for (const std::uint64_t number :
       {std::uint64_t{0}, std::uint64_t{1}, std::uint64_t{5}, std::uint64_t{999999},
        std::uint64_t{1000000}, std::uint64_t{123456789}, UINT64_MAX}) {
    SCOPED_TRACE(number);
    EXPECT_EQ(ParseFileName(LogFileName("db", number).filename().string()),
              (ParsedFileName{.type = FileType::kLog, .number = number}));
    EXPECT_EQ(ParseFileName(TableFileName("db", number).filename().string()),
              (ParsedFileName{.type = FileType::kTable, .number = number}));
    EXPECT_EQ(ParseFileName(DescriptorFileName("db", number).filename().string()),
              (ParsedFileName{.type = FileType::kDescriptor, .number = number}));
  }
  EXPECT_EQ(ParseFileName("CURRENT"), (ParsedFileName{.type = FileType::kCurrent, .number = 0}));
  EXPECT_EQ(ParseFileName("LOCK"), (ParsedFileName{.type = FileType::kLock, .number = 0}));
}

// Directory entries are untrusted: only the exact forms above are accepted.
TEST(FileNameTest, AnythingElseIsNotADatabaseFile) {
  for (const std::string_view name : {
           "",
           "5.log",                      // too few digits
           "00005.log",                  // too few digits
           "0000005.log",                // more than six digits, but a leading zero
           "000005.LOG",                 // case matters
           "000005.log.tmp",             // suffix after the type
           "000005.",                    // no type
           ".log",                       // no number
           "000005.sst2",                // unknown type
           "x00005.log",                 // not a digit
           "-00005.log",                 // sign
           "+00005.log",                 // sign
           " 000005.log",                // space
           "000005 .log",                // space
           "18446744073709551616.log",   // 2^64: too big
           "99999999999999999999.log",   // too big
           "000000000000000000005.log",  // 21 digits
           "MANIFEST-",                  // no number
           "MANIFEST-5",                 // too few digits
           "MANIFEST-000005x",           // trailing junk
           "manifest-000005",            // case matters
           "CURRENT.tmp",                // not CURRENT
           "current",                    // case matters
           "LOCK2",                      // not LOCK
       }) {
    EXPECT_EQ(ParseFileName(name), std::nullopt) << "'" << name << "'";
  }
}

}  // namespace
}  // namespace forgedb
