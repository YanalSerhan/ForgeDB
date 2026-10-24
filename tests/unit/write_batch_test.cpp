#include "forgedb/write_batch.h"

#include <cstddef>
#include <cstdint>

#include <gtest/gtest.h>

#include <random>
#include <string>
#include <string_view>

#include "forgedb/status.h"

#include "db/internal_key.h"
#include "db/write_batch_internal.h"
#include "memtable/memtable.h"

namespace forgedb {
namespace {

// Records the operations it receives as text, e.g. "Put(a, 1) Delete(b) ".
class Recorder final : public WriteBatch::Handler {
 public:
  void Put(std::string_view key, std::string_view value) override {
    log_.append("Put(").append(key).append(", ").append(value).append(") ");
  }
  void Delete(std::string_view key) override { log_.append("Delete(").append(key).append(") "); }

  [[nodiscard]] const std::string& Log() const noexcept { return log_; }

 private:
  std::string log_;
};

// The operations in a batch, or "error: <status>" if it is malformed.
std::string Contents(const WriteBatch& batch) {
  Recorder recorder;
  const Status status = batch.Iterate(recorder);
  if (!status.ok()) {
    EXPECT_EQ(recorder.Log(), "") << "a malformed batch must not reach the handler";
    return "error: " + status.ToString();
  }
  return recorder.Log();
}

// A batch holding exactly `bytes` (which must include a full header).
WriteBatch FromBytes(std::string_view bytes) {
  WriteBatch batch;
  EXPECT_TRUE(WriteBatchInternal::SetContents(batch, bytes).ok());
  return batch;
}

std::string Bytes(const WriteBatch& batch) {
  return std::string(WriteBatchInternal::Contents(batch));
}

// --- Building and reading -----------------------------------------------------

TEST(WriteBatchTest, Empty) {
  const WriteBatch batch;
  EXPECT_EQ(batch.Count(), 0U);
  EXPECT_EQ(batch.ApproximateSize(), WriteBatchInternal::kHeaderSize);
  EXPECT_EQ(WriteBatchInternal::Sequence(batch), 0U);
  EXPECT_EQ(Contents(batch), "");
}

TEST(WriteBatchTest, OperationsComeBackInOrder) {
  WriteBatch batch;
  batch.Put("a", "1");
  batch.Delete("b");
  batch.Put("a", "2");  // the same key twice is fine: later wins when applied
  batch.Put("", "");    // empty key and value
  EXPECT_EQ(batch.Count(), 4U);
  EXPECT_EQ(Contents(batch), "Put(a, 1) Delete(b) Put(a, 2) Put(, ) ");
}

// The exact bytes, matching docs/file-formats.md, "Write batch".
TEST(WriteBatchTest, ByteLayout) {
  WriteBatch batch;
  WriteBatchInternal::SetSequence(batch, 100);
  batch.Put("ab", "xyz");
  batch.Delete("c");
  const std::string expected(
      "\x64\x00\x00\x00\x00\x00\x00\x00"  // sequence 100, fixed64
      "\x02\x00\x00\x00"                  // count 2, fixed32
      "\x01\x02"
      "ab\x03xyz"  // Put: type 1, len 2, "ab", len 3, "xyz"
      "\x00\x01"
      "c",  // Delete: type 0, len 1, "c"
      23);
  EXPECT_EQ(Bytes(batch), expected);
  EXPECT_EQ(batch.ApproximateSize(), 23U);
}

TEST(WriteBatchTest, BinaryAndLongKeysAndValues) {
  WriteBatch batch;
  const std::string binary("\x00\xff\x01", 3);
  const std::string long_value(300, 'v');  // a 2-byte varint length
  batch.Put(binary, long_value);
  batch.Delete(binary);
  EXPECT_EQ(Contents(batch), "Put(" + binary + ", " + long_value + ") Delete(" + binary + ") ");
}

TEST(WriteBatchTest, ClearEmptiesTheBatch) {
  WriteBatch batch;
  WriteBatchInternal::SetSequence(batch, 7);
  batch.Put("a", "1");
  batch.Clear();
  EXPECT_EQ(batch.Count(), 0U);
  EXPECT_EQ(batch.ApproximateSize(), WriteBatchInternal::kHeaderSize);
  EXPECT_EQ(WriteBatchInternal::Sequence(batch), 0U) << "Clear resets the header too";
  batch.Put("b", "2");  // usable again
  EXPECT_EQ(Contents(batch), "Put(b, 2) ");
}

TEST(WriteBatchTest, ApproximateSizeGrowsWithEachOperation) {
  WriteBatch batch;
  const std::size_t empty = batch.ApproximateSize();
  batch.Put("key", "value");
  const std::size_t one_put = batch.ApproximateSize();
  EXPECT_EQ(one_put, empty + 1 + 1 + 3 + 1 + 5);  // type, len, key, len, value
  batch.Delete("key");
  EXPECT_EQ(batch.ApproximateSize(), one_put + 1 + 1 + 3);
}

TEST(WriteBatchTest, CopiesAreIndependent) {
  WriteBatch original;
  original.Put("a", "1");
  WriteBatch copy = original;
  copy.Delete("b");
  EXPECT_EQ(Contents(original), "Put(a, 1) ");
  EXPECT_EQ(Contents(copy), "Put(a, 1) Delete(b) ");
}

TEST(WriteBatchTest, SequenceAndContentsRoundTrip) {
  WriteBatch batch;
  WriteBatchInternal::SetSequence(batch, kMaxSequenceNumber);
  batch.Put("k", "v");
  EXPECT_EQ(WriteBatchInternal::Sequence(batch), kMaxSequenceNumber);

  const WriteBatch restored = FromBytes(WriteBatchInternal::Contents(batch));
  EXPECT_EQ(Bytes(restored), Bytes(batch));
  EXPECT_EQ(WriteBatchInternal::Sequence(restored), kMaxSequenceNumber);
  EXPECT_EQ(Contents(restored), "Put(k, v) ");
}

// --- Applying to a memtable -------------------------------------------------------

std::string GetValue(const MemTable& table, std::string_view key, SequenceNumber snapshot) {
  std::string value;
  switch (table.Get(LookupKey(key, snapshot), &value)) {
    case MemTable::GetResult::kFound:
      return value;
    case MemTable::GetResult::kDeleted:
      return "DELETED";
    case MemTable::GetResult::kNotFound:
      return "NOT_FOUND";
  }
  return "invalid";
}

TEST(WriteBatchTest, InsertIntoNumbersOperationsFromTheBatchSequence) {
  WriteBatch batch;
  WriteBatchInternal::SetSequence(batch, 100);
  batch.Put("a", "first");    // 100
  batch.Put("b", "b-value");  // 101
  batch.Delete("a");          // 102
  batch.Put("a", "second");   // 103

  const MemTableRef table = MemTable::Create();
  ASSERT_TRUE(WriteBatchInternal::InsertInto(batch, *table).ok());

  EXPECT_EQ(GetValue(*table, "a", 99), "NOT_FOUND");
  EXPECT_EQ(GetValue(*table, "a", 100), "first");
  EXPECT_EQ(GetValue(*table, "b", 101), "b-value");
  EXPECT_EQ(GetValue(*table, "a", 102), "DELETED");
  EXPECT_EQ(GetValue(*table, "a", 103), "second") << "later operations in a batch win";
  EXPECT_EQ(GetValue(*table, "a", kMaxSequenceNumber), "second");
}

TEST(WriteBatchTest, InsertIntoAcceptsTheLastSequenceNumbers) {
  WriteBatch batch;
  WriteBatchInternal::SetSequence(batch, kMaxSequenceNumber - 1);
  batch.Put("a", "1");  // kMax - 1
  batch.Put("b", "2");  // kMax
  const MemTableRef table = MemTable::Create();
  ASSERT_TRUE(WriteBatchInternal::InsertInto(batch, *table).ok());
  EXPECT_EQ(GetValue(*table, "b", kMaxSequenceNumber), "2");
}

TEST(WriteBatchTest, InsertIntoRejectsSequenceOverflowWithoutChangingTheMemtable) {
  for (const SequenceNumber first :
       {kMaxSequenceNumber, kMaxSequenceNumber + 1, ~SequenceNumber{0}}) {
    SCOPED_TRACE(first);
    WriteBatch batch;
    WriteBatchInternal::SetSequence(batch, first);
    batch.Put("a", "1");
    batch.Put("b", "2");  // first + 1 > kMaxSequenceNumber
    const MemTableRef table = MemTable::Create();
    const std::size_t memory_before = table->ApproximateMemoryUsage();
    const Status status = WriteBatchInternal::InsertInto(batch, *table);
    EXPECT_EQ(status.code(), Status::Code::kCorruption) << status.ToString();
    EXPECT_EQ(table->ApproximateMemoryUsage(), memory_before) << "nothing was added";
  }
}

TEST(WriteBatchTest, InsertIntoAnEmptyBatchDoesNothing) {
  WriteBatch batch;
  WriteBatchInternal::SetSequence(batch, ~SequenceNumber{0});  // irrelevant: no operations
  const MemTableRef table = MemTable::Create();
  EXPECT_TRUE(WriteBatchInternal::InsertInto(batch, *table).ok());
}

// --- Malformed input ----------------------------------------------------------------
//
// SetContents and Iterate are the path for bytes read back from the log, so
// they must reject every malformed batch with Corruption, never crash, and
// never hand part of a malformed batch to the handler (Contents() checks that).

bool IsCorruption(const std::string& contents) {
  return contents.starts_with("error: Corruption: ");
}

TEST(WriteBatchTest, SetContentsRejectsBytesShorterThanTheHeader) {
  for (std::size_t size = 0; size < WriteBatchInternal::kHeaderSize; ++size) {
    WriteBatch batch;
    batch.Put("stale", "data");
    const Status status = WriteBatchInternal::SetContents(batch, std::string(size, '\0'));
    EXPECT_EQ(status.code(), Status::Code::kCorruption) << "size " << size;
    EXPECT_EQ(batch.Count(), 0U) << "left empty, not holding the stale operations";
    EXPECT_EQ(Contents(batch), "");
  }
}

// Every strict prefix of a valid batch is malformed: either a record is cut
// off, or the records are whole but fewer than the header's count.
TEST(WriteBatchTest, EveryTruncationIsCorruption) {
  WriteBatch batch;
  WriteBatchInternal::SetSequence(batch, 5);
  batch.Put("key", "value");
  batch.Delete("gone");
  batch.Put(std::string(200, 'k'), std::string(300, 'v'));  // multi-byte varints
  const std::string bytes = Bytes(batch);
  for (std::size_t size = WriteBatchInternal::kHeaderSize; size < bytes.size(); ++size) {
    const std::string contents = Contents(FromBytes(std::string_view(bytes).substr(0, size)));
    EXPECT_TRUE(IsCorruption(contents)) << "size " << size << ": " << contents;
  }
}

TEST(WriteBatchTest, CountMismatchIsCorruption) {
  WriteBatch batch;
  batch.Put("a", "1");
  batch.Delete("b");
  for (const std::uint32_t wrong_count : {0U, 1U, 3U, UINT32_MAX}) {
    WriteBatch modified = batch;
    WriteBatchInternal::SetCount(modified, wrong_count);
    EXPECT_EQ(Contents(modified),
              "error: Corruption: write batch record count mismatch: header says " +
                  std::to_string(wrong_count) + ", found 2");
  }
}

TEST(WriteBatchTest, UnknownRecordTypeIsCorruption) {
  WriteBatch batch;
  batch.Put("a", "1");
  std::string bytes = Bytes(batch);
  bytes[WriteBatchInternal::kHeaderSize] = '\x02';  // the Put's type byte
  EXPECT_EQ(Contents(FromBytes(bytes)),
            "error: Corruption: unknown record type in write batch: type 2 at byte 12");
}

TEST(WriteBatchTest, LengthPastTheEndIsCorruption) {
  WriteBatch batch;
  batch.Put("a", "1");
  std::string bytes = Bytes(batch);
  bytes[WriteBatchInternal::kHeaderSize + 1] = '\x7f';  // key length 127, but 3 bytes follow
  EXPECT_EQ(Contents(FromBytes(bytes)),
            "error: Corruption: truncated Put record in write batch: at byte 12");

  WriteBatch deletion;
  deletion.Delete("a");
  bytes = Bytes(deletion);
  bytes[WriteBatchInternal::kHeaderSize + 1] = '\x05';  // key length 5, but 1 byte follows
  EXPECT_EQ(Contents(FromBytes(bytes)),
            "error: Corruption: truncated Delete record in write batch: at byte 12");
}

TEST(WriteBatchTest, OversizedVarintLengthIsCorruption) {
  // A length varint that doesn't fit in 32 bits (five bytes, top bits set).
  std::string bytes(WriteBatchInternal::kHeaderSize, '\0');
  bytes[8] = '\x01';                                    // count 1
  bytes += std::string("\x00\xff\xff\xff\xff\x7f", 6);  // Delete, bad length
  EXPECT_TRUE(IsCorruption(Contents(FromBytes(bytes))));
}

// Random damage to valid batches: every result must be either a clean parse
// or Corruption with the handler untouched (checked inside Contents); ASan
// and UBSan catch any out-of-bounds read along the way.
TEST(WriteBatchTest, RandomDamageNeverCrashesOrLeaksPartialBatches) {
  std::mt19937_64 rng(77);
  int corrupt = 0;
  int clean = 0;
  for (int round = 0; round < 3000; ++round) {
    WriteBatch batch;
    const int operations = static_cast<int>(rng() % 6);
    for (int i = 0; i < operations; ++i) {
      const std::string key(rng() % 20, static_cast<char>('a' + (rng() % 26)));
      if (rng() % 3 == 0) {
        batch.Delete(key);
      } else {
        batch.Put(key, std::string(rng() % 200, 'v'));
      }
    }
    std::string bytes = Bytes(batch);
    // Flip, overwrite, truncate or extend.
    switch (rng() % 4) {
      case 0: {
        char& byte = bytes[rng() % bytes.size()];
        byte = static_cast<char>(static_cast<unsigned char>(byte) ^ (1U << (rng() % 8)));
        break;
      }
      case 1:
        bytes[rng() % bytes.size()] = static_cast<char>(rng());
        break;
      case 2:
        bytes.resize(WriteBatchInternal::kHeaderSize +
                     (rng() % (bytes.size() + 1 - WriteBatchInternal::kHeaderSize)));
        break;
      default:
        bytes.push_back(static_cast<char>(rng()));
        break;
    }
    const std::string contents = Contents(FromBytes(bytes));
    if (contents.starts_with("error: ")) {
      ASSERT_TRUE(IsCorruption(contents)) << contents;
      ++corrupt;
    } else {
      ++clean;  // e.g. a flipped bit inside a key: still well-formed
    }
  }
  EXPECT_GT(corrupt, 0);
  EXPECT_GT(clean, 0);
}

TEST(WriteBatchTest, MalformedBatchIsNotInsertedIntoTheMemtable) {
  WriteBatch batch;
  batch.Put("a", "1");
  batch.Put("b", "2");
  std::string bytes = Bytes(batch);
  bytes.pop_back();  // damage the second record only
  const WriteBatch damaged = FromBytes(bytes);

  const MemTableRef table = MemTable::Create();
  const Status status = WriteBatchInternal::InsertInto(damaged, *table);
  EXPECT_EQ(status.code(), Status::Code::kCorruption);
  EXPECT_EQ(GetValue(*table, "a", kMaxSequenceNumber), "NOT_FOUND")
      << "the valid first record must not be applied on its own";
}

}  // namespace
}  // namespace forgedb
