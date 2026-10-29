// Choosing what to compact (TODO 4.3; docs/adr/0008).
//
// Each level gets a score: level 0 its number of tables divided by
// Options::level0_compaction_trigger, levels 1 to 5 their bytes divided by
// their target (Options::max_bytes_for_level_base for level 1, ten times
// more for each level below). Level 6 is the last and has no target. The
// level with the highest score of at least 1 is compacted into the next one:
//
//   - From level 0, every table: level-0 tables overlap, and moving only some
//     of them down could put an older version of a key above a newer one.
//     Tables flushed while the compaction runs are newer and stay above.
//   - From a deeper level, one table, chosen round-robin by a per-level
//     pointer (kept in memory only: after a reopen it starts from the first
//     key again).
//   - Plus every table of the next level whose key range overlaps them.
//
// Compaction cuts its output tables only between two different user keys,
// so in levels 1 and below no user key is split between two tables. That is
// what makes it safe to take a single table from such a level: all versions
// of its keys come with it.
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "forgedb/options.h"

#include "db/version_edit.h"
#include "db/version_set.h"

namespace forgedb {

// The most bytes level `level` (1 to 5) should hold.
[[nodiscard]] std::uint64_t MaxBytesForLevel(const Options& options, int level) noexcept;

// The level's compaction score (see above); 0 for the last level.
[[nodiscard]] double CompactionScore(const Version& version, const Options& options,
                                     int level) noexcept;

// Whether some level scores 1 or more.
[[nodiscard]] bool NeedsCompaction(const Version& version, const Options& options) noexcept;

// The tables of `level` whose user-key range overlaps [smallest, largest]
// (user keys).
[[nodiscard]] std::vector<FileMetaData> OverlappingFiles(const Version& version, int level,
                                                         std::string_view smallest,
                                                         std::string_view largest);

struct Compaction {
  int level = 0;  // inputs[0] are from here; the output goes to level + 1
  std::array<std::vector<FileMetaData>, 2> inputs;
  // The version the inputs were chosen from (kept alive while compacting).
  std::shared_ptr<const Version> version;

  // Whether no level below the output level has a table whose range holds
  // `user_key`: then no older version of it exists outside the inputs.
  [[nodiscard]] bool IsBaseLevelForKey(std::string_view user_key) const;

  // A single table with nothing to merge in the next level: it can simply
  // move down.
  [[nodiscard]] bool IsTrivialMove() const noexcept {
    return inputs[0].size() == 1 && inputs[1].empty();
  }
};

// The round-robin pointers: the largest internal key compacted last, per level.
using CompactionPointers = std::array<std::string, kNumLevels>;

// The compaction for the highest-scoring level, or nullopt if none scores 1.
// Updates `pointers` for the level chosen.
[[nodiscard]] std::optional<Compaction> PickCompaction(std::shared_ptr<const Version> version,
                                                       const Options& options,
                                                       CompactionPointers& pointers);

// A compaction of `level` (0 to 5) whatever its score; nullopt if it is empty.
[[nodiscard]] std::optional<Compaction> CompactionForLevel(std::shared_ptr<const Version> version,
                                                           int level, CompactionPointers& pointers);

}  // namespace forgedb
