#include <algorithm>
#include <cstddef>
#include <cstdint>

#include <gtest/gtest.h>

#include <memory>
#include <optional>
#include <random>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "forgedb/result.h"
#include "forgedb/status.h"

#include "env/file_system.h"
#include "env/mem_file_system.h"
#include "log/log_format.h"
#include "log/log_reader.h"
#include "log/log_writer.h"
#include "util/coding.h"
#include "util/crc32c.h"

namespace forgedb::log {
namespace {

// Everything a reader produced from one log.
struct ReadResult {
  std::vector<std::string> records;
  std::vector<std::uint64_t> offsets;  // LastRecordOffset() after each record
  Status status;                       // OK, or the error that ended reading
  std::optional<Reader::TornTail> tail;
};

class LogTest : public testing::Test {
 protected:
  // The bytes of a new log holding `records`.
  std::string WriteLog(const std::vector<std::string>& records) {
    const std::unique_ptr<WritableFile> file = std::move(fs_.NewWritableFile("log")).value();
    Writer writer(file.get());
    for (const std::string& record : records) {
      EXPECT_TRUE(writer.AddRecord(record).ok());
    }
    EXPECT_TRUE(file->Close().ok());
    std::string contents = FileContents("log");
    // Counted exactly, padding and headers included (DBStats::log_bytes).
    EXPECT_EQ(writer.BytesWritten(), contents.size());
    return contents;
  }

  // Reads every record from a log consisting of `bytes`.
  ReadResult ReadLog(std::string_view bytes) {
    {
      const std::unique_ptr<WritableFile> file = std::move(fs_.NewWritableFile("input")).value();
      EXPECT_TRUE(file->Append(bytes).ok());
      EXPECT_TRUE(file->Close().ok());
    }
    Reader reader(std::move(fs_.NewSequentialFile("input")).value());
    ReadResult result;
    while (true) {
      Result<std::optional<std::string_view>> record = reader.ReadRecord();
      if (!record.ok()) {
        result.status = record.status();
        // Errors are sticky.
        EXPECT_EQ(reader.ReadRecord().status(), result.status);
        break;
      }
      if (!record->has_value()) {
        EXPECT_FALSE(reader.ReadRecord().value().has_value()) << "the end is sticky too";
        break;
      }
      result.records.emplace_back(**record);
      result.offsets.push_back(reader.LastRecordOffset());
    }
    result.tail = reader.IgnoredTail();
    return result;
  }

  std::string FileContents(const std::string& name) {
    const std::unique_ptr<SequentialFile> file = std::move(fs_.NewSequentialFile(name)).value();
    std::string contents;
    std::string buffer(kBlockSize, '\0');
    while (true) {
      const std::size_t read = std::move(file->Read(std::span<char>(buffer))).value();
      contents.append(buffer, 0, read);
      if (read == 0) {
        return contents;
      }
    }
  }

