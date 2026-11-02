#include "tools/bench/leveldb_properties.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "forgedb/result.h"
#include "forgedb/status.h"

#include "tools/tool_util.h"

namespace forgedb::bench {
namespace {

// The next line of `text` (without its '\n'), removed from `text`.
std::string_view NextLine(std::string_view& text) {
  const std::size_t end = text.find('\n');
  const std::string_view line = text.substr(0, end);
  text.remove_prefix(end == std::string_view::npos ? text.size() : end + 1);
  return line;
}

Status Malformed(std::string_view property, std::string_view line) {
  return Status::Corruption("unexpected line in LevelDB's " + std::string(property),
                            tools::EscapeBytes(line, 80));
}

std::vector<std::string_view> Words(std::string_view line) {
  std::vector<std::string_view> words;
  while (!line.empty()) {
    const std::size_t start = line.find_first_not_of(' ');
    if (start == std::string_view::npos) {
      break;
    }
    line.remove_prefix(start);
    const std::size_t end = line.find(' ');
    words.push_back(line.substr(0, end));
    line.remove_prefix(end == std::string_view::npos ? line.size() : end);
  }
  return words;
}

constexpr std::string_view kLevelPrefix = "--- level ";
constexpr std::string_view kLevelSuffix = " ---";

}  // namespace

Result<LevelDBTables> ParseLevelDBTables(std::string_view sstables) {
  LevelDBTables tables;
  std::optional<std::size_t> level;
  while (!sstables.empty()) {
    const std::string_view line = NextLine(sstables);
    if (line.starts_with(kLevelPrefix) && line.ends_with(kLevelSuffix)) {
      const std::optional<std::uint64_t> number = tools::ParseUnsigned(line.substr(
          kLevelPrefix.size(), line.size() - kLevelPrefix.size() - kLevelSuffix.size()));
      if (!number || *number >= kLevelDBLevels) {
        return Malformed("leveldb.sstables", line);
      }
      level = static_cast<std::size_t>(*number);
      continue;
    }
    // " <file number>:<size>[<smallest key> .. <largest key>]"
    const std::size_t colon = line.find(':');
    const std::size_t bracket = line.find('[');
    if (!level || !line.starts_with(' ') || colon == std::string_view::npos ||
        bracket == std::string_view::npos || bracket < colon) {
      return Malformed("leveldb.sstables", line);
    }
    const std::optional<std::uint64_t> size =
        tools::ParseUnsigned(line.substr(colon + 1, bracket - colon - 1));
    if (!size || !tools::ParseUnsigned(line.substr(1, colon - 1))) {
      return Malformed("leveldb.sstables", line);
    }
    ++tables.files.at(*level);
    tables.bytes.at(*level) += *size;
  }
  return tables;
}

double LevelDBCompactionScore(const LevelDBTables& tables) {
  constexpr double kLevel0Trigger = 4;             // config::kL0_CompactionTrigger
  constexpr double kLevel1Bytes = 10 * 1048576.0;  // MaxBytesForLevel(1)
  double best = static_cast<double>(tables.files[0]) / kLevel0Trigger;
  double max_bytes = kLevel1Bytes;
  for (std::size_t level = 1; level + 1 < kLevelDBLevels; ++level) {
    const double score = static_cast<double>(tables.bytes.at(level)) / max_bytes;
    best = score > best ? score : best;
    max_bytes *= 10;
  }
  return best;
}

Result<LevelDBCompactionStats> ParseLevelDBStats(std::string_view stats) {
  constexpr std::string_view kRule = "--------------------------------------------------\n";
  const std::size_t rule = stats.find(kRule);
  if (rule == std::string_view::npos) {
    return Malformed("leveldb.stats", stats.substr(0, stats.find('\n')));
  }
  stats.remove_prefix(rule + kRule.size());
  LevelDBCompactionStats totals;
  while (!stats.empty()) {
    const std::string_view line = NextLine(stats);
    if (line.empty()) {
      continue;
    }
    // Level, files, size (MiB), time (s), read (MiB), write (MiB); LevelDB
    // prints the last four with "%.0f", so all six are whole numbers.
    const std::vector<std::string_view> words = Words(line);
    std::array<double, 6> numbers{};
    if (words.size() != numbers.size()) {
      return Malformed("leveldb.stats", line);
    }
    for (std::size_t i = 0; i < numbers.size(); ++i) {
      const std::optional<std::uint64_t> number = tools::ParseUnsigned(words[i]);
      if (!number) {
        return Malformed("leveldb.stats", line);
      }
      numbers.at(i) = static_cast<double>(*number);
    }
    totals.seconds += numbers[3];
    totals.read_mb += numbers[4];
    totals.write_mb += numbers[5];
  }
  return totals;
}

}  // namespace forgedb::bench
