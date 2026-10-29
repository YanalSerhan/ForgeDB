#include "db/compaction.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string_view>
#include <utility>
#include <vector>

#include "forgedb/options.h"

#include "db/internal_key.h"
#include "db/version_edit.h"
#include "db/version_set.h"

namespace forgedb {
namespace {

constexpr int kLastLevel = kNumLevels - 1;

std::uint64_t LevelBytes(const std::vector<FileMetaData>& files) noexcept {
  std::uint64_t bytes = 0;
  for (const FileMetaData& file : files) {
    bytes += file.file_size;
  }
  return bytes;
}

// The smallest and largest user keys of `files` (not empty).
std::pair<std::string_view, std::string_view> UserKeyRange(const std::vector<FileMetaData>& files) {
  std::string_view smallest = ExtractUserKey(files.front().smallest);
  std::string_view largest = ExtractUserKey(files.front().largest);
  for (const FileMetaData& file : files) {
    smallest = std::min(smallest, ExtractUserKey(file.smallest));
    largest = std::max(largest, ExtractUserKey(file.largest));
  }
  return {smallest, largest};
}

// Adds the next level's overlapping tables and fills in the rest.
Compaction Finish(std::shared_ptr<const Version> version, int level,
                  std::vector<FileMetaData> inputs) {
  const auto [smallest, largest] = UserKeyRange(inputs);
  Compaction compaction;
  compaction.level = level;
  compaction.inputs[1] = OverlappingFiles(*version, level + 1, smallest, largest);
  compaction.inputs[0] = std::move(inputs);
  compaction.version = std::move(version);
  return compaction;
}

}  // namespace

std::uint64_t MaxBytesForLevel(const Options& options, int level) noexcept {
  std::uint64_t bytes = options.max_bytes_for_level_base;
  for (int i = 1; i < level; ++i) {
    bytes = bytes > UINT64_MAX / 10 ? UINT64_MAX : bytes * 10;
  }
  return bytes;
}

double CompactionScore(const Version& version, const Options& options, int level) noexcept {
  const std::vector<FileMetaData>& files = version.files.at(static_cast<std::size_t>(level));
  if (level == 0) {
    return static_cast<double>(files.size()) /
           static_cast<double>(options.level0_compaction_trigger);
  }
  if (level == kLastLevel) {
    return 0;
  }
  return static_cast<double>(LevelBytes(files)) /
         static_cast<double>(MaxBytesForLevel(options, level));
}

bool NeedsCompaction(const Version& version, const Options& options) noexcept {
  for (int level = 0; level < kLastLevel; ++level) {
    if (CompactionScore(version, options, level) >= 1) {
      return true;
    }
  }
  return false;
}

std::vector<FileMetaData> OverlappingFiles(const Version& version, int level,
                                           std::string_view smallest, std::string_view largest) {
  std::vector<FileMetaData> overlapping;
  for (const FileMetaData& file : version.files.at(static_cast<std::size_t>(level))) {
    if (ExtractUserKey(file.largest) < smallest || ExtractUserKey(file.smallest) > largest) {
      continue;
    }
    overlapping.push_back(file);
  }
  return overlapping;
}

bool Compaction::IsBaseLevelForKey(std::string_view user_key) const {
  for (int deeper = level + 2; deeper < kNumLevels; ++deeper) {
    for (const FileMetaData& file : version->files.at(static_cast<std::size_t>(deeper))) {
      if (user_key >= ExtractUserKey(file.smallest) && user_key <= ExtractUserKey(file.largest)) {
        return false;
      }
    }
  }
  return true;
}

std::optional<Compaction> CompactionForLevel(std::shared_ptr<const Version> version, int level,
                                             CompactionPointers& pointers) {
  if (level < 0 || level >= kLastLevel) {
    return std::nullopt;
  }
  const std::vector<FileMetaData>& files = version->files.at(static_cast<std::size_t>(level));
  if (files.empty()) {
    return std::nullopt;
  }
  if (level == 0) {
    return Finish(std::move(version), 0, files);  // every level-0 table
  }
  // The first table after the pointer, round-robin. Files are sorted by
  // smallest key and do not overlap, so this is also the first one whose
  // largest key is after the pointer.
  std::string& pointer = pointers.at(static_cast<std::size_t>(level));
  auto it = std::ranges::find_if(files, [&pointer](const FileMetaData& file) {
    return pointer.empty() || CompareInternalKeys(file.largest, pointer) > 0;
  });
  if (it == files.end()) {
    it = files.begin();
  }
  pointer = it->largest;
  return Finish(std::move(version), level, {*it});
}

std::optional<Compaction> PickCompaction(std::shared_ptr<const Version> version,
                                         const Options& options, CompactionPointers& pointers) {
  int best_level = -1;
  double best_score = 1;
  for (int level = 0; level < kLastLevel; ++level) {
    const double score = CompactionScore(*version, options, level);
    if (score >= best_score && (best_level < 0 || score > best_score)) {
      best_level = level;
      best_score = score;
    }
  }
  if (best_level < 0) {
    return std::nullopt;
  }
  return CompactionForLevel(std::move(version), best_level, pointers);
}

}  // namespace forgedb
