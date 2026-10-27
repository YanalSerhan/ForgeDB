#include "db/table_cache.h"

#include <atomic>
#include <cstdint>

#include <gtest/gtest.h>

#include <memory>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include "forgedb/iterator.h"
#include "forgedb/result.h"
#include "forgedb/status.h"

#include "db/filename.h"
#include "db/internal_key.h"
#include "env/file_system.h"
#include "env/mem_file_system.h"
#include "table/table.h"
#include "table/table_builder.h"

namespace forgedb {
namespace {

constexpr std::string_view kDb = "db";

// "t<table>-<i>", i zero-padded to 3 digits so keys sort by i.
std::string UserKey(std::uint64_t table, int i) {
  const std::string digits = std::to_string(i);
  return "t" + std::to_string(table) + "-" + std::string(3 - digits.size(), '0') + digits;
}

std::string ValueOf(std::uint64_t table, int i) { return "value " + UserKey(table, i); }

class TableCacheTest : public ::testing::Test {
 protected:
  void SetUp() override { ASSERT_TRUE(fs_.CreateDir(std::string(kDb)).ok()); }

  // Writes table `number` with keys t<number>-000 ... t<number>-199; returns its size.
  std::uint64_t WriteTable(std::uint64_t number) {
    const std::unique_ptr<WritableFile> file =
        std::move(fs_.NewWritableFile(TableFileName(std::string(kDb), number))).value();
    TableBuilder builder(TableOptions{}, file.get());
    for (int i = 0; i < 200; ++i) {
      std::string key;
      AppendInternalKey(key, {.user_key = UserKey(number, i),
                              .sequence = static_cast<SequenceNumber>(i + 1),
                              .type = ValueType::kValue});
      builder.Add(key, ValueOf(number, i));
    }
    EXPECT_TRUE(builder.Finish().ok());
    EXPECT_TRUE(file->Close().ok());
    sizes_.resize(std::max<std::size_t>(sizes_.size(), number + 1));
    sizes_[number] = builder.FileSize();
    return builder.FileSize();
  }

  // Looks up key i of table `number` and checks the value.
  void ExpectFound(TableCache& cache, std::uint64_t number, int i) {
    std::string value;
    const Result<TableLookup> found = cache.Get(
        number, sizes_[number], LookupKey(UserKey(number, i), kMaxSequenceNumber), &value);
    ASSERT_TRUE(found.ok()) << found.status().ToString();
    ASSERT_EQ(*found, TableLookup::kFound);
    EXPECT_EQ(value, ValueOf(number, i));
  }

