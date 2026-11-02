// The LevelDB adapter for forgedb_bench (TODO 5.1b; builds with
// FORGEDB_BENCH_ENGINES, LevelDB from vcpkg, with its crc32c feature for
// hardware CRC-32C as ForgeDB uses).
//
// EngineOptions map to LevelDB's options as:
//   cache_bytes        -> an LRU block cache of that size (0: caches nothing)
//   memtable_bytes     -> write_buffer_size
//   bloom_bits_per_key -> a Bloom filter policy with that many bits (0: none)
//   sync               -> WriteOptions::sync on every write
// Compression is off. Everything else is LevelDB's default (its level sizes,
// file size and level-0 triggers are compiled in, and are ForgeDB's too).
//
// LevelDB has no call that waits for its compactions, so
// WaitForBackgroundWork polls: it returns once the compaction scores (from
// the "leveldb.sstables" property, leveldb_properties.h) are all below 1 and
// the list of table files has not changed for 1 second. The 1 second counts
// in the settle time the benchmark reports. Counters come from the same
// property (tables, table_bytes) and from "leveldb.stats" (compaction time
// and MiB read and written, which LevelDB rounds per level).
#pragma once

#include <filesystem>
#include <memory>

#include "forgedb/result.h"

#include "tools/bench/engine.h"

namespace forgedb::bench {

// A new LevelDB database in folder `dir` (a DiskEngineOpener).
[[nodiscard]] Result<std::unique_ptr<Engine>> OpenLevelDB(const std::filesystem::path& dir,
                                                          const EngineOptions& options);

}  // namespace forgedb::bench
