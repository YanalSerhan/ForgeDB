#include "table/table.h"

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdio>

#include <gtest/gtest.h>

#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include "forgedb/iterator.h"
#include "forgedb/result.h"
#include "forgedb/status.h"

#include "db/internal_key.h"
#include "env/file_system.h"
#include "table/block_format.h"
#include "table/table_builder.h"
#include "table/table_format.h"
#include "util/coding.h"
#include "util/crc32c.h"

namespace forgedb {
namespace {

// ---------------------------------------------------------------------------
// Files in memory
// ---------------------------------------------------------------------------

// Collects appended bytes; fails every Append after `fail_after` succeeded.
class StringSink final : public WritableFile {
 public:
  explicit StringSink(std::string* out, int fail_after = -1) : out_(out), left_(fail_after) {}

  Status Append(std::string_view data) override {
    if (left_ == 0) {
      return Status::IOError("injected write error");
    }
    if (left_ > 0) {
      --left_;
    }
    out_->append(data);
    return Status::OK();
  }
  Status Flush() override { return Status::OK(); }
  Status Sync() override { return Status::OK(); }
  Status Close() override { return Status::OK(); }

 private:
  std::string* out_;
  int left_;
};

class StringSource final : public RandomAccessFile {
 public:
  explicit StringSource(std::string contents) : contents_(std::move(contents)) {}

  Result<std::size_t> Read(std::uint64_t offset, std::span<char> buffer) const override {
    if (offset >= contents_.size()) {
      return std::size_t{0};
    }
    const std::size_t n = std::min(buffer.size(), contents_.size() - offset);
    std::copy_n(contents_.data() + offset, n, buffer.data());
    return n;
  }

