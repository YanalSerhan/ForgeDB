// Table: an open, immutable table file.
//
//   FORGEDB_ASSIGN_OR_RETURN(std::unique_ptr<Table> table,
//                            Table::Open(std::move(file), file_size, &block_cache));
//   FORGEDB_ASSIGN_OR_RETURN(TableLookup found, table->Get(LookupKey(key, sequence), &value));
//
// Open() checks the footer and reads the index and filter blocks into memory
// (each CRC-checked); a damaged footer, index or filter fails the open. Data
// blocks are read on demand, CRC-checked on every read from the file, and
// kept in the block cache if one is given.
//
// Get() asks the bloom filter, then the index, then reads one data block.
// The iterator walks the index and opens data blocks as it goes (a "two-level"
// iterator); it stops with Corruption at the first bad block rather than skip
// data. Format: docs/file-formats.md, "Table file".
//
// Thread safety: Get and NewIterator may be called concurrently. A Table must
// outlive its iterators, and the block cache must outlive the Table.
#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <variant>

#include "forgedb/iterator.h"
#include "forgedb/result.h"

#include "db/internal_key.h"
#include "env/file_system.h"
#include "table/block.h"
#include "table/table_format.h"
#include "util/lru_cache.h"

namespace forgedb {

using BlockCache = ShardedLruCache<Block>;

enum class TableLookup : std::uint8_t {
  kNotFound,  // no version of the key at or below the lookup's sequence
  kFound,     // the newest such version is a value; it was copied out
  kDeleted,   // the newest such version is a deletion
};

// Counters, for tests and tools.
struct TableStats {
  std::uint64_t filter_rejections = 0;  // Gets answered by the filter alone
  std::uint64_t data_block_reads = 0;   // data blocks read from the file
  std::uint64_t data_block_cache_hits = 0;
};

class Table {
 public:
  // `block_cache` may be null (blocks are then read for each use).
  [[nodiscard]] static Result<std::unique_ptr<Table>> Open(std::unique_ptr<RandomAccessFile> file,
                                                           std::uint64_t file_size,
                                                           BlockCache* block_cache);

  ~Table() = default;
  Table(const Table&) = delete;
  Table& operator=(const Table&) = delete;
  Table(Table&&) = delete;
  Table& operator=(Table&&) = delete;

  // The newest version of key.UserKey() with sequence <= the lookup's.
  // On kFound, assigns the value to *value. Corruption if a block is bad.
  [[nodiscard]] Result<TableLookup> Get(const LookupKey& key, std::string* value) const;

  // Every entry (internal key -> value) in order.
  [[nodiscard]] std::unique_ptr<Iterator> NewIterator() const;

  [[nodiscard]] bool HasFilter() const noexcept { return filter_.has_value(); }
  [[nodiscard]] TableStats GetStats() const noexcept;

  // A block of this table, cached or owned. Public for the iterator.
  using BlockRef = std::variant<BlockCache::Handle, std::unique_ptr<Block>>;
  [[nodiscard]] Result<BlockRef> ReadDataBlock(const BlockHandle& handle) const;

 private:
  Table(std::unique_ptr<RandomAccessFile> file, BlockCache* block_cache, Block index,
        std::optional<std::string> filter, std::uint64_t data_end) noexcept;

  std::unique_ptr<RandomAccessFile> file_;
  BlockCache* block_cache_;
  std::uint64_t cache_id_;
  Block index_;
  std::optional<std::string> filter_;
  std::uint64_t data_end_;  // data blocks end here: the filter or index starts

  mutable std::atomic<std::uint64_t> filter_rejections_{0};
  mutable std::atomic<std::uint64_t> data_block_reads_{0};
  mutable std::atomic<std::uint64_t> data_block_cache_hits_{0};
};

// The block a BlockRef refers to.
[[nodiscard]] const Block& BlockOf(const Table::BlockRef& ref) noexcept;

}  // namespace forgedb
