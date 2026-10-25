// Names of the files in a database directory.
//
//   000005.log        write-ahead log number 5
//   000007.sst        table file number 7                    (Phase 3)
//   MANIFEST-000003   descriptor (MANIFEST) number 3         (Phase 3.6)
//   CURRENT           names the current MANIFEST             (Phase 3.6)
//   LOCK              held while the database is open
//
// File numbers come from one counter, so a number identifies one file of any
// type. They are written in decimal with at least six digits ("000005"; a
// seventh digit appears from 1,000,000 on).
#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

namespace forgedb {

enum class FileType : std::uint8_t {
  kLog,
  kTable,
  kDescriptor,
  kCurrent,
  kLock,
};

[[nodiscard]] std::filesystem::path LogFileName(const std::filesystem::path& db,
                                                std::uint64_t number);
[[nodiscard]] std::filesystem::path TableFileName(const std::filesystem::path& db,
                                                  std::uint64_t number);
[[nodiscard]] std::filesystem::path DescriptorFileName(const std::filesystem::path& db,
                                                       std::uint64_t number);
[[nodiscard]] std::filesystem::path CurrentFileName(const std::filesystem::path& db);
[[nodiscard]] std::filesystem::path LockFileName(const std::filesystem::path& db);

struct ParsedFileName {
  FileType type = FileType::kLog;
  std::uint64_t number = 0;  // 0 for CURRENT and LOCK

  friend bool operator==(const ParsedFileName&, const ParsedFileName&) = default;
};

// Parses a name from the database directory (a name, not a path). The names
// come from the file system, so they are untrusted: only the exact forms the
// functions above produce are accepted (no extra or missing zeros, no signs,
// no numbers above 2^64 - 1); anything else is nullopt and is left alone.
[[nodiscard]] std::optional<ParsedFileName> ParseFileName(std::string_view name) noexcept;

}  // namespace forgedb