 private:
  std::string contents_;
};

// ---------------------------------------------------------------------------
// Building and opening
// ---------------------------------------------------------------------------

struct Entry {
  std::string user_key;
  SequenceNumber sequence;
  ValueType type;
  std::string value;
};

std::string InternalKey(std::string_view user_key, SequenceNumber sequence,
                        ValueType type = ValueType::kValue) {
  std::string key;
  AppendInternalKey(key, {.user_key = user_key, .sequence = sequence, .type = type});
  return key;
}

// Entries must be in internal-key order.
std::string BuildTable(const std::vector<Entry>& entries, const TableOptions& options = {}) {
  std::string bytes;
  StringSink sink(&bytes);
  TableBuilder builder(options, &sink);
  for (const Entry& e : entries) {
    builder.Add(InternalKey(e.user_key, e.sequence, e.type), e.value);
  }
  EXPECT_TRUE(builder.Finish().ok());
  EXPECT_EQ(builder.FileSize(), bytes.size());
  EXPECT_EQ(builder.NumEntries(), entries.size());
  return bytes;
}

Result<std::unique_ptr<Table>> OpenTable(const std::string& bytes, BlockCache* cache = nullptr) {
  return Table::Open(std::make_unique<StringSource>(bytes), bytes.size(), cache);
}

std::unique_ptr<Table> OpenOrDie(const std::string& bytes, BlockCache* cache = nullptr) {
  Result<std::unique_ptr<Table>> table = OpenTable(bytes, cache);
  EXPECT_TRUE(table.ok()) << table.status().ToString();
  return table.ok() ? std::move(table).value() : nullptr;
}

std::string NumberedKey(int n) {
  const std::string digits = std::to_string(n);
  return "key" + std::string(7 - digits.size(), '0') + digits;
}

std::string ValueFor(int n) {
  return std::string(static_cast<std::size_t>(n % 50), 'v') + std::to_string(n);
}

// Keys 0, 2, 4, ...: odd numbers are missing keys inside the table's range.
std::vector<Entry> EvenEntries(int count) {
  std::vector<Entry> entries;
  entries.reserve(static_cast<std::size_t>(count));
  for (int i = 0; i < count; ++i) {
    entries.push_back({.user_key = NumberedKey(2 * i),
                       .sequence = static_cast<SequenceNumber>(i + 1),
                       .type = ValueType::kValue,
                       .value = ValueFor(2 * i)});
  }
  return entries;
}

Result<TableLookup> Get(const Table& table, std::string_view user_key, std::string* value,
                        SequenceNumber sequence = kMaxSequenceNumber) {
  return table.Get(LookupKey(user_key, sequence), value);
}

std::vector<std::pair<std::string, std::string>> Scan(const Table& table, Status* status) {
  std::vector<std::pair<std::string, std::string>> result;
  const std::unique_ptr<Iterator> it = table.NewIterator();
  for (it->SeekToFirst(); it->Valid(); it->Next()) {
    result.emplace_back(it->key(), it->value());
  }
  *status = it->status();
  return result;
}

std::vector<std::pair<std::string, std::string>> Expected(const std::vector<Entry>& entries) {
  std::vector<std::pair<std::string, std::string>> result;
  result.reserve(entries.size());
  for (const Entry& e : entries) {
    result.emplace_back(InternalKey(e.user_key, e.sequence, e.type), e.value);
  }
  return result;
}

bool IsCorruption(const Status& status) { return status.code() == Status::Code::kCorruption; }

void FlipBit(std::string& bytes, std::uint64_t at, int bit) {
  const auto i = static_cast<std::size_t>(at);
  bytes[i] = static_cast<char>(static_cast<unsigned char>(bytes[i]) ^ (1U << bit));
}

// ---------------------------------------------------------------------------
// Round trips
// ---------------------------------------------------------------------------

TEST(TableTest, RoundTrip100kKeys) {
  constexpr int kKeys = 100000;
  const std::vector<Entry> entries = EvenEntries(kKeys);
  const std::string bytes = BuildTable(entries);
  BlockCache cache(64 << 20);
  const std::unique_ptr<Table> table = OpenOrDie(bytes, &cache);
  ASSERT_NE(table, nullptr);
  EXPECT_TRUE(table->HasFilter());

  Status status;
  EXPECT_EQ(Scan(*table, &status), Expected(entries));
  EXPECT_TRUE(status.ok()) << status.ToString();

  // Seek to every 7th key exactly, and to the missing key just before it.
  const std::unique_ptr<Iterator> it = table->NewIterator();
  for (int i = 0; i < kKeys; i += 7) {
    it->Seek(InternalKey(NumberedKey(2 * i), kMaxSequenceNumber));
    ASSERT_TRUE(it->Valid()) << i;
    EXPECT_EQ(ExtractUserKey(it->key()), NumberedKey(2 * i));
    EXPECT_EQ(it->value(), ValueFor(2 * i));
    it->Seek(InternalKey(NumberedKey((2 * i) - 1), kMaxSequenceNumber));
    ASSERT_TRUE(it->Valid()) << i;
    EXPECT_EQ(ExtractUserKey(it->key()), NumberedKey(2 * i));
  }
  it->Seek(InternalKey(NumberedKey(2 * kKeys), kMaxSequenceNumber));
  EXPECT_FALSE(it->Valid());
  EXPECT_TRUE(it->status().ok());

  std::string value;
  for (int i = 0; i < kKeys; ++i) {
    const Result<TableLookup> found = Get(*table, NumberedKey(2 * i), &value);
    ASSERT_TRUE(found.ok()) << found.status().ToString();
    ASSERT_EQ(*found, TableLookup::kFound) << i;
    ASSERT_EQ(value, ValueFor(2 * i));
  }

  // Missing keys inside the table's range: without a filter every one would
  // read a data block; the filter should stop all but ~0.8% of them.
  const TableStats before = table->GetStats();
  for (int i = 0; i < kKeys; ++i) {
    const Result<TableLookup> found = Get(*table, NumberedKey((2 * i) + 1), &value);
    ASSERT_TRUE(found.ok());
    ASSERT_EQ(*found, TableLookup::kNotFound);
  }
  const TableStats after = table->GetStats();
  const std::uint64_t blocks = (after.data_block_reads + after.data_block_cache_hits) -
                               (before.data_block_reads + before.data_block_cache_hits);
  std::printf(  // NOLINT(cppcoreguidelines-pro-type-vararg)
      "[ table    ] %d missing-key Gets: %llu rejected by the filter, %llu read a data block "
      "(%.3f%%)\n",
      kKeys, static_cast<unsigned long long>(after.filter_rejections - before.filter_rejections),
      static_cast<unsigned long long>(blocks), 100.0 * static_cast<double>(blocks) / kKeys);
  EXPECT_EQ(after.filter_rejections - before.filter_rejections + blocks,
            static_cast<std::uint64_t>(kKeys));
  EXPECT_LT(blocks, static_cast<std::uint64_t>(kKeys) / 50);  // under 2%
}

TEST(TableTest, VersionsAndDeletionsAcrossBlockBoundaries) {
  // Tiny blocks, so one user key's versions span several blocks.
  const TableOptions options{.block_size = 32};
  std::vector<Entry> entries = {
      {.user_key = "a", .sequence = 1, .type = ValueType::kValue, .value = "a1"},
  };
  for (SequenceNumber s = 20; s >= 2; --s) {
    entries.push_back({.user_key = "k",
                       .sequence = s,
                       .type = s % 3 == 0 ? ValueType::kDeletion : ValueType::kValue,
                       .value = s % 3 == 0 ? "" : "k" + std::to_string(s)});
  }
  entries.push_back({.user_key = "z", .sequence = 1, .type = ValueType::kValue, .value = "z1"});
  const std::string bytes = BuildTable(entries, options);
  const std::unique_ptr<Table> table = OpenOrDie(bytes);
  ASSERT_NE(table, nullptr);

  Status status;
  EXPECT_EQ(Scan(*table, &status), Expected(entries));
  EXPECT_TRUE(status.ok());

  std::string value;
  for (SequenceNumber s = 2; s <= 25; ++s) {
    const SequenceNumber newest = std::min<SequenceNumber>(s, 20);
    const Result<TableLookup> found = Get(*table, "k", &value, s);
    ASSERT_TRUE(found.ok());
    if (newest % 3 == 0) {
      EXPECT_EQ(*found, TableLookup::kDeleted) << s;
    } else {
      ASSERT_EQ(*found, TableLookup::kFound) << s;
      EXPECT_EQ(value, "k" + std::to_string(newest));
    }
  }
  EXPECT_EQ(*Get(*table, "k", &value, 1), TableLookup::kNotFound);  // older than every version
  EXPECT_EQ(*Get(*table, "b", &value), TableLookup::kNotFound);
  EXPECT_EQ(*Get(*table, "zz", &value), TableLookup::kNotFound);  // past the last key
  ASSERT_EQ(*Get(*table, "z", &value), TableLookup::kFound);
  EXPECT_EQ(value, "z1");
}

TEST(TableTest, ValuesLargerThanABlock) {
  const std::vector<Entry> entries = {
      {.user_key = "a",
       .sequence = 1,
       .type = ValueType::kValue,
       .value = std::string(100000, 'a')},
      {.user_key = "b", .sequence = 1, .type = ValueType::kValue, .value = "small"},
      {.user_key = "c",
       .sequence = 1,
       .type = ValueType::kValue,
       .value = std::string(1 << 20, 'c')},
  };
  const std::unique_ptr<Table> table = OpenOrDie(BuildTable(entries));
  ASSERT_NE(table, nullptr);
  Status status;
  EXPECT_EQ(Scan(*table, &status), Expected(entries));
  std::string value;
  ASSERT_EQ(*Get(*table, "c", &value), TableLookup::kFound);
  EXPECT_EQ(value.size(), std::size_t{1} << 20);
}

TEST(TableTest, EmptyTable) {
  for (const int bits : {10, 0}) {
    const std::string bytes = BuildTable({}, {.filter_bits_per_key = bits});
    // Filter (9 bytes) if any, empty index block (8 bytes), 5-byte trailers, footer.
    EXPECT_EQ(bytes.size(), (bits > 0 ? 9 + 5 : 0) + 8 + 5 + kTableFooterSize);
    const std::unique_ptr<Table> table = OpenOrDie(bytes);
    ASSERT_NE(table, nullptr);
    Status status;
    EXPECT_TRUE(Scan(*table, &status).empty());
    EXPECT_TRUE(status.ok());
    std::string value;
    EXPECT_EQ(*Get(*table, "anything", &value), TableLookup::kNotFound);
    const std::unique_ptr<Iterator> it = table->NewIterator();
    it->Seek(InternalKey("", kMaxSequenceNumber));
    EXPECT_FALSE(it->Valid());
  }
}

TEST(TableTest, WithoutAFilter) {
  const std::vector<Entry> entries = EvenEntries(1000);
  const std::unique_ptr<Table> table = OpenOrDie(BuildTable(entries, {.filter_bits_per_key = 0}));
  ASSERT_NE(table, nullptr);
  EXPECT_FALSE(table->HasFilter());
  std::string value;
  ASSERT_EQ(*Get(*table, NumberedKey(10), &value), TableLookup::kFound);
  EXPECT_EQ(*Get(*table, NumberedKey(11), &value), TableLookup::kNotFound);
  EXPECT_EQ(table->GetStats().filter_rejections, 0U);
  EXPECT_EQ(table->GetStats().data_block_reads, 2U);  // no cache: one read per Get
}

TEST(TableTest, BlockCacheServesRepeatedReads) {
  const std::vector<Entry> entries = EvenEntries(5000);
  BlockCache cache(1 << 20);
  const std::unique_ptr<Table> table = OpenOrDie(BuildTable(entries), &cache);
  ASSERT_NE(table, nullptr);
  Status status;
  (void)Scan(*table, &status);
  const TableStats first = table->GetStats();
  EXPECT_GT(first.data_block_reads, 10U);
  EXPECT_EQ(first.data_block_cache_hits, 0U);
  (void)Scan(*table, &status);
  const TableStats second = table->GetStats();
  EXPECT_EQ(second.data_block_reads, first.data_block_reads);  // all from the cache
  EXPECT_EQ(second.data_block_cache_hits, first.data_block_reads);
  EXPECT_EQ(cache.GetStats().entries, first.data_block_reads);
}

TEST(TableTest, BlockSizeIsRespected) {
  const std::vector<Entry> entries = EvenEntries(20000);
  const std::string bytes = BuildTable(entries);
  BlockCache cache(64 << 20);
  const std::unique_ptr<Table> table = OpenOrDie(bytes, &cache);
  ASSERT_NE(table, nullptr);
  Status status;
  (void)Scan(*table, &status);
  const CacheStats stats = cache.GetStats();
  // A block is cut by the entry that takes it to 4096 bytes or more (entries
  // here are under 100 bytes); only the last block may be smaller.
  ASSERT_GT(stats.entries, 10U);
  EXPECT_GE(stats.charge, 4096 * (stats.entries - 1));
  EXPECT_LT(stats.charge, (4096 + 100) * stats.entries);
}

TEST(TableTest, ConcurrentGets) {
  const std::vector<Entry> entries = EvenEntries(20000);
  BlockCache cache(256 << 10);  // smaller than the table: constant eviction
  const std::unique_ptr<Table> table = OpenOrDie(BuildTable(entries), &cache);
  ASSERT_NE(table, nullptr);
  std::atomic<int> wrong{0};
  std::vector<std::thread> threads;
  threads.reserve(8);
  for (int t = 0; t < 8; ++t) {
    threads.emplace_back([&table, &wrong, t] {
      std::string value;
      for (int i = t; i < 40000; i += 3) {
        const Result<TableLookup> found = Get(*table, NumberedKey(i), &value);
        const bool present = i % 2 == 0;
        if (!found.ok() || (*found == TableLookup::kFound) != present ||
            (present && value != ValueFor(i))) {
          wrong.fetch_add(1);
        }
      }
    });
  }
  for (std::thread& thread : threads) {
    thread.join();
  }
  EXPECT_EQ(wrong.load(), 0);
}

TEST(TableTest, WriteErrorsStick) {
  for (int fail_after = 0; fail_after < 8; ++fail_after) {
    std::string bytes;
    StringSink sink(&bytes, fail_after);
    TableBuilder builder({.block_size = 64}, &sink);
    for (const Entry& e : EvenEntries(50)) {
      builder.Add(InternalKey(e.user_key, e.sequence), e.value);
    }
    const Status status = builder.Finish();
    EXPECT_EQ(status.code(), Status::Code::kIOError) << fail_after;
    EXPECT_EQ(builder.status(), status);
  }
}

// ---------------------------------------------------------------------------
// Footer
// ---------------------------------------------------------------------------

TEST(TableFooterTest, Layout) {
  const TableFooter footer{.filter = {.offset = 0x1122, .size = 0x33},
                           .index = {.offset = 0x4455, .size = 0x66}};
  const std::string encoded = EncodeTableFooter(footer);
  ASSERT_EQ(encoded.size(), kTableFooterSize);
  EXPECT_EQ(DecodeFixed64(encoded.data()), 0x1122U);
  EXPECT_EQ(DecodeFixed64(&encoded[8]), 0x33U);
  EXPECT_EQ(DecodeFixed64(&encoded[16]), 0x4455U);
  EXPECT_EQ(DecodeFixed64(&encoded[24]), 0x66U);
  EXPECT_EQ(DecodeFixed32(&encoded[32]), 1U);  // format version
  EXPECT_EQ(DecodeFixed32(&encoded[36]),
            crc32c::Mask(crc32c::Value(std::string_view(encoded).substr(0, 36))));
  EXPECT_EQ(encoded.substr(40), "ForgeSST");
  const Result<TableFooter> decoded = DecodeTableFooter(encoded, 0x5000);
  ASSERT_TRUE(decoded.ok()) << decoded.status().ToString();
  EXPECT_EQ(*decoded, footer);
}

TEST(TableFooterTest, BlockHandleRoundTrip) {
  for (const BlockHandle handle :
       {BlockHandle{.offset = 0, .size = 0}, BlockHandle{.offset = 127, .size = 128},
        BlockHandle{.offset = ~0ULL, .size = ~0ULL}}) {
    std::string encoded;
    AppendBlockHandle(encoded, handle);
    EXPECT_EQ(DecodeBlockHandle(encoded), handle);
    EXPECT_FALSE(DecodeBlockHandle(encoded + "x"));  // must be exactly one handle
    EXPECT_FALSE(DecodeBlockHandle(encoded.substr(0, encoded.size() - 1)));
  }
}

// Rewrites the footer's fields and recomputes its CRC, to reach the checks
// after it.
std::string WithFooter(std::string bytes, const TableFooter& footer, std::uint32_t version = 1) {
  std::string encoded = EncodeTableFooter(footer);
  EncodeFixed32(&encoded[32], version);
  EncodeFixed32(&encoded[36], crc32c::Mask(crc32c::Value(std::string_view(encoded).substr(0, 36))));
  bytes.replace(bytes.size() - kTableFooterSize, kTableFooterSize, encoded);
  return bytes;
}

TableFooter FooterOf(const std::string& bytes) {
  return *DecodeTableFooter(std::string_view(bytes).substr(bytes.size() - kTableFooterSize),
                            bytes.size());
}

void ExpectOpenFails(const std::string& bytes, Status::Code code, std::string_view message) {
  const Result<std::unique_ptr<Table>> table = OpenTable(bytes);
  ASSERT_FALSE(table.ok()) << message;
  EXPECT_EQ(table.status().code(), code) << table.status().ToString();
  EXPECT_NE(table.status().ToString().find(message), std::string::npos)
      << table.status().ToString();
}

TEST(TableOpenTest, RejectsDamagedFooters) {
  const std::string valid = BuildTable(EvenEntries(500));
  const TableFooter footer = FooterOf(valid);

  ExpectOpenFails(valid.substr(0, 47), Status::Code::kCorruption, "too short");
  ExpectOpenFails(valid.substr(0, valid.size() - 1), Status::Code::kCorruption, "bad magic");
  {
    std::string damaged = valid;
    FlipBit(damaged, damaged.size() - 1, 0);
    ExpectOpenFails(damaged, Status::Code::kCorruption, "bad magic");
  }
  {
    std::string damaged = valid;
    FlipBit(damaged, damaged.size() - kTableFooterSize + 3, 0);
    ExpectOpenFails(damaged, Status::Code::kCorruption, "footer checksum mismatch");
  }
  ExpectOpenFails(WithFooter(valid, footer, 2), Status::Code::kNotSupported,
                  "unknown table format version: 2");
  {
    TableFooter bad = footer;
    bad.index.offset = valid.size();
    ExpectOpenFails(WithFooter(valid, bad), Status::Code::kCorruption, "index block outside");
    bad.index = {.offset = ~0ULL - 2, .size = 10};  // offset + size overflows
    ExpectOpenFails(WithFooter(valid, bad), Status::Code::kCorruption, "index block outside");
  }
  {
    TableFooter bad = footer;
    bad.filter.offset = footer.index.offset;  // overlaps the index
    ExpectOpenFails(WithFooter(valid, bad), Status::Code::kCorruption, "filter block not before");
    bad.filter = {.offset = 5, .size = 0};
    ExpectOpenFails(WithFooter(valid, bad), Status::Code::kCorruption, "filter handle malformed");
  }
}

TEST(TableOpenTest, FileSizeLargerThanTheFile) {
  const std::string bytes = BuildTable(EvenEntries(10));
  const Result<std::unique_ptr<Table>> table =
      Table::Open(std::make_unique<StringSource>(bytes), bytes.size() + 10, nullptr);
  ASSERT_FALSE(table.ok());
  EXPECT_TRUE(IsCorruption(table.status())) << table.status().ToString();
}

TEST(TableOpenTest, DamagedIndexOrFilterFailsTheOpen) {
  const std::string valid = BuildTable(EvenEntries(500));
  const TableFooter footer = FooterOf(valid);
  for (const BlockHandle& block : {footer.index, footer.filter}) {
    std::string damaged = valid;
    FlipBit(damaged, block.offset + (block.size / 2), 4);
    ExpectOpenFails(damaged, Status::Code::kCorruption, "block checksum mismatch");
  }
}

// ---------------------------------------------------------------------------
// Damaged data
// ---------------------------------------------------------------------------

TEST(TableCorruptionTest, DamagedDataBlock) {
  const std::vector<Entry> entries = EvenEntries(2000);
  std::string bytes = BuildTable(entries);
  FlipBit(bytes, 100, 0);  // inside the first data block
  const std::unique_ptr<Table> table = OpenOrDie(bytes);
  ASSERT_NE(table, nullptr);

  std::string value;
  const Result<TableLookup> first = Get(*table, NumberedKey(0), &value);
  ASSERT_FALSE(first.ok());
  EXPECT_TRUE(IsCorruption(first.status())) << first.status().ToString();
  const Result<TableLookup> last = Get(*table, NumberedKey(2 * 1999), &value);
  ASSERT_TRUE(last.ok());  // other blocks are fine
  EXPECT_EQ(*last, TableLookup::kFound);

  Status status;
  EXPECT_TRUE(Scan(*table, &status).empty());
  EXPECT_TRUE(IsCorruption(status)) << status.ToString();
}

// Flips each bit of a small multi-block table in turn: the open fails, or
// every Get and the scan return correct data or Corruption. Never wrong data,
// never a crash (ASan builds check memory). Every byte of a table is covered
// by a CRC, so every flip is caught: by the open, or by the scan.
TEST(TableCorruptionTest, EveryBitFlipIsCaughtOrHarmless) {
  const std::vector<Entry> entries = EvenEntries(12);
  const std::string valid = BuildTable(entries, {.block_size = 64});
  const auto expected = Expected(entries);
  int opened = 0;
  for (std::size_t byte = 0; byte < valid.size(); ++byte) {
    for (int bit = 0; bit < 8; ++bit) {
      std::string damaged = valid;
      FlipBit(damaged, byte, bit);
      const Result<std::unique_ptr<Table>> table = OpenTable(damaged);
      if (!table.ok()) {
        continue;
      }
      ++opened;
      Status status;
      const auto scanned = Scan(**table, &status);
      ASSERT_LE(scanned.size(), expected.size());
      ASSERT_TRUE(std::equal(scanned.begin(), scanned.end(), expected.begin()))
          << "wrong data, byte " << byte << " bit " << bit;
      ASSERT_TRUE(IsCorruption(status)) << "undetected flip, byte " << byte << " bit " << bit;
      for (int i = 0; i < 24; ++i) {
        std::string value;
        const Result<TableLookup> found = Get(**table, NumberedKey(i), &value);
        if (!found.ok()) {
          ASSERT_TRUE(IsCorruption(found.status()));
          continue;
        }
        ASSERT_EQ(*found == TableLookup::kFound, i % 2 == 0) << "byte " << byte << " bit " << bit;
        if (*found == TableLookup::kFound) {
          ASSERT_EQ(value, ValueFor(i));
        }
      }
    }
  }
  EXPECT_GT(opened, 0);  // flips in data blocks pass the open (data is read lazily)
}

}  // namespace
}  // namespace forgedb
