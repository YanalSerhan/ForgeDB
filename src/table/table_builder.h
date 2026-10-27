// TableBuilder: writes a table file from entries in internal-key order.
//
//   TableBuilder builder(TableOptions{}, file.get());
//   for (each entry, in CompareInternalKeys order) builder.Add(internal_key, value);
//   FORGEDB_RETURN_IF_ERROR(builder.Finish());
//   FORGEDB_RETURN_IF_ERROR(file->Sync());   // the caller's job (ADR 0002)
//   FORGEDB_RETURN_IF_ERROR(file->Close());
//
// Data blocks are cut when they reach options.block_size; each gets an index
// entry (its last key -> its handle). Finish() appends the bloom filter over
// the user keys, the index block and the footer (docs/file-formats.md,
// "Table file"). Not thread-safe.
#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

#include "forgedb/status.h"

#include "env/file_system.h"
#include "table/block_builder.h"
#include "table/bloom_filter.h"
#include "table/table_format.h"

namespace forgedb {

// How tables are written. Internal for now: the public Options gain these
// fields when the DB writes tables (Phase 3.5).
struct TableOptions {
  // Uncompressed size at which a data block is cut. Blocks hold whole entries,
  // so a block is at least one entry, and may be larger than this.
  std::size_t block_size = 4096;
  // Entries between restart points in data blocks (the index block uses 1).
  std::size_t block_restart_interval = kDefaultBlockRestartInterval;
  // Bloom filter bits per distinct user key; 0 writes no filter.
  int filter_bits_per_key = 10;
};

class TableBuilder {
 public:
  // Writes into `file`, which must be empty and outlive the builder.
  // Preconditions: block_size >= 1, block_restart_interval >= 1,
  // filter_bits_per_key >= 0.
  TableBuilder(const TableOptions& options, WritableFile* file);

  // Appends an entry. Preconditions: Finish() not called; `internal_key` is a
  // valid internal key sorting after every key added before it (checked in
  // debug builds). After an error, Add does nothing.
  void Add(std::string_view internal_key, std::string_view value);

  // Writes any pending data block, the filter, the index and the footer. The
  // file is not synced or closed. Precondition: not called before.
  [[nodiscard]] Status Finish();

  // OK, or the first write error (it sticks).
  [[nodiscard]] Status status() const { return status_; }

  [[nodiscard]] std::uint64_t NumEntries() const noexcept { return entries_; }

  // Bytes written so far; after a successful Finish(), the file's size.
  [[nodiscard]] std::uint64_t FileSize() const noexcept { return offset_; }

 private:
  void FlushDataBlock();
  // Appends `contents` and its trailer; returns where they went.
  BlockHandle WriteBlock(std::string_view contents);

  TableOptions options_;
  WritableFile* file_;
  BlockBuilder data_block_;
  BlockBuilder index_block_;
  std::optional<BloomFilterBuilder> filter_;
  std::string last_key_;
  std::uint64_t entries_ = 0;
  std::uint64_t offset_ = 0;
  Status status_;
  bool finished_ = false;
};

}  // namespace forgedb
