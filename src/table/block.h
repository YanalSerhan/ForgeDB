// Block: a parsed block of a table file, and iterators over its entries.
//
//   FORGEDB_ASSIGN_OR_RETURN(const std::string_view contents, CheckBlockTrailer(stored));
//   FORGEDB_ASSIGN_OR_RETURN(Block block, Block::Open(std::string(contents)));
//   std::unique_ptr<Iterator> it = block.NewIterator(&CompareInternalKeys);
//   for (it->Seek(target); it->Valid(); it->Next()) { ... }
//   FORGEDB_RETURN_IF_ERROR(it->status());
//
// The bytes come from a file, so they are untrusted. Open() checks the restart
// array before anything else is read: a restart count of at least 1 that fits
// the block, the first restart point at offset 0, then strictly increasing
// offsets inside the entry area. The iterator bounds-checks every entry as it
// decodes it and requires a restart point's entry to store its key in full.
// Anything malformed ends iteration with a Corruption status; nothing is ever
// read outside the block.
//
// Not checked: that keys are in order. The block's CRC (checked before Open)
// rules out damage; out-of-order keys would take a buggy writer, and the
// builder checks the order in debug builds. With such a block a Seek may miss
// a key, but nothing reads out of bounds.
//
// Iterators are forward-only, like forgedb::Iterator: SeekToFirst, Seek and
// Next. Format details: docs/file-formats.md, "Block".
#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <utility>

#include "forgedb/iterator.h"
#include "forgedb/result.h"

#include "table/block_format.h"

namespace forgedb {

class Block {
 public:
  // Parses `contents` (a block without its trailer, already checked with
  // CheckBlockTrailer). Corruption if the restart array is malformed.
  [[nodiscard]] static Result<Block> Open(std::string contents);

  ~Block() = default;
  Block(const Block&) = delete;
  Block& operator=(const Block&) = delete;
  Block(Block&&) noexcept = default;
  Block& operator=(Block&&) noexcept = default;

  // The size of the contents in bytes.
  [[nodiscard]] std::size_t size() const noexcept { return contents_.size(); }

  // An iterator over the entries, ordered by `compare` (the comparison the
  // block was built with). The iterator points into this block: the block
  // must not be destroyed or moved while the iterator exists.
  [[nodiscard]] std::unique_ptr<Iterator> NewIterator(KeyCompare compare) const;

 private:
  Block(std::string contents, std::uint32_t restarts_offset, std::uint32_t num_restarts) noexcept
      : contents_(std::move(contents)),
        restarts_offset_(restarts_offset),
        num_restarts_(num_restarts) {}

  std::string contents_;
  std::uint32_t restarts_offset_;  // where the restart array starts: the end of the entries
  std::uint32_t num_restarts_;     // at least 1
};

}  // namespace forgedb
