// Phase 1 exit gate: the DB agrees with std::map over 1,000,000 random
// operations, for several seeds. Since Phase 2.3 the runs also close and
// reopen the database (about five times per run), so recovery from the log
// is checked against the model too.
//
// The operation count is a build-time default (FORGEDB_MODEL_TEST_OPERATIONS,
// set in tests/CMakeLists.txt): 1,000,000 in optimized builds, 100,000 in
// debug and sanitizer builds, which run 20-50x slower. Override at run time:
//   FORGEDB_MODEL_OPS=5000000   operations per seed
//   FORGEDB_MODEL_SEED=17       run only this seed (e.g. to reproduce a failure)
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdio>

#include <gtest/gtest.h>

#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "forgedb/db.h"
#include "forgedb/iterator.h"
#include "forgedb/options.h"
#include "forgedb/result.h"
#include "forgedb/snapshot.h"
#include "forgedb/stats.h"
#include "forgedb/status.h"
#include "forgedb/write_batch.h"

#include "db/db_impl.h"
#include "db/version_set.h"
#include "env/mem_file_system.h"
#include "model/model_harness.h"
#include "support/test_env.h"

#ifndef FORGEDB_MODEL_TEST_OPERATIONS
#define FORGEDB_MODEL_TEST_OPERATIONS 100000
#endif

namespace forgedb::model {
namespace {

std::size_t OperationsPerSeed() {
  return static_cast<std::size_t>(GetEnvNumber("FORGEDB_MODEL_OPS", FORGEDB_MODEL_TEST_OPERATIONS));
}

std::vector<std::uint64_t> Seeds() {
  if (GetEnv("FORGEDB_MODEL_SEED")) {
    return {GetEnvNumber("FORGEDB_MODEL_SEED", 0)};
  }
  return {1, 2, 3};
}

class MemoryDBModelTest : public testing::TestWithParam<std::uint64_t> {};

// Statistics summed over every open of a run (DB::GetStats counts one open).
struct RunTotals {
  std::uint64_t opens = 0;
  std::uint64_t user_bytes = 0;
  std::uint64_t table_bytes = 0;  // written by flushes and compactions
  std::uint64_t flushes = 0;
  std::uint64_t compactions = 0;  // trivial moves included
};

// Forwards everything to a real DB, and adds its statistics to `totals`
// when it is closed.
class TotallingDB final : public DB {
 public:
  TotallingDB(std::unique_ptr<DB> real, std::shared_ptr<RunTotals> totals)
      : real_(std::move(real)), totals_(std::move(totals)) {}
  ~TotallingDB() override {
    const DBStats stats = real_->GetStats();
    ++totals_->opens;
    totals_->user_bytes += stats.user_bytes;
    totals_->table_bytes += stats.TableBytesWritten();
    totals_->flushes += stats.flushes;
    for (const LevelStats& level : stats.levels) {
      totals_->compactions += level.compactions.count + level.compactions.trivial_moves;
    }
  }
  TotallingDB(const TotallingDB&) = delete;
  TotallingDB& operator=(const TotallingDB&) = delete;
  TotallingDB(TotallingDB&&) = delete;
  TotallingDB& operator=(TotallingDB&&) = delete;

  Status Write(const WriteOptions& options, const WriteBatch& batch) override {
    return real_->Write(options, batch);
  }
  Result<std::string> Get(const ReadOptions& options, std::string_view key) override {
    return real_->Get(options, key);
  }
  std::unique_ptr<Iterator> NewIterator(const ReadOptions& options) override {
    return real_->NewIterator(options);
  }
  std::shared_ptr<const Snapshot> GetSnapshot() override { return real_->GetSnapshot(); }
  DBStats GetStats() override { return real_->GetStats(); }

