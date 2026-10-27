#include "table/block.h"

#include <compare>
#include <cstddef>
#include <cstdint>

#include <gtest/gtest.h>

#include <memory>
#include <random>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "forgedb/iterator.h"
#include "forgedb/status.h"

#include "db/internal_key.h"
#include "table/block_builder.h"
#include "table/block_format.h"
#include "util/coding.h"
#include "util/crc32c.h"

namespace forgedb {
namespace {

std::strong_ordering Bytewise(std::string_view a, std::string_view b) noexcept {
  return a.compare(b) <=> 0;
}

using Entries = std::vector<std::pair<std::string, std::string>>;

std::string Build(const Entries& entries, std::size_t restart_interval = 16) {
  BlockBuilder builder(&Bytewise, restart_interval);
  for (const auto& [key, value] : entries) {
    builder.Add(key, value);
  }
  return std::string(builder.Finish());
}

Block OpenOrDie(std::string contents) {
  Result<Block> block = Block::Open(std::move(contents));
  EXPECT_TRUE(block.ok()) << block.status().ToString();
  return std::move(block).value();
}

Entries ReadAll(const Block& block) {
  Entries result;
  const std::unique_ptr<Iterator> it = block.NewIterator(&Bytewise);
  for (it->SeekToFirst(); it->Valid(); it->Next()) {
    result.emplace_back(it->key(), it->value());
  }
  EXPECT_TRUE(it->status().ok()) << it->status().ToString();
  return result;
}

// Keys "k00000", "k00002", ...: even numbers, so odd ones fall between them.
std::string NumberedKey(int n) {
  const std::string key = std::to_string(n);
  return "k" + std::string(5 - key.size(), '0') + key;
}

Entries NumberedEntries(int count) {
  Entries entries;
  for (int i = 0; i < count; ++i) {
    entries.emplace_back(NumberedKey(2 * i), "value" + std::to_string(i));
  }
  return entries;
}

std::uint32_t RestartCount(std::string_view contents) {
  return DecodeFixed32(contents.data() + contents.size() - 4);
}

// Where the restart array starts: the end of the entries.
std::size_t RestartArrayOffset(std::string_view contents) {
  return contents.size() - 4 - (std::size_t{RestartCount(contents)} * 4);
}

std::uint32_t RestartAt(std::string_view contents, std::uint32_t index) {
  return DecodeFixed32(contents.data() + RestartArrayOffset(contents) + (std::size_t{index} * 4));
}

void ExpectCorruption(const Status& status, std::string_view message_part) {
  EXPECT_TRUE(status.code() == Status::Code::kCorruption) << status.ToString();
  EXPECT_NE(status.ToString().find(message_part), std::string::npos) << status.ToString();
}

// ---------------------------------------------------------------------------
// Building and reading
// ---------------------------------------------------------------------------

TEST(BlockTest, EmptyBlock) {
  BlockBuilder builder(&Bytewise);
  EXPECT_TRUE(builder.empty());
  const std::string contents(builder.Finish());
  EXPECT_EQ(contents.size(), 8U);  // one restart point (offset 0) and the count
  EXPECT_EQ(RestartCount(contents), 1U);

  const Block block = OpenOrDie(contents);
  const std::unique_ptr<Iterator> it = block.NewIterator(&Bytewise);
  it->SeekToFirst();
  EXPECT_FALSE(it->Valid());
  it->Seek("anything");
  EXPECT_FALSE(it->Valid());
  it->Seek("");
  EXPECT_FALSE(it->Valid());
  EXPECT_TRUE(it->status().ok());
}

// The example in docs/file-formats.md, "Block".
TEST(BlockTest, FormatIsStable) {
  using namespace std::string_literals;
  EXPECT_EQ(Build({{"ab", "1"}, {"ac", "2"}}),
            "\x00\x02\x01"
            "ab1"
            "\x01\x01\x01"
            "c2"
            "\x00\x00\x00\x00"
            "\x01\x00\x00\x00"s);
}

TEST(BlockTest, SingleEntry) {
  const Entries entries = {{"key", "value"}};
  const Block block = OpenOrDie(Build(entries));
  EXPECT_EQ(ReadAll(block), entries);

  const std::unique_ptr<Iterator> it = block.NewIterator(&Bytewise);
  it->Seek("a");
  ASSERT_TRUE(it->Valid());
  EXPECT_EQ(it->key(), "key");
  EXPECT_EQ(it->value(), "value");
  it->Seek("key");
  ASSERT_TRUE(it->Valid());
  EXPECT_EQ(it->key(), "key");
  it->Seek("key\x01");
  EXPECT_FALSE(it->Valid());
  EXPECT_TRUE(it->status().ok());
}

TEST(BlockTest, EmptyKeysAndValues) {
  const Entries entries = {{"", ""}, {"a", ""}, {"b", "x"}, {"c", std::string(100000, 'v')}};
  const Block block = OpenOrDie(Build(entries, 2));
  EXPECT_EQ(ReadAll(block), entries);

  const std::unique_ptr<Iterator> it = block.NewIterator(&Bytewise);
  it->Seek("");
  ASSERT_TRUE(it->Valid());
  EXPECT_EQ(it->key(), "");
  it->Seek("bb");
  ASSERT_TRUE(it->Valid());
  EXPECT_EQ(it->key(), "c");
  EXPECT_EQ(it->value().size(), 100000U);
}

TEST(BlockTest, SharedPrefixesAreCompressed) {
  const std::string prefix(200, 'p');
  Entries entries;
  for (int i = 0; i < 64; ++i) {
    entries.emplace_back(prefix + NumberedKey(i), "v");
  }
  const std::string contents = Build(entries);
  EXPECT_EQ(RestartCount(contents), 4U);
  // Only the 4 restart points store the 200-byte prefix.
  EXPECT_LT(contents.size(), (4 * prefix.size()) + (std::size_t{64} * 16));
  EXPECT_EQ(ReadAll(OpenOrDie(contents)), entries);
}

TEST(BlockTest, KeysThatArePrefixesOfEachOther) {
  const Entries entries = {{"a", "1"}, {"ab", "2"}, {"abc", "3"}, {"abd", "4"}, {"b", "5"}};
  for (const std::size_t interval : {1, 2, 16}) {
    const Block block = OpenOrDie(Build(entries, interval));
    EXPECT_EQ(ReadAll(block), entries) << interval;
    const std::unique_ptr<Iterator> it = block.NewIterator(&Bytewise);
    it->Seek("abb");
    ASSERT_TRUE(it->Valid());
    EXPECT_EQ(it->key(), "abc");
  }
}

// Every count around a restart boundary, with several intervals: the restart
// count, a full scan, and a Seek to every key, between keys, before the first
// and after the last.
TEST(BlockTest, RestartBoundaries) {
  for (const std::size_t interval : {1, 2, 3, 16}) {
    for (const int count : {1, 2, 15, 16, 17, 31, 32, 33, 100}) {
      SCOPED_TRACE("interval " + std::to_string(interval) + ", count " + std::to_string(count));
      const Entries entries = NumberedEntries(count);
      const std::string contents = Build(entries, interval);
      EXPECT_EQ(RestartCount(contents), (count + interval - 1) / interval);

      const Block block = OpenOrDie(contents);
      EXPECT_EQ(ReadAll(block), entries);

      const std::unique_ptr<Iterator> it = block.NewIterator(&Bytewise);
      for (int i = 0; i < count; ++i) {
        it->Seek(NumberedKey(2 * i));  // exact
        ASSERT_TRUE(it->Valid());
        EXPECT_EQ(it->key(), entries[i].first);
        EXPECT_EQ(it->value(), entries[i].second);

        it->Seek(NumberedKey((2 * i) - 1));  // just before: lands on key i
        ASSERT_TRUE(it->Valid());
        EXPECT_EQ(it->key(), entries[i].first);

        // Next from a Seek continues in order.
        it->Seek(entries[i].first);
        it->Next();
        if (i + 1 < count) {
          ASSERT_TRUE(it->Valid());
          EXPECT_EQ(it->key(), entries[i + 1].first);
        } else {
          EXPECT_FALSE(it->Valid());
        }
      }
      it->Seek("");
      ASSERT_TRUE(it->Valid());
      EXPECT_EQ(it->key(), entries.front().first);
      it->Seek(NumberedKey(2 * count));
      EXPECT_FALSE(it->Valid());
      EXPECT_TRUE(it->status().ok());
    }
  }
}

TEST(BlockTest, ResetStartsANewBlock) {
  BlockBuilder builder(&Bytewise, 2);
  builder.Add("x", "1");
  builder.Add("y", "2");
  builder.Add("z", "3");
  const std::string first(builder.Finish());
  EXPECT_EQ(builder.CurrentSizeEstimate(), first.size());

  builder.Reset();
  EXPECT_TRUE(builder.empty());
  builder.Add("a", "1");  // smaller than the previous block's keys: fine after Reset
  const std::size_t estimate = builder.CurrentSizeEstimate();
  const std::string second(builder.Finish());
  EXPECT_EQ(estimate, second.size());
  EXPECT_EQ(ReadAll(OpenOrDie(second)), (Entries{{"a", "1"}}));
  EXPECT_EQ(ReadAll(OpenOrDie(first)), (Entries{{"x", "1"}, {"y", "2"}, {"z", "3"}}));
}

// Data blocks hold internal keys: several versions of one user key, newest
// first. A seek to (key, S) finds the newest version visible at S.
TEST(BlockTest, InternalKeysSeekToTheVisibleVersion) {
  BlockBuilder builder(&CompareInternalKeys);
  auto internal_key = [](std::string_view user_key, SequenceNumber sequence) {
    std::string key;
    AppendInternalKey(key, {.user_key = user_key, .sequence = sequence, .type = ValueType::kValue});
    return key;
  };
  builder.Add(internal_key("apple", 9), "apple@9");
  builder.Add(internal_key("apple", 5), "apple@5");
  builder.Add(internal_key("apple", 1), "apple@1");
  builder.Add(internal_key("banana", 7), "banana@7");
  const Block block = OpenOrDie(std::string(builder.Finish()));

  const std::unique_ptr<Iterator> it = block.NewIterator(&CompareInternalKeys);
  it->Seek(internal_key("apple", 6));
  ASSERT_TRUE(it->Valid());
  EXPECT_EQ(it->value(), "apple@5");
  it->Seek(internal_key("apple", 100));
  ASSERT_TRUE(it->Valid());
  EXPECT_EQ(it->value(), "apple@9");
  it->Seek(internal_key("apple", 0));
  ASSERT_TRUE(it->Valid());
  EXPECT_EQ(it->value(), "banana@7");
}

// ---------------------------------------------------------------------------
// Trailer
// ---------------------------------------------------------------------------

std::string WithTrailer(std::string_view contents) {
  std::string stored(contents);
  const auto trailer = MakeBlockTrailer(contents);
  stored.append(trailer.data(), trailer.size());
  return stored;
}

TEST(BlockTrailerTest, RoundTrip) {
  const std::string contents = Build(NumberedEntries(40));
  const std::string stored = WithTrailer(contents);
  ASSERT_EQ(stored.size(), contents.size() + kBlockTrailerSize);
  EXPECT_EQ(stored[contents.size()], '\0');  // type: uncompressed
  const Result<std::string_view> checked = CheckBlockTrailer(stored);
  ASSERT_TRUE(checked.ok()) << checked.status().ToString();
  EXPECT_EQ(*checked, contents);
  EXPECT_EQ(checked->data(), stored.data());  // a view into the input
}

TEST(BlockTrailerTest, StoredCrcIsMaskedAndCoversTheType) {
  const std::string contents = "some bytes";
  const std::string stored = WithTrailer(contents);
  std::string covered = contents;
  covered.push_back('\0');
  EXPECT_EQ(DecodeFixed32(&stored[contents.size() + 1]), crc32c::Mask(crc32c::Value(covered)));
}

TEST(BlockTrailerTest, EveryBitFlipIsDetected) {
  const std::string stored = WithTrailer(Build(NumberedEntries(5)));
  for (std::size_t byte = 0; byte < stored.size(); ++byte) {
    for (int bit = 0; bit < 8; ++bit) {
      std::string damaged = stored;
      damaged[byte] = static_cast<char>(damaged[byte] ^ (1 << bit));
      const Result<std::string_view> checked = CheckBlockTrailer(damaged);
      ASSERT_FALSE(checked.ok()) << "byte " << byte << " bit " << bit;
      EXPECT_TRUE(checked.status().code() == Status::Code::kCorruption);
    }
  }
}

TEST(BlockTrailerTest, TooShort) {
  for (std::size_t size = 0; size < kBlockTrailerSize; ++size) {
    ExpectCorruption(CheckBlockTrailer(std::string(size, '\0')).status(), "too short");
  }
  // Exactly a trailer, no contents: a valid (empty) stored block.
  EXPECT_TRUE(CheckBlockTrailer(WithTrailer("")).ok());
}

TEST(BlockTrailerTest, UnknownTypeWithMatchingCrc) {
  // Type 1 does not exist (yet), so build its trailer by hand.
  std::string stored = "abc";
  stored.push_back('\x01');
  PutFixed32(stored, crc32c::Mask(crc32c::Value(stored)));
  ExpectCorruption(CheckBlockTrailer(stored).status(), "unknown block type: 1");
}

// ---------------------------------------------------------------------------
// Malformed contents
// ---------------------------------------------------------------------------

// Rewrites restart point `index` (or, with index == count, the count itself).
void SetRestartField(std::string& contents, std::uint32_t index, std::uint32_t value) {
  EncodeFixed32(&contents[RestartArrayOffset(contents) + (std::size_t{index} * 4)], value);
}

TEST(BlockCorruptionTest, TooShortForTheRestartCount) {
  for (std::size_t size = 0; size < 4; ++size) {
    ExpectCorruption(Block::Open(std::string(size, '\0')).status(), "too short");
  }
}

TEST(BlockCorruptionTest, NoRestartPoints) {
  std::string contents = Build(NumberedEntries(3));
  EncodeFixed32(&contents[contents.size() - 4], 0);
  ExpectCorruption(Block::Open(contents).status(), "no restart points");
}

TEST(BlockCorruptionTest, RestartCountLargerThanTheBlock) {
  const std::string valid = Build(NumberedEntries(3));
  for (const std::uint32_t count :
       {static_cast<std::uint32_t>(valid.size() / 4), 0x40000000U, 0xFFFFFFFFU}) {
    std::string contents = valid;
    EncodeFixed32(&contents[contents.size() - 4], count);
    ExpectCorruption(Block::Open(contents).status(), "restart count larger than the block");
  }
}

TEST(BlockCorruptionTest, FirstRestartNotAtZero) {
  std::string contents = Build(NumberedEntries(20));
  SetRestartField(contents, 0, 1);
  ExpectCorruption(Block::Open(contents).status(), "first restart point is not at offset 0");
}

TEST(BlockCorruptionTest, RestartsOutOfOrder) {
  const std::string valid = Build(NumberedEntries(40), 4);  // 10 restart points
  {
    std::string contents = valid;
    SetRestartField(contents, 2, 0);  // not after restart 1
    ExpectCorruption(Block::Open(contents).status(), "out of order or outside");
  }
  {
    std::string contents = valid;
    SetRestartField(contents, 4, RestartAt(contents, 3));  // equal to restart 3
    ExpectCorruption(Block::Open(contents).status(), "out of order or outside");
  }
}

TEST(BlockCorruptionTest, RestartOutsideTheEntries) {
  std::string contents = Build(NumberedEntries(40), 4);
  const auto entries_end = static_cast<std::uint32_t>(RestartArrayOffset(contents));
  for (const std::uint32_t offset : {entries_end, entries_end + 1, 0xFFFFFFFFU}) {
    SetRestartField(contents, 9, offset);
    ExpectCorruption(Block::Open(contents).status(), "out of order or outside");
  }
}

TEST(BlockCorruptionTest, EmptyBlockWithTwoRestarts) {
  std::string contents;
  PutFixed32(contents, 0);
  PutFixed32(contents, 0);
  PutFixed32(contents, 2);
  ExpectCorruption(Block::Open(contents).status(), "empty block has more than one");
}

// Corrupt entries pass Open (it checks only the restart array) and are caught
// by the iterator.
Status ScanStatus(const Block& block) {
  const std::unique_ptr<Iterator> it = block.NewIterator(&Bytewise);
  for (it->SeekToFirst(); it->Valid(); it->Next()) {
  }
  return it->status();
}

TEST(BlockCorruptionTest, RestartEntryThatSharesBytes) {
  std::string contents = Build(NumberedEntries(3));
  contents[0] = 1;  // the first entry's `shared`
  const Block block = OpenOrDie(contents);
  ExpectCorruption(ScanStatus(block), "restart point does not store its full key");

  const std::unique_ptr<Iterator> it = block.NewIterator(&Bytewise);
  it->Seek("k");
  EXPECT_FALSE(it->Valid());
  ExpectCorruption(it->status(), "restart point does not store its full key");
}

TEST(BlockCorruptionTest, RestartEntryCheckedDuringSeekBinarySearch) {
  std::string contents = Build(NumberedEntries(64), 16);  // 4 restart points
  // Find restart 2 and make its entry claim a shared prefix.
  contents[RestartAt(contents, 2)] = 1;
  const Block block = OpenOrDie(contents);
  const std::unique_ptr<Iterator> it = block.NewIterator(&Bytewise);
  it->Seek(NumberedKey(100));
  EXPECT_FALSE(it->Valid());
  ExpectCorruption(it->status(), "restart point does not store its full key");
}

TEST(BlockCorruptionTest, EntryLongerThanTheBlock) {
  std::string contents = Build({{"a", "1"}, {"b", "2"}});
  // The first entry is [0][1][1]['a']['1']; make its value length 100.
  contents[2] = 100;
  ExpectCorruption(ScanStatus(OpenOrDie(contents)), "truncated block entry");
}

TEST(BlockCorruptionTest, SharedLongerThanThePreviousKey) {
  std::string contents = Build({{"a", "1"}, {"ab", "2"}}, 16);
  // The second entry starts at offset 5: [1][1][1]['b']['2']; claim 5 shared bytes.
  ASSERT_EQ(contents[5], 1);
  contents[5] = 5;
  ExpectCorruption(ScanStatus(OpenOrDie(contents)), "shares more bytes than the previous key");
}

TEST(BlockCorruptionTest, RestartPointInsideAnEntry) {
  std::string contents = Build(NumberedEntries(4), 2);       // restarts at entries 0 and 2
  SetRestartField(contents, 1, RestartAt(contents, 1) - 1);  // now inside entry 1
  ExpectCorruption(ScanStatus(OpenOrDie(contents)), "restart point inside an entry");
}

TEST(BlockCorruptionTest, ErrorsAreSticky) {
  std::string contents = Build(NumberedEntries(3));
  contents[0] = 1;
  const Block block = OpenOrDie(contents);
  const std::unique_ptr<Iterator> it = block.NewIterator(&Bytewise);
  it->SeekToFirst();
  ASSERT_FALSE(it->status().ok());
  it->SeekToFirst();
  EXPECT_FALSE(it->Valid());
  it->Seek("");
  EXPECT_FALSE(it->Valid());
  EXPECT_FALSE(it->status().ok());
}

// Random damage to the contents (the CRC would normally catch it; this tests
// the parser alone): Open fails or the block can be scanned and searched
// without reading outside it (ASan builds check that) and without looping.
TEST(BlockCorruptionTest, RandomDamageNeverReadsOutOfBounds) {
  std::mt19937_64 random(20261027);
  Entries entries;
  for (int i = 0; i < 50; ++i) {
    entries.emplace_back(NumberedKey(2 * i) + std::string(i % 7, 'x'), std::string(i % 13, 'v'));
  }
  const std::string valid = Build(entries, 4);
  int opened = 0;
  for (int round = 0; round < 3000; ++round) {
    std::string contents = valid;
    const int flips = 1 + static_cast<int>(random() % 4);
    for (int f = 0; f < flips; ++f) {
      const std::size_t at = random() % contents.size();
      contents[at] = static_cast<char>(random());
    }
    if (random() % 8 == 0) {
      contents.resize(random() % (contents.size() + 1));
    }
    const Result<Block> block = Block::Open(std::move(contents));
    if (!block.ok()) {
      EXPECT_TRUE(block.status().code() == Status::Code::kCorruption);
      continue;
    }
    ++opened;
    const std::unique_ptr<Iterator> it = block->NewIterator(&Bytewise);
    std::size_t steps = 0;
    for (it->SeekToFirst(); it->Valid(); it->Next()) {
      ASSERT_LE(++steps, valid.size());
    }
    for (const auto& entry : entries) {
      it->Seek(entry.first);
    }
    EXPECT_TRUE(it->status().ok() || it->status().code() == Status::Code::kCorruption);
  }
  EXPECT_GT(opened, 0);
}

}  // namespace
}  // namespace forgedb
