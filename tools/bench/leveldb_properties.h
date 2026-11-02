// Reading LevelDB's "leveldb.sstables" and "leveldb.stats" properties, for
// the LevelDB adapter (leveldb_engine.h). Plain text in, numbers out, so the
// parsing is tested without LevelDB (tests/unit/bench_test.cpp). The formats
// (Version::DebugString and DBImpl::GetProperty) are the same in LevelDB 1.23
// and in the commit vcpkg builds (7ee830d, 2026-03-11).
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>

#include "forgedb/result.h"

namespace forgedb::bench {

inline constexpr std::size_t kLevelDBLevels = 7;

// The table files per level, from "leveldb.sstables", which lists them as
//   --- level 1 ---
//    17:2097152['key1' @ 5 : 1 .. 'key9' @ 8 : 1]
// (file number, then size in bytes).
struct LevelDBTables {
  std::array<std::uint64_t, kLevelDBLevels> files{};
  std::array<std::uint64_t, kLevelDBLevels> bytes{};
};
[[nodiscard]] Result<LevelDBTables> ParseLevelDBTables(std::string_view sstables);

// LevelDB's compaction score for `tables` (VersionSet::Finalize): level 0
// scores its file count / 4, levels 1 to 5 their bytes / (10 MiB x 10^(level
// - 1)). LevelDB compacts while the highest score is 1 or more (it also
// compacts a file that reads keep seeking past, which no property shows).
[[nodiscard]] double LevelDBCompactionScore(const LevelDBTables& tables);

// The compaction totals in "leveldb.stats", summed over its levels. LevelDB
// prints them per level in whole seconds and whole MiB, so the sums are
// rounded too; flushes count as level-0 compactions.
struct LevelDBCompactionStats {
  double seconds = 0;
  double read_mb = 0;
  double write_mb = 0;
};
[[nodiscard]] Result<LevelDBCompactionStats> ParseLevelDBStats(std::string_view stats);

}  // namespace forgedb::bench