 private:
  std::unique_ptr<DB> real_;
  std::shared_ptr<RunTotals> totals_;
};

Result<std::unique_ptr<DB>> OpenTotalling(const Options& options,
                                          const std::shared_ptr<MemFileSystem>& fs,
                                          const std::shared_ptr<RunTotals>& totals) {
  FORGEDB_ASSIGN_OR_RETURN(std::unique_ptr<DB> real, OpenDB(options, "model-test", fs));
  return std::unique_ptr<DB>(std::make_unique<TotallingDB>(std::move(real), totals));
}

TEST_P(MemoryDBModelTest, AgreesWithStdMap) {
  // Each run (and each replay while shrinking) starts on a new in-memory file
  // system; a reopen opens the same one again.
  const auto storage = std::make_shared<std::shared_ptr<MemFileSystem>>();
  const DbSetup setup(
      [storage] {
        *storage = std::make_shared<MemFileSystem>();
        return OpenDB(Options{}, "model-test", *storage);
      },
      [storage] { return OpenDB(Options{}, "model-test", *storage); });
  RunOptions options;
  options.seed = GetParam();
  options.operations = OperationsPerSeed();
  options.generator.expected_reopens = 5;
  EXPECT_EQ(RunModelTest(setup, options), "");
}

// TODO 3.9: the same with a small memtable, so data constantly moves from
// memtables to tables and reads merge many tables (and a reopen writes one
// more). The memtable is sized for about 100 flushes per run whatever the
// operation count (the generator writes about 66 bytes of memtable per
// operation, measured): without compaction (Phase 4) every flush adds a
// level-0 table that every read must consider. Flushes run in the background
// thread (4.2). Since 4.1 the run also takes snapshots (about 200, in 4
// slots) and reads at them, compared with copies of the model, while their
// data moves into tables.
TEST_P(MemoryDBModelTest, AgreesWithStdMapAcrossFlushes) {
  Options db_options;
  db_options.memtable_bytes = std::max(std::size_t{16} << 10, OperationsPerSeed() * 64 / 100);
  db_options.block_cache_bytes = std::size_t{64} << 10;  // small: misses and evictions too
  // No compaction: every table stays in level 0 (AgreesWithStdMapAcrossCompactions
  // below compacts).
  db_options.level0_compaction_trigger = 1 << 30;
  db_options.level0_slowdown_writes_trigger = 1 << 30;
  db_options.level0_stop_writes_trigger = 1 << 30;
  const auto storage = std::make_shared<std::shared_ptr<MemFileSystem>>();
  const DbSetup setup(
      [storage, db_options] {
        *storage = std::make_shared<MemFileSystem>();
        return OpenDB(db_options, "model-test", *storage);
      },
      [storage, db_options] { return OpenDB(db_options, "model-test", *storage); });
  RunOptions options;
  options.seed = GetParam();
  options.operations = OperationsPerSeed();
  options.generator.expected_reopens = 5;
  options.generator.snapshot_slots = 4;
  options.generator.expected_snapshots = 200;
  EXPECT_EQ(RunModelTest(setup, options), "");

  // The run must really have flushed: count the tables it left.
  const Result<std::vector<std::string>> children = (*storage)->GetChildren("model-test");
  ASSERT_TRUE(children.ok());
  const auto tables = std::ranges::count_if(
      *children, [](const std::string& name) { return name.ends_with(".sst"); });
  std::printf("[ model    ] seed %llu: %lld tables at the end\n",  // NOLINT(*-vararg)
              static_cast<unsigned long long>(GetParam()), static_cast<long long>(tables));
  if (options.operations >= 10000) {
    EXPECT_GT(tables, 30);
  }
}

// TODO 4.3/4.5: the same with compaction and tiny levels, so data moves
// through four or more levels while snapshots hold old versions: the drop
// rules, the level iterators and the one-table-per-level Get are all checked
// against the model. About 250 flushes per run; level 1 holds one
// memtable's worth, and tables are a quarter of a memtable.
TEST_P(MemoryDBModelTest, AgreesWithStdMapAcrossCompactions) {
  Options db_options;
  db_options.memtable_bytes = std::max(std::size_t{8} << 10, OperationsPerSeed() * 64 / 250);
  db_options.block_cache_bytes = std::size_t{64} << 10;
  db_options.level0_compaction_trigger = 2;
  db_options.max_bytes_for_level_base = db_options.memtable_bytes;
  db_options.target_file_size = db_options.memtable_bytes / 4;
  db_options.block_size = 1024;
  const auto storage = std::make_shared<std::shared_ptr<MemFileSystem>>();
  const auto totals = std::make_shared<RunTotals>();
  const DbSetup setup(
      [storage, totals, db_options] {
        *storage = std::make_shared<MemFileSystem>();
        *totals = RunTotals{};  // a new run (or a replay while shrinking)
        return OpenTotalling(db_options, *storage, totals);
      },
      [storage, totals, db_options] { return OpenTotalling(db_options, *storage, totals); });
  RunOptions options;
  options.seed = GetParam();
  options.operations = OperationsPerSeed();
  options.generator.expected_reopens = 5;
  options.generator.snapshot_slots = 4;
  options.generator.expected_snapshots = 200;
  EXPECT_EQ(RunModelTest(setup, options), "");

  // Measured, not checked: with these tiny levels it says little about real
  // settings. Tables written at reopen by recovery are not counted.
  EXPECT_GT(totals->user_bytes, 0U);
  std::printf(  // NOLINT(*-vararg)
      "[ model    ] seed %llu: write amplification %.2f (%llu table bytes / %llu user bytes; "
      "%llu flushes, %llu compactions, %llu opens)\n",
      static_cast<unsigned long long>(GetParam()),
      static_cast<double>(totals->table_bytes) /
          static_cast<double>(std::max<std::uint64_t>(totals->user_bytes, 1)),
      static_cast<unsigned long long>(totals->table_bytes),
      static_cast<unsigned long long>(totals->user_bytes),
      static_cast<unsigned long long>(totals->flushes),
      static_cast<unsigned long long>(totals->compactions),
      static_cast<unsigned long long>(totals->opens));

  // The data really went deep: open the storage once more and look.
  const Result<std::unique_ptr<DB>> db = OpenDB(db_options, "model-test", *storage);
  ASSERT_TRUE(db.ok()) << db.status().ToString();
  auto& impl = dynamic_cast<DBImpl&>(**db);
  impl.WaitForBackgroundWorkForTesting();
  const std::shared_ptr<const Version> version = impl.CurrentVersion();
  int deepest = 0;
  std::string levels;
  for (std::size_t level = 0; level < version->files.size(); ++level) {
    levels += " " + std::to_string(version->files[level].size());
    if (!version->files[level].empty()) {
      deepest = static_cast<int>(level);
    }
  }
  std::printf("[ model    ] seed %llu: tables per level:%s\n",  // NOLINT(*-vararg)
              static_cast<unsigned long long>(GetParam()), levels.c_str());
  if (options.operations >= 10000) {
    EXPECT_GE(deepest, 3);
  }
}

INSTANTIATE_TEST_SUITE_P(Seeds, MemoryDBModelTest, testing::ValuesIn(Seeds()));

}  // namespace
}  // namespace forgedb::model
