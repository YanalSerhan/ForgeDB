// Phase 1 exit gate: the memory-only DB agrees with std::map over 1,000,000
// random operations, for several seeds.
//
// The operation count is a build-time default (FORGEDB_MODEL_TEST_OPERATIONS,
// set in tests/CMakeLists.txt): 1,000,000 in optimized builds, 100,000 in
// debug and sanitizer builds, which run 20-50x slower. Override at run time:
//   FORGEDB_MODEL_OPS=5000000   operations per seed
//   FORGEDB_MODEL_SEED=17       run only this seed (e.g. to reproduce a failure)
#include <cstdint>
#include <cstdlib>

#include <gtest/gtest.h>

#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "forgedb/db.h"

#include "db/db_impl.h"
#include "env/mem_file_system.h"
#include "model/model_harness.h"

#ifndef FORGEDB_MODEL_TEST_OPERATIONS
#define FORGEDB_MODEL_TEST_OPERATIONS 100000
#endif

namespace forgedb::model {
namespace {

std::optional<std::string> GetEnv(const char* name) {
#ifdef _MSC_VER  // MSVC and clang-cl
  // _dupenv_s, not std::getenv: the MSVC runtime deprecates getenv (C4996).
  char* value = nullptr;
  std::size_t length = 0;
  if (_dupenv_s(&value, &length, name) != 0 || value == nullptr) {
    return std::nullopt;
  }
  std::string result(value);
  std::free(value);  // NOLINT(cppcoreguidelines-no-malloc,cppcoreguidelines-owning-memory)
  return result;
#else
  const char* value = std::getenv(name);  // NOLINT(concurrency-mt-unsafe): read before threads
  return value == nullptr ? std::nullopt : std::optional<std::string>(value);
#endif
}

std::size_t OperationsPerSeed() {
  const std::optional<std::string> value = GetEnv("FORGEDB_MODEL_OPS");
  return value ? static_cast<std::size_t>(std::stoull(*value)) : FORGEDB_MODEL_TEST_OPERATIONS;
}

std::vector<std::uint64_t> Seeds() {
  if (const std::optional<std::string> seed = GetEnv("FORGEDB_MODEL_SEED")) {
    return {std::stoull(*seed)};
  }
  return {1, 2, 3};
}

class MemoryDBModelTest : public testing::TestWithParam<std::uint64_t> {};

TEST_P(MemoryDBModelTest, AgreesWithStdMap) {
  // Each replay gets a new database on its own in-memory file system.
  const auto open = [] {
    return OpenDB(Options{}, "model-test", std::make_shared<MemFileSystem>());
  };
  RunOptions options;
  options.seed = GetParam();
  options.operations = OperationsPerSeed();
  EXPECT_EQ(RunModelTest(open, options), "");
}

INSTANTIATE_TEST_SUITE_P(Seeds, MemoryDBModelTest, testing::ValuesIn(Seeds()));

}  // namespace
}  // namespace forgedb::model
