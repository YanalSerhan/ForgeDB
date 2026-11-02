// The engine adapters of forgedb_bench (TODO 5.1b): every engine keeps the
// Engine contract (tools/bench/engine.h) on real files. Built only with
// FORGEDB_BENCH_ENGINES (the msvc-bench preset), which has RocksDB, LevelDB
// and SQLite; ForgeDB runs through the same tests.
#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>

#include <gtest/gtest.h>

#include <map>
#include <memory>
#include <ostream>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include "forgedb/iterator.h"
#include "forgedb/result.h"
#include "forgedb/status.h"

#include "tools/bench/engine.h"
#include "tools/bench/forgedb_engine.h"
#include "tools/bench/leveldb_engine.h"
#include "tools/bench/rocksdb_engine.h"
#include "tools/bench/sqlite_engine.h"

namespace forgedb::bench {
namespace {

struct EngineCase {
  std::string_view name;
  DiskEngineOpener open;
  // A counter that grows with every write.
  std::string_view write_counter;
};

void PrintTo(const EngineCase& engine, std::ostream* out) { *out << engine.name; }

class BenchEngineTest : public ::testing::TestWithParam<EngineCase> {
 protected:
  void SetUp() override {
    const std::string test = ::testing::UnitTest::GetInstance()->current_test_info()->name();
    dir_ = std::filesystem::temp_directory_path() /
           ("forgedb_engine_test_" + std::string(GetParam().name) + "_" +
            test.substr(0, test.find('/')));
    std::filesystem::remove_all(dir_);
  }

  void TearDown() override {
    engine_.reset();
    std::filesystem::remove_all(dir_);
  }

  // Opens engine_; use as ASSERT_TRUE(Open(...)).
  ::testing::AssertionResult Open(const EngineOptions& options) {
    Result<std::unique_ptr<Engine>> engine = GetParam().open(dir_, options);
    if (!engine.ok()) {
      return ::testing::AssertionFailure() << engine.status().ToString();
    }
    engine_ = std::move(engine).value();
    return ::testing::AssertionSuccess();
  }

  static std::string Get(Engine& engine, std::string_view key) {
    std::string value;
    const Result<bool> found = engine.Get(key, value);
    EXPECT_TRUE(found.ok()) << found.status().ToString();
    return found.ok() && *found ? value : "<missing>";
  }

  static std::string Key(int i) {
    const std::string key = std::to_string(i);
    return std::string(16 - key.size(), '0') + key;
  }