  MemFileSystem fs_;
};

// One fragment with a correct header, for hand-built logs. Takes the type as
// a raw byte so tests can also write types that don't exist.
std::string FragmentWithTypeByte(std::uint8_t type, std::string_view payload) {
  const auto type_byte = static_cast<char>(type);
  const std::uint32_t crc = crc32c::Extend(crc32c::Value(std::string_view(&type_byte, 1)), payload);
  std::string fragment(kHeaderSize, '\0');
  EncodeFixed32(fragment.data(), crc32c::Mask(crc));
  fragment[4] = static_cast<char>(payload.size() & 0xFFU);
  fragment[5] = static_cast<char>(payload.size() >> 8U);
  fragment[6] = type_byte;
  return fragment.append(payload);
}

std::string Fragment(RecordType type, std::string_view payload) {
  return FragmentWithTypeByte(static_cast<std::uint8_t>(type), payload);
}

std::optional<std::uint64_t> TailOffset(const ReadResult& result) {
  if (!result.tail) {
    return std::nullopt;
  }
  return result.tail->offset;
}

std::string RandomBytes(std::size_t size, std::uint64_t seed) {
  std::mt19937_64 rng(seed);
  std::string bytes(size, '\0');
  for (char& c : bytes) {
    c = static_cast<char>(rng());
  }
  return bytes;
}

bool IsCorruption(const Status& status) { return status.code() == Status::Code::kCorruption; }

// --- Writing and reading back ---------------------------------------------------

TEST_F(LogTest, EmptyLog) {
  const ReadResult result = ReadLog("");
  EXPECT_TRUE(result.status.ok());
  EXPECT_TRUE(result.records.empty());
  EXPECT_FALSE(result.tail.has_value());
}

TEST_F(LogTest, ByteLayout) {
  const std::string bytes = WriteLog({"abc"});
  ASSERT_EQ(bytes.size(), kHeaderSize + 3);
  const std::uint32_t crc =
      crc32c::Value(std::string_view("\x01"
                                     "abc",
                                     4));
  EXPECT_EQ(DecodeFixed32(bytes.data()), crc32c::Mask(crc));
  EXPECT_EQ(bytes.substr(4), std::string("\x03\x00\x01"
                                         "abc",
                                         6));
}

TEST_F(LogTest, RecordsOfEverySizeClassRoundTrip) {
  constexpr std::size_t kMaxInBlock = kBlockSize - kHeaderSize;
  const std::vector<std::size_t> sizes = {0,
                                          1,
                                          100,
                                          kMaxInBlock - 1,
                                          kMaxInBlock,
                                          kMaxInBlock + 1,
                                          kBlockSize,
                                          2 * kBlockSize,
                                          (3 * kBlockSize) + 7,
                                          std::size_t{1} << 20U,
                                          0,
                                          5};
  std::vector<std::string> records;
  records.reserve(sizes.size());
  for (std::size_t i = 0; i < sizes.size(); ++i) {
    records.push_back(RandomBytes(sizes[i], i));
  }
  const ReadResult result = ReadLog(WriteLog(records));
  EXPECT_TRUE(result.status.ok()) << result.status.ToString();
  EXPECT_FALSE(result.tail.has_value());
  ASSERT_EQ(result.records.size(), records.size());
  for (std::size_t i = 0; i < records.size(); ++i) {
    EXPECT_EQ(result.records[i], records[i]) << "record " << i << " of size " << sizes[i];
  }
}

TEST_F(LogTest, ManyRandomRecords) {
  std::mt19937_64 rng(7);
  std::vector<std::string> records;
  for (int i = 0; i < 500; ++i) {
    const std::size_t size = (rng() % 10 == 0) ? rng() % 70000 : rng() % 300;
    records.push_back(RandomBytes(size, rng()));
  }
  const ReadResult result = ReadLog(WriteLog(records));
  EXPECT_TRUE(result.status.ok());
  EXPECT_EQ(result.records, records);
}

// A record that leaves fewer than a header's worth of bytes in its block:
// the rest is zero-filled and the next record starts at the next block.
TEST_F(LogTest, BlockTailsTooSmallForAHeaderArePadded) {
  for (std::size_t leftover = 0; leftover < kHeaderSize; ++leftover) {
    SCOPED_TRACE(leftover);
    const std::string first(kBlockSize - kHeaderSize - leftover, 'a');
    const std::string bytes = WriteLog({first, "next"});
    EXPECT_EQ(bytes.size(), kBlockSize + kHeaderSize + 4);
    EXPECT_EQ(bytes.substr(kBlockSize - leftover, leftover), std::string(leftover, '\0'));
    const ReadResult result = ReadLog(bytes);
    EXPECT_TRUE(result.status.ok());
    EXPECT_EQ(result.records, std::vector<std::string>({first, "next"}));
    ASSERT_EQ(result.offsets.size(), 2U);
    EXPECT_EQ(result.offsets[1], kBlockSize);
  }
}

// Exactly a header's worth left: a record starts there with an empty FIRST
// fragment (or, if the record is empty, fits there as a FULL fragment).
TEST_F(LogTest, ExactlyAHeaderLeftInABlock) {
  const std::string first(kBlockSize - (2 * kHeaderSize), 'a');
  {
    const std::string bytes = WriteLog({first, "next"});
    EXPECT_EQ(bytes.size(), kBlockSize + kHeaderSize + 4);
    EXPECT_EQ(static_cast<RecordType>(bytes[kBlockSize - 1]), RecordType::kFirst);
    const ReadResult result = ReadLog(bytes);
    EXPECT_EQ(result.records, std::vector<std::string>({first, "next"}));
    ASSERT_EQ(result.offsets.size(), 2U);
    EXPECT_EQ(result.offsets[1], kBlockSize - kHeaderSize)
        << "the record starts in the first block";
  }
  {
    const std::string bytes = WriteLog({first, ""});
    EXPECT_EQ(bytes.size(), kBlockSize);
    EXPECT_EQ(ReadLog(bytes).records, std::vector<std::string>({first, ""}));
  }
}

// --- Torn tails: what a crash can leave -----------------------------------------------

// Cutting the log at any byte must give back exactly the records that were
// complete before the cut, with no error.
TEST_F(LogTest, EveryTruncationIsATornTail) {
  const std::vector<std::string> records = {
      std::string(10, 'a'), std::string(kBlockSize - kHeaderSize, 'b'),  // fills a block exactly
      std::string(5, 'c'),  RandomBytes(40000, 1),                       // spans blocks
      std::string(3, 'd'),  std::string(),                               // empty
      std::string(1, 'f')};
  // The end offset of each record: the size of a log holding it and those before.
  std::vector<std::size_t> ends;
  for (std::size_t i = 1; i <= records.size(); ++i) {
    ends.push_back(
        WriteLog({records.begin(), records.begin() + static_cast<std::ptrdiff_t>(i)}).size());
  }
  const std::string full = WriteLog(records);
  ASSERT_EQ(full.size(), ends.back());

  for (std::size_t cut = 0; cut <= full.size(); ++cut) {
    // Every byte near a record end or block boundary; elsewhere every 61st.
    const bool interesting =
        std::ranges::any_of(
            ends, [cut](std::size_t end) { return cut + 20 >= end && cut <= end + 20; }) ||
        (cut % kBlockSize) < 20 || (cut % kBlockSize) > kBlockSize - 20;
    if (!interesting && cut % 61 != 0) {
      continue;
    }
    const ReadResult result = ReadLog(std::string_view(full).substr(0, cut));
    ASSERT_TRUE(result.status.ok()) << "cut " << cut << ": " << result.status.ToString();
    const auto complete = static_cast<std::size_t>(
        std::ranges::count_if(ends, [cut](std::size_t end) { return end <= cut; }));
    ASSERT_EQ(result.records.size(), complete) << "cut " << cut;
    for (std::size_t i = 0; i < complete; ++i) {
      ASSERT_EQ(result.records[i], records[i]) << "cut " << cut << ", record " << i;
    }
    const bool at_record_end = cut == 0 || std::ranges::find(ends, cut) != ends.end();
    EXPECT_EQ(result.tail.has_value(), !at_record_end) << "cut " << cut;
  }
}

TEST_F(LogTest, ZeroFilledTailIsATornTail) {
  const std::string log = WriteLog({"one", "two"});
  for (const std::size_t zeros : {std::size_t{1}, kHeaderSize, std::size_t{100}, 2 * kBlockSize}) {
    SCOPED_TRACE(zeros);
    const ReadResult result = ReadLog(log + std::string(zeros, '\0'));
    EXPECT_TRUE(result.status.ok()) << result.status.ToString();
    EXPECT_EQ(result.records, std::vector<std::string>({"one", "two"}));
    EXPECT_EQ(TailOffset(result), std::optional<std::uint64_t>(log.size()));
  }
}

TEST_F(LogTest, GarbageTailIsATornTail) {
  const std::string log = WriteLog({"one", "two"});
  for (const std::uint64_t seed : {1U, 2U, 3U}) {
    const ReadResult result = ReadLog(log + RandomBytes(3000, seed));
    EXPECT_TRUE(result.status.ok()) << result.status.ToString();
    EXPECT_EQ(result.records, std::vector<std::string>({"one", "two"}));
    EXPECT_TRUE(result.tail.has_value());
  }
}

TEST_F(LogTest, UnfinishedMultiFragmentRecordIsATornTail) {
  const std::string bytes = Fragment(RecordType::kFull, "a") + Fragment(RecordType::kFirst, "b") +
                            Fragment(RecordType::kMiddle, "c");
  const ReadResult result = ReadLog(bytes);
  EXPECT_TRUE(result.status.ok());
  EXPECT_EQ(result.records, std::vector<std::string>({"a"}));
  EXPECT_EQ(TailOffset(result), std::optional<std::uint64_t>(kHeaderSize + 1))
      << "the tail starts at the unfinished record";
}

// --- Mid-log corruption: damage followed by valid data -------------------------------

TEST_F(LogTest, FragmentsAreReassembledInOrder) {
  const ReadResult result =
      ReadLog(Fragment(RecordType::kFirst, "a") + Fragment(RecordType::kMiddle, "b") +
              Fragment(RecordType::kLast, "c"));
  EXPECT_TRUE(result.status.ok());
  EXPECT_EQ(result.records, std::vector<std::string>({"abc"}));
}

TEST_F(LogTest, FragmentsOutOfOrderAreCorruption) {
  const std::vector<std::string> logs = {
      Fragment(RecordType::kMiddle, "x"),
      Fragment(RecordType::kLast, "x"),
      Fragment(RecordType::kFirst, "a") + Fragment(RecordType::kFull, "b"),
      Fragment(RecordType::kFirst, "a") + Fragment(RecordType::kFirst, "b") +
          Fragment(RecordType::kLast, "c"),
  };
  for (const std::string& log : logs) {
    const ReadResult result = ReadLog(log);
    EXPECT_TRUE(IsCorruption(result.status)) << result.status.ToString();
    EXPECT_TRUE(result.records.empty());
  }
}

TEST_F(LogTest, UnknownTypeIsCorruptionOnlyIfValidDataFollows) {
  const std::string unknown = FragmentWithTypeByte(9, "x");
  const ReadResult at_end = ReadLog(Fragment(RecordType::kFull, "a") + unknown);
  EXPECT_TRUE(at_end.status.ok());
  EXPECT_EQ(at_end.records, std::vector<std::string>({"a"}));
  EXPECT_TRUE(at_end.tail.has_value());

  const ReadResult in_middle = ReadLog(unknown + Fragment(RecordType::kFull, "b"));
  EXPECT_TRUE(IsCorruption(in_middle.status)) << in_middle.status.ToString();
  EXPECT_NE(in_middle.status.message().find("unknown fragment type 9"), std::string::npos);
}

// Flip one bit at every byte of a small log. Damage inside the last record is
// a torn tail; damage anywhere earlier is corruption, because the records
// after it are still valid. A damaged record is never returned.
TEST_F(LogTest, EverySingleBitFlipIsDetected) {
  const std::vector<std::string> records = {"first record", "second", "the third and last record"};
  std::vector<std::size_t> ends;
  for (std::size_t i = 1; i <= records.size(); ++i) {
    ends.push_back(
        WriteLog({records.begin(), records.begin() + static_cast<std::ptrdiff_t>(i)}).size());
  }
  const std::string log = WriteLog(records);
  for (std::size_t byte = 0; byte < log.size(); ++byte) {
    std::string damaged = log;
    damaged[byte] =
        static_cast<char>(static_cast<unsigned char>(damaged[byte]) ^ (1U << (byte % 8)));
    const ReadResult result = ReadLog(damaged);
    const auto record = static_cast<std::size_t>(
        std::ranges::count_if(ends, [byte](std::size_t end) { return end <= byte; }));
    for (std::size_t i = 0; i < result.records.size(); ++i) {
      ASSERT_EQ(result.records[i], records[i]) << "byte " << byte << ": a damaged record came back";
    }
    if (record + 1 == records.size()) {
      EXPECT_TRUE(result.status.ok()) << "byte " << byte << ": " << result.status.ToString();
      EXPECT_EQ(result.records.size(), record) << "byte " << byte;
      EXPECT_TRUE(result.tail.has_value()) << "byte " << byte;
    } else {
      EXPECT_TRUE(IsCorruption(result.status))
          << "byte " << byte << ": " << result.status.ToString();
      EXPECT_EQ(result.records.size(), record) << "byte " << byte;
    }
  }
}

TEST_F(LogTest, DamageInsideAMultiBlockRecordIsCorruptionWhenRecordsFollow) {
  const std::vector<std::string> records = {"before", RandomBytes(3 * kBlockSize, 5), "after"};
  const std::string log = WriteLog(records);
  // A byte in the first, a middle and the last fragment of the long record
  // (the last one holds only the final ~40 bytes, from 3 * kBlockSize on).
  ASSERT_GT(log.size(), (3 * kBlockSize) + 10);
  for (const std::size_t byte : {std::size_t{100}, kBlockSize + 100, (3 * kBlockSize) + 10}) {
    std::string damaged = log;
    damaged[byte] = static_cast<char>(damaged[byte] ^ 0x40);
    const ReadResult result = ReadLog(damaged);
    EXPECT_TRUE(IsCorruption(result.status)) << "byte " << byte << ": " << result.status.ToString();
    EXPECT_EQ(result.records, std::vector<std::string>({"before"})) << "byte " << byte;
  }
}

TEST_F(LogTest, ZeroedRegionInTheMiddleIsCorruption) {
  const std::vector<std::string> records = {RandomBytes(3 * kBlockSize, 6), "after"};
  std::string log = WriteLog(records);
  log.replace(kBlockSize, 500, std::string(500, '\0'));  // a middle fragment's header and data
  const ReadResult result = ReadLog(log);
  EXPECT_TRUE(IsCorruption(result.status)) << result.status.ToString();
  EXPECT_NE(result.status.message().find("offset " + std::to_string(kBlockSize)), std::string::npos)
      << result.status.message();
}

TEST_F(LogTest, DamagedLengthInTheLastBlockWithRecordsAfterIsCorruption) {
  // The length of the first of three records in the final, partial block
  // points past the end of the file; the records after it are intact.
  std::string log = WriteLog({"aaaa", "bbbb", "cccc"});
  log[5] = '\x7f';  // length high byte: 0x7f04 bytes
  const ReadResult result = ReadLog(log);
  EXPECT_TRUE(IsCorruption(result.status)) << result.status.ToString();
}

// --- Payloads that look like log data (found by fuzzing, Phase 2.7) -----------------

// A valid, complete FULL fragment holding `payload`: what a value may contain.
std::string ForgedFragment(std::string_view payload) {
  std::string fragment;
  const char type = static_cast<char>(RecordType::kFull);
  PutFixed32(fragment,
             crc32c::Mask(crc32c::Extend(crc32c::Value(std::string_view(&type, 1)), payload)));
  fragment.push_back(static_cast<char>(payload.size() & 0xFFU));
  fragment.push_back(static_cast<char>(payload.size() >> 8U));
  fragment.push_back(type);
  fragment.append(payload);
  return fragment;
}

// A crash tears a record whose value contains a valid fragment. The torn
// record's payload is user data and is not searched: a torn tail, never
// Corruption (which would keep the database from opening).
TEST_F(LogTest, TornRecordWhosePayloadHoldsAValidFragmentIsATornTail) {
  for (const std::string& inside : {ForgedFragment(""), ForgedFragment("hello")}) {
    const std::string value = std::string(100, 'v') + inside + std::string(100, 'v');
    const std::string log = WriteLog({"first", value});
    const std::size_t first_end = kHeaderSize + 5;
    for (std::size_t cut = first_end + 1; cut < log.size(); ++cut) {
      const ReadResult result = ReadLog(std::string_view(log).substr(0, cut));
      ASSERT_TRUE(result.status.ok()) << "cut at " << cut << ": " << result.status.ToString();
      ASSERT_EQ(result.records, std::vector<std::string>{"first"}) << "cut at " << cut;
      ASSERT_TRUE(result.tail.has_value()) << "cut at " << cut;
    }
  }
}

// The worst case of the above: a whole log stored as a value (in records
// that span blocks). Every truncation of the outer log is still a torn tail.
TEST_F(LogTest, EveryTruncationIsATornTailEvenWhenValuesAreLogs) {
  const std::string inner = WriteLog({"a", "bb", std::string(40000, 'c'), "dddd"});
  const std::vector<std::string> records = {"before", inner, inner.substr(0, 9000), "after"};
  std::vector<std::size_t> ends;
  for (std::size_t i = 1; i <= records.size(); ++i) {
    ends.push_back(
        WriteLog({records.begin(), records.begin() + static_cast<std::ptrdiff_t>(i)}).size());
  }
  const std::string log = WriteLog(records);
  // Every cut would take minutes under sanitizers: every 97th, plus every
  // cut near a record end or a block boundary.
  for (std::size_t cut = 0; cut <= log.size(); ++cut) {
    const bool near_end = std::ranges::any_of(
        ends, [cut](std::size_t end) { return cut + 8 >= end && cut <= end + 8; });
    const bool near_block = cut % kBlockSize < 8 || cut % kBlockSize > kBlockSize - 8;
    if (cut % 97 != 0 && !near_end && !near_block) {
      continue;
    }
    const ReadResult result = ReadLog(std::string_view(log).substr(0, cut));
    ASSERT_TRUE(result.status.ok()) << "cut at " << cut << ": " << result.status.ToString();
    const auto complete = static_cast<std::size_t>(
        std::ranges::count_if(ends, [cut](std::size_t end) { return end <= cut; }));
    ASSERT_EQ(result.records.size(), complete) << "cut at " << cut;
    for (std::size_t i = 0; i < complete; ++i) {
      ASSERT_EQ(result.records[i], records[i]) << "cut at " << cut;
    }
  }
}

// An intact-looking header whose length field was damaged: the CRC matches a
// shorter payload, so the real fragment ended there and data follows it.
TEST_F(LogTest, DamagedLengthIsCaughtByTheChecksum) {
  std::string log = WriteLog({"aaaa", "bbbb"});
  log[4] = static_cast<char>(4 + kHeaderSize);  // now swallows the next fragment's header
  const ReadResult result = ReadLog(log);
  EXPECT_TRUE(IsCorruption(result.status)) << result.status.ToString();
  EXPECT_NE(result.status.ToString().find("length field damaged"), std::string::npos)
      << result.status.ToString();
  EXPECT_TRUE(result.records.empty());
}

// --- I/O errors ------------------------------------------------------------------------

class FailingSequentialFile final : public SequentialFile {
 public:
  Result<std::size_t> Read(std::span<char> /*buffer*/) override {
    return Status::IOError("read failed", "injected");
  }
  Status Skip(std::uint64_t /*n*/) override { return Status::IOError("skip failed", "injected"); }
};

class FailingWritableFile final : public WritableFile {
 public:
  Status Append(std::string_view /*data*/) override {
    return Status::IOError("append failed", "injected");
  }
  Status Flush() override { return Status::OK(); }
  Status Sync() override { return Status::OK(); }
  Status Close() override { return Status::OK(); }
};

TEST(LogIoTest, ReadErrorsAreReturnedAndSticky) {
  Reader reader(std::make_unique<FailingSequentialFile>());
  const Status first = reader.ReadRecord().status();
  EXPECT_EQ(first.code(), Status::Code::kIOError);
  EXPECT_EQ(reader.ReadRecord().status(), first);
}

TEST(LogIoTest, WriteErrorsAreReturned) {
  FailingWritableFile file;
  Writer writer(&file);
  EXPECT_EQ(writer.AddRecord("x").code(), Status::Code::kIOError);
}

}  // namespace
}  // namespace forgedb::log
