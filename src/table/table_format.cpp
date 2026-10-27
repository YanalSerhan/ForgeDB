#include "table/table_format.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>

#include "forgedb/result.h"
#include "forgedb/status.h"

#include "table/block_format.h"
#include "util/coding.h"
#include "util/crc32c.h"

namespace forgedb {

namespace {

// Footer layout: four fixed64 handle fields, the version, the CRC of those
// 36 bytes, then the magic.
constexpr std::size_t kFooterCrcOffset = 36;
constexpr std::size_t kFooterMagicOffset = 40;

// Whether `handle` plus its trailer ends at or before `limit`, without
// overflowing.
bool FitsBefore(const BlockHandle& handle, std::uint64_t limit) noexcept {
  return handle.offset <= limit && handle.size <= limit - handle.offset &&
         kBlockTrailerSize <= limit - handle.offset - handle.size;
}

std::string Describe(const BlockHandle& handle) {
  return "offset " + std::to_string(handle.offset) + ", size " + std::to_string(handle.size);
}

}  // namespace

void AppendBlockHandle(std::string& dst, const BlockHandle& handle) {
  PutVarint64(dst, handle.offset);
  PutVarint64(dst, handle.size);
}

std::optional<BlockHandle> DecodeBlockHandle(std::string_view encoded) noexcept {
  const std::optional<std::uint64_t> offset = GetVarint64(encoded);
  const std::optional<std::uint64_t> size = offset ? GetVarint64(encoded) : std::nullopt;
  if (!size || !encoded.empty()) {
    return std::nullopt;
  }
  return BlockHandle{.offset = *offset, .size = *size};
}

std::string EncodeTableFooter(const TableFooter& footer) {
  std::string out;
  out.reserve(kTableFooterSize);
  PutFixed64(out, footer.filter.offset);
  PutFixed64(out, footer.filter.size);
  PutFixed64(out, footer.index.offset);
  PutFixed64(out, footer.index.size);
  PutFixed32(out, kTableFormatVersion);
  PutFixed32(out, crc32c::Mask(crc32c::Value(out)));
  out.append(kTableMagic);
  return out;
}

Result<TableFooter> DecodeTableFooter(std::string_view footer, std::uint64_t file_size) {
  if (footer.size() != kTableFooterSize || file_size < kTableFooterSize) {
    return Status::Corruption("file too short to be a table", std::to_string(file_size) + " bytes");
  }
  if (footer.substr(kFooterMagicOffset) != kTableMagic) {
    return Status::Corruption("not a ForgeDB table (bad magic number)");
  }
  const std::uint32_t stored_crc = crc32c::Unmask(DecodeFixed32(&footer[kFooterCrcOffset]));
  if (crc32c::Value(footer.substr(0, kFooterCrcOffset)) != stored_crc) {
    return Status::Corruption("table footer checksum mismatch");
  }
  const std::uint32_t version = DecodeFixed32(&footer[32]);
  if (version != kTableFormatVersion) {
    return Status::NotSupported("unknown table format version", std::to_string(version));
  }

  // The four handle fields, read in order (braced initializers are evaluated
  // left to right). The footer's size was checked, so each read succeeds.
  std::string_view fields = footer.substr(0, 32);
  const auto next = [&fields] { return GetFixed64(fields).value_or(0); };
  const TableFooter decoded{
      .filter = {.offset = next(), .size = next()},
      .index = {.offset = next(), .size = next()},
  };
  const std::uint64_t footer_start = file_size - kTableFooterSize;
  if (!FitsBefore(decoded.index, footer_start)) {
    return Status::Corruption("table index block outside the file", Describe(decoded.index));
  }
  if (decoded.filter.size == 0) {
    if (decoded.filter.offset != 0) {
      return Status::Corruption("table filter handle malformed", Describe(decoded.filter));
    }
  } else if (!FitsBefore(decoded.filter, decoded.index.offset)) {
    return Status::Corruption("table filter block not before the index", Describe(decoded.filter));
  }
  return decoded;
}

Result<std::string> ReadBlockContents(const RandomAccessFile& file, const BlockHandle& handle,
                                      std::uint64_t limit) {
  if (!FitsBefore(handle, limit)) {
    return Status::Corruption("table block handle outside its region",
                              Describe(handle) + ", limit " + std::to_string(limit));
  }
  std::string stored(static_cast<std::size_t>(handle.size) + kBlockTrailerSize, '\0');
  FORGEDB_ASSIGN_OR_RETURN(const std::size_t read,
                           file.Read(handle.offset, std::span<char>(stored)));
  if (read != stored.size()) {
    return Status::Corruption("truncated table block read",
                              Describe(handle) + ", got " + std::to_string(read) + " bytes");
  }
  const Result<std::string_view> contents = CheckBlockTrailer(stored);
  if (!contents.ok()) {
    return Status::Corruption(contents.status().message(), "table block at " + Describe(handle));
  }
  stored.resize(contents->size());  // drop the trailer; the contents start at 0
  return stored;
}

}  // namespace forgedb
