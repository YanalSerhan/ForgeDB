// VersionEdit: one change to the set of table files, as recorded in the
// MANIFEST (docs/file-formats.md, "Version edit").
//
// An edit is a list of tagged fields: the scalars (log number, next file
// number, last sequence number), files removed and files added. A MANIFEST's
// first record is a full snapshot: all three scalars and every live file.
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "forgedb/result.h"

#include "db/internal_key.h"

namespace forgedb {

// Levels of the LSM tree: 0 (flushed memtables) to 6 (ADR 0008).
inline constexpr int kNumLevels = 7;

// A table file as the MANIFEST describes it.
struct FileMetaData {
  std::uint64_t number = 0;
  std::uint64_t file_size = 0;
  std::string smallest;  // internal keys
  std::string largest;

  friend bool operator==(const FileMetaData&, const FileMetaData&) = default;
};

struct VersionEdit {
  struct AddedFile {
    int level = 0;
    FileMetaData file;
    friend bool operator==(const AddedFile&, const AddedFile&) = default;
  };
  struct RemovedFile {
    int level = 0;
    std::uint64_t number = 0;
    friend bool operator==(const RemovedFile&, const RemovedFile&) = default;
  };

  // Logs numbered below this hold nothing that is not in a table.
  std::optional<std::uint64_t> log_number;
  // The next unused file number.
  std::optional<std::uint64_t> next_file_number;
  // The last sequence number whose write is in a table or an older log.
  std::optional<SequenceNumber> last_sequence;
  std::vector<RemovedFile> removed_files;
  std::vector<AddedFile> added_files;

  // Whether all three scalars are set (required of a MANIFEST's first record).
  [[nodiscard]] bool HasAllScalars() const noexcept {
    return log_number && next_file_number && last_sequence;
  }

  // Appends the encoding to `dst`.
  void EncodeTo(std::string& dst) const;

  // Parses bytes from a MANIFEST record. Corruption for an unknown tag, a
  // truncated field, a repeated scalar, a level outside [0, kNumLevels), a
  // malformed key, smallest > largest, or a sequence number beyond the
  // maximum.
  [[nodiscard]] static Result<VersionEdit> Decode(std::string_view input);

  friend bool operator==(const VersionEdit&, const VersionEdit&) = default;
};

}  // namespace forgedb
