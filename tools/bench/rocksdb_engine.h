// The RocksDB adapter for forgedb_bench (TODO 5.1b; builds with
// FORGEDB_BENCH_ENGINES, RocksDB from vcpkg).
//
// EngineOptions map to RocksDB's options as:
//   cache_bytes        -> an LRU block cache of that size (0: no block cache)
//   memtable_bytes     -> write_buffer_size
//   bloom_bits_per_key -> a Bloom filter policy with that many bits (0: none)
//   sync               -> WriteOptions::sync on every write
// Compression is off. Everything else is RocksDB's default: among others,
// level_compaction_dynamic_level_bytes, a 256 MiB level-1 target, 64 MiB
// files, level-0 triggers of 4, 20 and 36 files, and two background jobs
// (ForgeDB and LevelDB run one). Statistics are on, for Counters().
//
// WaitForBackgroundWork is DB::WaitForCompact: it returns once no flush or
// compaction is running or wanted. Counters are RocksDB's statistics tickers
// for bytes written (by Put, to the log, by flushes, by compactions) and
// stall time, under RocksDB's names.
#pragma once

#include <filesystem>
#include <memory>

#include "forgedb/result.h"

#include "tools/bench/engine.h"

namespace forgedb::bench {

// A new RocksDB database in folder `dir` (a DiskEngineOpener).
[[nodiscard]] Result<std::unique_ptr<Engine>> OpenRocksDB(const std::filesystem::path& dir,
                                                          const EngineOptions& options);

}  // namespace forgedb::bench
