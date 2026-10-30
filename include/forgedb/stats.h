// DBStats: what an open database has done (DB::GetStats).
//
//   const forgedb::DBStats stats = db->GetStats();
//   if (const std::optional<double> amplification = stats.WriteAmplification()) { ... }
//
// The counters start at zero when the database is opened and are not saved:
// they describe this open only. The table written by recovery at open is
// not counted (its writes were made in an earlier open; Open's recovery
// reports it). Only flushes and compactions that finished count: a failed
// or abandoned one deletes its tables, so the database kept none of it.
// Times are wall-clock microseconds. The level sizes are the database as it
// is now.
//
// GetStats copies everything under one lock, so the numbers agree with each
// other at one moment.
#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

namespace forgedb {

// Times writes were held up, and for how long in total.
struct StallStats {
  std::uint64_t count = 0;
  std::uint64_t microseconds = 0;
};

// Compactions from one level into the next.
struct CompactionStats {
  std::uint64_t count = 0;          // compactions that merged and rewrote tables
  std::uint64_t trivial_moves = 0;  // tables moved down without a rewrite (0 bytes)
  // Time of both kinds; a flush run in the middle of a compaction is not
  // included (it is in DBStats::flush_microseconds).
  std::uint64_t microseconds = 0;
  std::uint64_t bytes_read = 0;     // sizes of the tables merged
  std::uint64_t bytes_written = 0;  // sizes of the tables written
};

struct LevelStats {
  std::size_t tables = 0;       // now
  std::uint64_t bytes = 0;      // now: the tables' file sizes
  CompactionStats compactions;  // from this level into the next, since open
};

struct DBStats {
  std::vector<LevelStats> levels;  // one per level, level 0 first

  // Keys plus values of every write that succeeded (a deletion: its key).
  std::uint64_t user_bytes = 0;
  // Bytes appended to the write-ahead log (record headers and block padding
  // included).
  std::uint64_t log_bytes = 0;

  std::uint64_t flushes = 0;
  std::uint64_t flush_microseconds = 0;
  std::uint64_t flush_bytes_written = 0;  // sizes of the level-0 tables written

  StallStats memtable_full;    // the memtable was full while the last flush still ran
  StallStats level0_slowdown;  // 1 ms delays: level 0 at its slowdown trigger
  StallStats level0_stop;      // waits: level 0 at its stop trigger

  // Bytes written to tables by flushes and compactions.
  [[nodiscard]] std::uint64_t TableBytesWritten() const noexcept {
    std::uint64_t bytes = flush_bytes_written;
    for (const LevelStats& level : levels) {
      bytes += level.compactions.bytes_written;
    }
    return bytes;
  }

  // TableBytesWritten() / user_bytes; nullopt before the first write. The log
  // is not included (log_bytes / user_bytes adds a little over 1).
  [[nodiscard]] std::optional<double> WriteAmplification() const noexcept {
    if (user_bytes == 0) {
      return std::nullopt;
    }
    return static_cast<double>(TableBytesWritten()) / static_cast<double>(user_bytes);
  }
};

}  // namespace forgedb