  MemFileSystem fs_;
  std::vector<std::uint64_t> sizes_;
};

TEST_F(TableCacheTest, OpensOnMissAndKeepsTablesOpen) {
  for (std::uint64_t n = 1; n <= 3; ++n) {
    WriteTable(n);
  }
  TableCache cache(&fs_, std::string(kDb), 10, nullptr);
  for (std::uint64_t n = 1; n <= 3; ++n) {
    ExpectFound(cache, n, 5);
  }
  EXPECT_EQ(cache.GetStats().misses, 3U);
  for (std::uint64_t n = 1; n <= 3; ++n) {
    ExpectFound(cache, n, 150);
  }
  EXPECT_EQ(cache.GetStats().misses, 3U);
  EXPECT_EQ(cache.GetStats().hits, 3U);
  EXPECT_EQ(cache.GetStats().entries, 3U);
}

TEST_F(TableCacheTest, ClosesTheLeastRecentlyUsedTable) {
  for (std::uint64_t n = 1; n <= 3; ++n) {
    WriteTable(n);
  }
  TableCache cache(&fs_, std::string(kDb), 2, nullptr);
  ExpectFound(cache, 1, 0);
  ExpectFound(cache, 2, 0);
  ExpectFound(cache, 1, 1);  // 2 is now the least recently used
  ExpectFound(cache, 3, 0);  // closes 2
  EXPECT_EQ(cache.GetStats().evictions, 1U);
  EXPECT_EQ(cache.GetStats().entries, 2U);
  const std::uint64_t misses = cache.GetStats().misses;
  ExpectFound(cache, 1, 2);
  EXPECT_EQ(cache.GetStats().misses, misses);  // 1 was still open
  ExpectFound(cache, 2, 2);
  EXPECT_EQ(cache.GetStats().misses, misses + 1);  // 2 had to be reopened
}

TEST_F(TableCacheTest, IteratorKeepsItsTableOpen) {
  const std::uint64_t size = WriteTable(7);
  TableCache cache(&fs_, std::string(kDb), 1, nullptr);
  Result<std::unique_ptr<Iterator>> it = cache.NewIterator(7, size);
  ASSERT_TRUE(it.ok()) << it.status().ToString();

  // The table is evicted and its file deleted, but the iterator still has it.
  cache.Evict(7);
  ASSERT_TRUE(fs_.RemoveFile(TableFileName(std::string(kDb), 7)).ok());
  EXPECT_EQ(cache.GetStats().entries, 0U);
  int count = 0;
  for ((*it)->SeekToFirst(); (*it)->Valid(); (*it)->Next()) {
    EXPECT_EQ(ExtractUserKey((*it)->key()), UserKey(7, count));
    ++count;
  }
  EXPECT_TRUE((*it)->status().ok());
  EXPECT_EQ(count, 200);

  // A new lookup has to open the (now missing) file again, and fails.
  std::string value;
  EXPECT_FALSE(cache.Get(7, size, LookupKey(UserKey(7, 0), kMaxSequenceNumber), &value).ok());
}

TEST_F(TableCacheTest, MissingFileOrWrongSizeIsAnError) {
  const std::uint64_t size = WriteTable(1);
  TableCache cache(&fs_, std::string(kDb), 10, nullptr);
  std::string value;
  const LookupKey key(UserKey(1, 0), kMaxSequenceNumber);
  EXPECT_FALSE(cache.Get(99, size, key, &value).ok());
  const Result<TableLookup> wrong_size = cache.Get(1, size + 1, key, &value);
  ASSERT_FALSE(wrong_size.ok());
  EXPECT_EQ(wrong_size.status().code(), Status::Code::kCorruption);
  EXPECT_EQ(cache.GetStats().entries, 0U);  // failed opens are not cached
  ExpectFound(cache, 1, 0);
}

TEST_F(TableCacheTest, TablesShareTheBlockCache) {
  WriteTable(1);
  WriteTable(2);
  BlockCache blocks(1 << 20);
  TableCache cache(&fs_, std::string(kDb), 10, &blocks);
  ExpectFound(cache, 1, 0);
  ExpectFound(cache, 2, 0);
  EXPECT_EQ(blocks.GetStats().entries, 2U);  // one block from each table
  ExpectFound(cache, 1, 0);
  EXPECT_EQ(blocks.GetStats().hits, 1U);
}

// Threads look up keys across more tables than may be open at once, so tables
// are opened, replaced and closed under them all the time.
TEST_F(TableCacheTest, ConcurrentLookups) {
  constexpr std::uint64_t kTables = 10;
  for (std::uint64_t n = 1; n <= kTables; ++n) {
    WriteTable(n);
  }
  BlockCache blocks(64 << 10);
  TableCache cache(&fs_, std::string(kDb), 3, &blocks);
  std::atomic<int> wrong{0};
  std::vector<std::thread> threads;
  threads.reserve(8);
  for (int t = 0; t < 8; ++t) {
    threads.emplace_back([this, &cache, &wrong, t] {
      std::string value;
      for (int i = 0; i < 3000; ++i) {
        const std::uint64_t n = 1 + static_cast<std::uint64_t>(((i * 7) + t) % kTables);
        const int k = ((i * 13) + t) % 200;
        const Result<TableLookup> found =
            cache.Get(n, sizes_[n], LookupKey(UserKey(n, k), kMaxSequenceNumber), &value);
        if (!found.ok() || *found != TableLookup::kFound || value != ValueOf(n, k)) {
          wrong.fetch_add(1);
        }
        if (i % 100 == 0) {
          Result<std::unique_ptr<Iterator>> it = cache.NewIterator(n, sizes_[n]);
          if (!it.ok()) {
            wrong.fetch_add(1);
            continue;
          }
          (*it)->Seek(std::string(UserKey(n, 100)) + std::string(8, '\xff'));
          if (!(*it)->Valid() || ExtractUserKey((*it)->key()) != UserKey(n, 100)) {
            wrong.fetch_add(1);
          }
        }
      }
    });
  }
  for (std::thread& thread : threads) {
    thread.join();
  }
  EXPECT_EQ(wrong.load(), 0);
  EXPECT_LE(cache.GetStats().entries, 3U);
}

}  // namespace
}  // namespace forgedb
