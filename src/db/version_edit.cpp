#include "db/version_edit.h"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

#include "forgedb/result.h"
#include "forgedb/status.h"

#include "db/internal_key.h"
#include "util/coding.h"

namespace forgedb {
namespace {

// Field tags. Written to disk: never renumber.
constexpr std::uint32_t kLogNumberTag = 1;
constexpr std::uint32_t kNextFileNumberTag = 2;
constexpr std::uint32_t kLastSequenceTag = 3;
constexpr std::uint32_t kRemovedFileTag = 4;
constexpr std::uint32_t kAddedFileTag = 5;

Status Malformed(std::string_view what) {
  return Status::Corruption("malformed version edit", what);
}

// Reads a scalar into `field`, which must not be set yet.
Status DecodeScalar(std::string_view& input, std::optional<std::uint64_t>& field,
                    std::string_view name) {
  if (field) {
    return Malformed(std::string(name) + " repeated");
  }
  field = GetVarint64(input);
  if (!field) {
    return Malformed(std::string(name) + " truncated");
  }
  return Status::OK();
}

std::optional<int> DecodeLevel(std::string_view& input) {
  const std::optional<std::uint32_t> level = GetVarint32(input);
  if (!level || *level >= static_cast<std::uint32_t>(kNumLevels)) {
    return std::nullopt;
  }
  return static_cast<int>(*level);
}

Status DecodeRemovedFile(std::string_view& input, VersionEdit& edit) {
  const std::optional<int> level = DecodeLevel(input);
  const std::optional<std::uint64_t> number = level ? GetVarint64(input) : std::nullopt;
  if (!level || !number) {
    return Malformed("removed file truncated or level out of range");
  }
  edit.removed_files.push_back({.level = *level, .number = *number});
  return Status::OK();
}

Status DecodeAddedFile(std::string_view& input, VersionEdit& edit) {
  const std::optional<int> level = DecodeLevel(input);
  const std::optional<std::uint64_t> number = level ? GetVarint64(input) : std::nullopt;
  const std::optional<std::uint64_t> size = number ? GetVarint64(input) : std::nullopt;
  const std::optional<std::string_view> smallest = size ? GetLengthPrefixed(input) : std::nullopt;
  const std::optional<std::string_view> largest =
      smallest ? GetLengthPrefixed(input) : std::nullopt;
  if (!level || !number || !size || !smallest || !largest) {
    return Malformed("added file truncated or level out of range");
  }
  if (!ParseInternalKey(*smallest) || !ParseInternalKey(*largest)) {
    return Malformed("added file with a malformed key");
  }
  if (CompareInternalKeys(*smallest, *largest) > 0) {
    return Malformed("added file whose smallest key is after its largest");
  }
  edit.added_files.push_back({.level = *level,
                              .file = {.number = *number,
                                       .file_size = *size,
                                       .smallest = std::string(*smallest),
                                       .largest = std::string(*largest)}});
  return Status::OK();
}

}  // namespace

void VersionEdit::EncodeTo(std::string& dst) const {
  if (log_number) {
    PutVarint32(dst, kLogNumberTag);
    PutVarint64(dst, *log_number);
  }
  if (next_file_number) {
    PutVarint32(dst, kNextFileNumberTag);
    PutVarint64(dst, *next_file_number);
  }
  if (last_sequence) {
    PutVarint32(dst, kLastSequenceTag);
    PutVarint64(dst, *last_sequence);
  }
  for (const RemovedFile& removed : removed_files) {
    PutVarint32(dst, kRemovedFileTag);
    PutVarint32(dst, static_cast<std::uint32_t>(removed.level));
    PutVarint64(dst, removed.number);
  }
  for (const AddedFile& added : added_files) {
    PutVarint32(dst, kAddedFileTag);
    PutVarint32(dst, static_cast<std::uint32_t>(added.level));
    PutVarint64(dst, added.file.number);
    PutVarint64(dst, added.file.file_size);
    PutLengthPrefixed(dst, added.file.smallest);
    PutLengthPrefixed(dst, added.file.largest);
  }
}

Result<VersionEdit> VersionEdit::Decode(std::string_view input) {
  VersionEdit edit;
  while (!input.empty()) {
    const std::optional<std::uint32_t> tag = GetVarint32(input);
    if (!tag) {
      return Malformed("truncated tag");
    }
    Status status;
    switch (*tag) {
      case kLogNumberTag:
        status = DecodeScalar(input, edit.log_number, "log number");
        break;
      case kNextFileNumberTag:
        status = DecodeScalar(input, edit.next_file_number, "next file number");
        break;
      case kLastSequenceTag:
        status = DecodeScalar(input, edit.last_sequence, "last sequence");
        if (status.ok() && edit.last_sequence.value_or(0) > kMaxSequenceNumber) {
          status = Malformed("last sequence beyond the maximum");
        }
        break;
      case kRemovedFileTag:
        status = DecodeRemovedFile(input, edit);
        break;
      case kAddedFileTag:
        status = DecodeAddedFile(input, edit);
        break;
      default:
        status = Malformed("unknown tag " + std::to_string(*tag));
        break;
    }
    FORGEDB_RETURN_IF_ERROR(status);
  }
  return edit;
}

}  // namespace forgedb
