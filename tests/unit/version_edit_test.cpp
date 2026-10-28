#include "db/version_edit.h"

#include <cstdint>

#include <gtest/gtest.h>

#include <random>
#include <string>
#include <string_view>

#include "forgedb/result.h"
#include "forgedb/status.h"

#include "db/internal_key.h"
#include "util/coding.h"

namespace forgedb {
namespace {

std::string Key(std::string_view user_key, SequenceNumber sequence) {
  std::string key;
  AppendInternalKey(key, {.user_key = user_key, .sequence = sequence, .type = ValueType::kValue});
  return key;
}

VersionEdit FullEdit() {
  VersionEdit edit;
  edit.log_number = 12;
  edit.next_file_number = 40;
  edit.last_sequence = 123456789;
  edit.removed_files.push_back({.level = 0, .number = 7});
  edit.removed_files.push_back({.level = 3, .number = 9});
  edit.added_files.push_back(
      {.level = 0,
       .file = {.number = 31, .file_size = 4096, .smallest = Key("a", 5), .largest = Key("m", 2)}});
  edit.added_files.push_back(
      {.level = 6,
       .file = {.number = 32, .file_size = 1, .smallest = Key("", 1), .largest = Key("", 1)}});
  return edit;
}

std::string Encode(const VersionEdit& edit) {
  std::string encoded;
  edit.EncodeTo(encoded);
  return encoded;
}

void ExpectMalformed(std::string_view encoded, std::string_view message_part) {
  const Result<VersionEdit> decoded = VersionEdit::Decode(encoded);
  ASSERT_FALSE(decoded.ok()) << message_part;
  EXPECT_EQ(decoded.status().code(), Status::Code::kCorruption);
  EXPECT_NE(decoded.status().ToString().find(message_part), std::string::npos)
      << decoded.status().ToString();
}

TEST(VersionEditTest, RoundTrip) {
  const VersionEdit edit = FullEdit();
  const Result<VersionEdit> decoded = VersionEdit::Decode(Encode(edit));
  ASSERT_TRUE(decoded.ok()) << decoded.status().ToString();
  EXPECT_EQ(*decoded, edit);
  EXPECT_TRUE(decoded->HasAllScalars());
}

TEST(VersionEditTest, EmptyEdit) {
  EXPECT_EQ(Encode(VersionEdit{}), "");
  const Result<VersionEdit> decoded = VersionEdit::Decode("");
  ASSERT_TRUE(decoded.ok());
  EXPECT_EQ(*decoded, VersionEdit{});
  EXPECT_FALSE(decoded->HasAllScalars());
}

// The example in docs/file-formats.md, "Version edit".
TEST(VersionEditTest, FormatIsStable) {
  VersionEdit edit;
  edit.log_number = 4;
  edit.next_file_number = 6;
  edit.last_sequence = 300;
  edit.added_files.push_back(
      {.level = 0,
       .file = {.number = 3, .file_size = 900, .smallest = Key("a", 1), .largest = Key("b", 2)}});
  using namespace std::string_literals;
  EXPECT_EQ(Encode(edit),
            "\x01\x04"
            "\x02\x06"
            "\x03\xac\x02"
            "\x05\x00\x03\x84\x07"
            "\x09"
            "a\x01\x01\x00\x00\x00\x00\x00\x00"
            "\x09"
            "b\x01\x02\x00\x00\x00\x00\x00\x00"s);
}

TEST(VersionEditTest, RejectsMalformedInput) {
  ExpectMalformed("\x09", "unknown tag 9");
  ExpectMalformed("\x80", "truncated tag");
  ExpectMalformed("\x01", "log number truncated");
  ExpectMalformed("\x01\x04\x01\x05", "log number repeated");
  ExpectMalformed("\x02\x80", "next file number truncated");
  {
    std::string encoded;
    PutVarint32(encoded, 3);
    PutVarint64(encoded, kMaxSequenceNumber + 1);
    ExpectMalformed(encoded, "last sequence beyond the maximum");
  }
  ExpectMalformed(std::string("\x04\x07\x01", 3), "level out of range");
  ExpectMalformed(std::string("\x04\x00", 2), "removed file truncated");
  {
    VersionEdit edit;
    edit.added_files.push_back(
        {.level = 0,
         .file = {.number = 1, .file_size = 1, .smallest = Key("z", 1), .largest = Key("a", 1)}});
    ExpectMalformed(Encode(edit), "smallest key is after its largest");
  }
  {
    VersionEdit edit;
    edit.added_files.push_back(
        {.level = 0, .file = {.number = 1, .file_size = 1, .smallest = "short", .largest = "x"}});
    ExpectMalformed(Encode(edit), "malformed key");
  }
  {
    VersionEdit edit;
    edit.added_files.push_back(
        {.level = kNumLevels,
         .file = {.number = 1, .file_size = 1, .smallest = Key("a", 1), .largest = Key("a", 1)}});
    ExpectMalformed(Encode(edit), "level out of range");
  }
}

// Every prefix and many random corruptions decode or fail cleanly (ASan
// builds check for out-of-bounds reads).
TEST(VersionEditTest, DamagedInputNeverCrashes) {
  const std::string valid = Encode(FullEdit());
  for (std::size_t n = 0; n < valid.size(); ++n) {
    const Result<VersionEdit> decoded = VersionEdit::Decode(std::string_view(valid).substr(0, n));
    EXPECT_TRUE(decoded.ok() || decoded.status().code() == Status::Code::kCorruption);
  }
  std::mt19937_64 random(3);
  for (int round = 0; round < 5000; ++round) {
    std::string damaged = valid;
    damaged[random() % damaged.size()] = static_cast<char>(random());
    const Result<VersionEdit> decoded = VersionEdit::Decode(damaged);
    EXPECT_TRUE(decoded.ok() || decoded.status().code() == Status::Code::kCorruption);
  }
}

}  // namespace
}  // namespace forgedb
