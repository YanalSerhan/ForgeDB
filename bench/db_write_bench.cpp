// Write throughput with sync = true on the real file system, 1 thread vs 8
// threads (TODO 2.2). Each synced write must reach stable storage before it
// returns; with group commit, writers that arrive while a sync is in progress
// share the next one, so 8 threads should do far more writes per second than
// 1 thread, even though the drive's sync rate is the same.
//
//   out\build\msvc-release\bench\forgedb_microbench.exe --benchmark_filter=DBWrite
//
// The database lives in a fresh directory under %TEMP%, deleted afterwards.
// Results depend heavily on the drive (and on its write cache settings).
#include <benchmark/benchmark.h>

#include <cstdint>
#include <filesystem>
#include <memory>
#include <mutex>
#include <random>
#include <string>
#include <system_error>

#include "forgedb/db.h"
#include "forgedb/options.h"

namespace forgedb {
namespace {

// One database shared by all threads of a benchmark run: the first thread to
// arrive opens it, the last one to leave deletes it.
struct SharedDB {
  std::mutex mutex;
  int users = 0;
  std::filesystem::path directory;
  std::unique_ptr<DB> db;
};

SharedDB& Shared() {
  static SharedDB shared;
  return shared;
}

DB* Acquire() {
  SharedDB& shared = Shared();
  const std::scoped_lock lock(shared.mutex);
  if (shared.users++ == 0) {
    std::random_device random;
    shared.directory =
        std::filesystem::temp_directory_path() / ("forgedb-bench-" + std::to_string(random()));
    Result<std::unique_ptr<DB>> db = DB::Open(Options{}, shared.directory);
    shared.db = db.ok() ? std::move(db).value() : nullptr;
  }
  return shared.db.get();
}

void Release() {
  SharedDB& shared = Shared();
  const std::scoped_lock lock(shared.mutex);
  if (--shared.users == 0) {
    shared.db.reset();
    std::error_code ignored;
    std::filesystem::remove_all(shared.directory, ignored);
  }
}

void BM_DBWriteSync(benchmark::State& state) {
  DB* const db = Acquire();
  if (db == nullptr) {
    state.SkipWithError("could not open the database");
    Release();
    return;
  }
  WriteOptions options;
  options.sync = true;
  const std::string value(100, 'v');
  std::uint64_t i = 0;
  for (auto _ : state) {
    // 16-byte keys, distinct per thread.
    std::string key = "t" + std::to_string(state.thread_index()) + "-" + std::to_string(i++);
    key.resize(16, '.');
    if (!db->Put(options, key, value).ok()) {
      state.SkipWithError("write failed");
      break;
    }
  }
  state.SetItemsProcessed(static_cast<std::int64_t>(state.iterations()));
  Release();
}

BENCHMARK(BM_DBWriteSync)->Threads(1)->Threads(8)->UseRealTime();

}  // namespace
}  // namespace forgedb
