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
#include <cstdint>

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

INSTANTIATE_TEST_SUITE_P(Seeds, MemoryDBModelTest, testing::ValuesIn(Seeds()));

}  // namespace
}  // namespace forgedb::model
