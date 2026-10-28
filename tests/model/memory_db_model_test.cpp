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
#include <vector>

#include "forgedb/db.h"

#include "db/db_impl.h"
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
// level-0 table that every read must consider.
TEST_P(MemoryDBModelTest, AgreesWithStdMapAcrossFlushes) {
  Options db_options;
  db_options.memtable_bytes = std::max(std::size_t{16} << 10, OperationsPerSeed() * 64 / 100);
  db_options.block_cache_bytes = std::size_t{64} << 10;  // small: misses and evictions too
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

INSTANTIATE_TEST_SUITE_P(Seeds, MemoryDBModelTest, testing::ValuesIn(Seeds()));

}  // namespace
}  // namespace forgedb::model
