// The parts of a table file outside its blocks: block handles, the footer, and
// reading a block back (docs/file-formats.md, "Table file").
//
//   [data block][trailer] ... [filter block][trailer] [index block][trailer] [footer]
#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

#include "forgedb/result.h"

#include "env/file_system.h"

namespace forgedb {

// Where a block is in the file: its offset and the size of its contents (the
// 5-byte trailer follows them).
struct BlockHandle {
  std::uint64_t offset = 0;
  std::uint64_t size = 0;

  friend bool operator==(const BlockHandle&, const BlockHandle&) = default;
};

// Index entries store a handle as varint64 offset, varint64 size.
void AppendBlockHandle(std::string& dst, const BlockHandle& handle);
// nullopt unless `encoded` is exactly one encoded handle.
[[nodiscard]] std::optional<BlockHandle> DecodeBlockHandle(std::string_view encoded) noexcept;

inline constexpr std::size_t kTableFooterSize = 48;
inline constexpr std::uint32_t kTableFormatVersion = 1;
inline constexpr std::string_view kTableMagic = "ForgeSST";

struct TableFooter {
  BlockHandle filter;  // size 0 (and offset 0): the table has no filter
  BlockHandle index;

  friend bool operator==(const TableFooter&, const TableFooter&) = default;
};

[[nodiscard]] std::string EncodeTableFooter(const TableFooter& footer);

// Parses and checks the last kTableFooterSize bytes of a table of
// `file_size` bytes: magic, then CRC, then version (NotSupported if unknown),
// then that both blocks and their trailers fit before the footer, the filter
// before the index. Corruption for anything else wrong.
[[nodiscard]] Result<TableFooter> DecodeTableFooter(std::string_view footer,
                                                    std::uint64_t file_size);

// Reads the block at `handle`, checks its trailer, and returns its contents.
// The block and trailer must end at or before `limit` (a file offset), which
// also bounds how much is allocated for a damaged handle. Corruption for a
// handle past `limit`, a short read, or a bad trailer; read errors as is.
[[nodiscard]] Result<std::string> ReadBlockContents(const RandomAccessFile& file,
                                                    const BlockHandle& handle, std::uint64_t limit);

}  // namespace forgedb
