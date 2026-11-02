// forgedb_bench: see tools/bench/bench.h and tools/bench/bench_options.h.
// Built with FORGEDB_BENCH_ENGINES (the msvc-bench preset), it also runs
// RocksDB, LevelDB and SQLite.
#include <cstddef>
#include <iostream>
#include <memory>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "env/win_util.h"
#include "tools/bench/bench.h"
#include "tools/bench/engine.h"
#include "tools/bench/forgedb_engine.h"
#ifdef FORGEDB_BENCH_ENGINES
#include "tools/bench/leveldb_engine.h"
#include "tools/bench/rocksdb_engine.h"
#include "tools/bench/sqlite_engine.h"
#endif

int wmain(int argc, wchar_t** argv) {  // NOLINT(misc-use-internal-linkage): the entry point
  using forgedb::bench::DiskEngine;
  std::vector<std::string> args;
  for (const wchar_t* arg : std::span<wchar_t*>(argv, static_cast<std::size_t>(argc)).subspan(1)) {
    args.push_back(forgedb::win::WideToUtf8(arg));
  }
  std::vector<DiskEngine> engines = {
      {.name = "forgedb", .open = forgedb::bench::OpenForgeDBOnDisk}};
#ifdef FORGEDB_BENCH_ENGINES
  engines.push_back({.name = "rocksdb", .open = forgedb::bench::OpenRocksDB});
  engines.push_back({.name = "leveldb", .open = forgedb::bench::OpenLevelDB});
  engines.push_back({.name = "sqlite", .open = forgedb::bench::OpenSQLite});
#endif
  const std::unique_ptr<forgedb::bench::EngineFactory> factory =
      forgedb::bench::NewDiskEngineFactory(std::move(engines));
  return forgedb::bench::RunBench(args, *factory, std::cout, std::cerr);
}