  std::filesystem::path dir_;
  std::unique_ptr<Engine> engine_;
};

TEST_P(BenchEngineTest, PutGetOverwriteAndMissing) {
  ASSERT_TRUE(Open({}));
  Engine& engine = *engine_;
  ASSERT_TRUE(engine.Put("apple", "red").ok());
  ASSERT_TRUE(engine.Put("banana", "yellow").ok());
  ASSERT_TRUE(engine.Put("empty", "").ok());
  EXPECT_EQ(Get(engine, "apple"), "red");
  EXPECT_EQ(Get(engine, "banana"), "yellow");
  EXPECT_EQ(Get(engine, "empty"), "") << "an empty value is a value";
  EXPECT_EQ(Get(engine, "cherry"), "<missing>");
  ASSERT_TRUE(engine.Put("apple", "green").ok());
  EXPECT_EQ(Get(engine, "apple"), "green");

  const std::string binary("k\0\xff", 3);
  ASSERT_TRUE(engine.Put(binary, std::string("v\0v", 3)).ok());
  EXPECT_EQ(Get(engine, binary), std::string("v\0v", 3));
}

TEST_P(BenchEngineTest, IteratorScansInKeyOrderAndSeeks) {
  ASSERT_TRUE(Open({}));
  Engine& engine = *engine_;
  {
    const std::unique_ptr<Iterator> it = engine.NewIterator();
    it->SeekToFirst();
    EXPECT_FALSE(it->Valid()) << "empty database";
    EXPECT_TRUE(it->status().ok());
  }
  for (const std::string_view key : {"k3", "k1", "k2", "k20"}) {
    ASSERT_TRUE(engine.Put(key, std::string("v") + std::string(key)).ok());
  }
  ASSERT_TRUE(engine.Put(std::string("k\xff", 2), "high").ok());  // bytewise: after "k3"

  const std::unique_ptr<Iterator> it = engine.NewIterator();
  std::vector<std::string> keys;
  for (it->SeekToFirst(); it->Valid(); it->Next()) {
    keys.emplace_back(it->key());
  }
  EXPECT_TRUE(it->status().ok()) << it->status().ToString();
  EXPECT_EQ(keys, (std::vector<std::string>{"k1", "k2", "k20", "k3", std::string("k\xff", 2)}));

  it->Seek("k15");
  ASSERT_TRUE(it->Valid());
  EXPECT_EQ(it->key(), "k2");
  EXPECT_EQ(it->value(), "vk2");
  it->Next();
  ASSERT_TRUE(it->Valid());
  EXPECT_EQ(it->key(), "k20");
  it->Seek("k2");
  ASSERT_TRUE(it->Valid());
  EXPECT_EQ(it->key(), "k2") << "Seek lands on an equal key";
  it->Seek("l");
  EXPECT_FALSE(it->Valid());
  EXPECT_TRUE(it->status().ok());
}

// Enough data, with a small write buffer, for flushes and compactions (in
// SQLite, checkpoints); then the background work settles and nothing is lost.
TEST_P(BenchEngineTest, ManyWritesSettleAndStayReadable) {
  EngineOptions options;
  options.cache_bytes = std::uint64_t{1} << 20U;
  options.memtable_bytes = std::uint64_t{64} << 10U;
  ASSERT_TRUE(Open(options));
  Engine& engine = *engine_;
  const std::map<std::string, double> before = engine.Counters();
  constexpr int kKeys = 20000;
  const std::string value(100, 'x');
  for (int i = 0; i < kKeys; ++i) {
    ASSERT_TRUE(engine.Put(Key(i), value + std::to_string(i)).ok());
  }
  ASSERT_TRUE(engine.WaitForBackgroundWork().ok());
  // Settled means settled: nothing more happens on its own.
  const std::map<std::string, double> settled = engine.Counters();
  std::this_thread::sleep_for(std::chrono::milliseconds(500));
  EXPECT_EQ(engine.Counters(), settled);
  for (int i = 0; i < kKeys; i += 97) {
    EXPECT_EQ(Get(engine, Key(i)), value + std::to_string(i));
  }

  const Result<std::uint64_t> bytes = engine.DiskBytes();
  ASSERT_TRUE(bytes.ok()) << bytes.status().ToString();
  EXPECT_GT(*bytes, std::uint64_t{kKeys} * 100) << "at least the values are on disk";

  const std::map<std::string, double> after = engine.Counters();
  const std::string counter(GetParam().write_counter);
  ASSERT_TRUE(after.contains(counter)) << counter;
  EXPECT_GT(after.at(counter), before.contains(counter) ? before.at(counter) : 0.0) << counter;

  // Settling again with nothing new to do returns.
  ASSERT_TRUE(engine.WaitForBackgroundWork().ok());
}

TEST_P(BenchEngineTest, SyncedWrites) {
  EngineOptions options;
  options.sync = true;
  ASSERT_TRUE(Open(options));
  Engine& engine = *engine_;
  for (int i = 0; i < 20; ++i) {
    ASSERT_TRUE(engine.Put(Key(i), "synced").ok());
  }
  EXPECT_EQ(Get(engine, Key(19)), "synced");
}

// Readers and an iterator on several threads while one thread writes (for
// SQLite, several pooled connections at once).
TEST_P(BenchEngineTest, ConcurrentReadersAndAWriter) {
  ASSERT_TRUE(Open({}));
  Engine& engine = *engine_;
  constexpr int kKeys = 2000;
  for (int i = 0; i < kKeys; ++i) {
    ASSERT_TRUE(engine.Put(Key(i), "old").ok());
  }
  std::atomic<int> wrong{0};
  std::atomic<int> errors{0};
  std::vector<std::thread> threads;
  threads.emplace_back([&] {
    for (int i = 0; i < kKeys; ++i) {
      if (!engine.Put(Key(i), "new").ok()) {
        ++errors;
      }
    }
  });
  for (int t = 0; t < 3; ++t) {
    threads.emplace_back([&, t] {
      std::string value;
      for (int i = t; i < kKeys; i += 3) {
        const Result<bool> found = engine.Get(Key(i), value);
        if (!found.ok()) {
          ++errors;
        } else if (!*found || (value != "old" && value != "new")) {
          ++wrong;
        }
      }
    });
  }
  threads.emplace_back([&] {
    const std::unique_ptr<Iterator> it = engine.NewIterator();
    int seen = 0;
    for (it->SeekToFirst(); it->Valid(); it->Next()) {
      ++seen;
    }
    if (!it->status().ok()) {
      ++errors;
    } else if (seen != kKeys) {
      ++wrong;
    }
  });
  for (std::thread& thread : threads) {
    thread.join();
  }
  EXPECT_EQ(errors.load(), 0);
  EXPECT_EQ(wrong.load(), 0);
  EXPECT_EQ(Get(engine, Key(kKeys - 1)), "new");
}

INSTANTIATE_TEST_SUITE_P(
    Engines, BenchEngineTest,
    ::testing::Values(
        EngineCase{.name = "forgedb", .open = OpenForgeDBOnDisk, .write_counter = "user_bytes"},
        EngineCase{
            .name = "rocksdb", .open = OpenRocksDB, .write_counter = "rocksdb.bytes.written"},
        EngineCase{.name = "leveldb", .open = OpenLevelDB, .write_counter = "table_bytes"},
        EngineCase{.name = "sqlite", .open = OpenSQLite, .write_counter = "cache_writes"}),
    [](const ::testing::TestParamInfo<EngineCase>& info) { return std::string(info.param.name); });

}  // namespace
}  // namespace forgedb::bench
