// WriteBatchInternal: the parts of WriteBatch only the engine uses, kept out
// of the public header (forgedb/write_batch.h).
//
// Encoding (docs/file-formats.md, "Write batch"):
//
//   [ fixed64 sequence ][ fixed32 count ][ record ]*
//   record := 0x01 varint32-prefixed key, varint32-prefixed value   (Put)
//           | 0x00 varint32-prefixed key                            (Delete)
//
// The record type bytes are ValueType's values (db/internal_key.h).
#pragma once

#include <cstddef>
#include <cstdint>
#include <string_view>

#include "forgedb/status.h"
#include "forgedb/write_batch.h"

#include "db/internal_key.h"

namespace forgedb {

class MemTable;

class WriteBatchInternal {
 public:
  // Size of the fixed header: sequence (8) + count (4).
  static constexpr std::size_t kHeaderSize = 12;

  static void SetCount(WriteBatch& batch, std::uint32_t count) noexcept;

  // The sequence number of the batch's first operation; operation i gets
  // Sequence() + i. Assigned by the DB when the batch is written.
  [[nodiscard]] static SequenceNumber Sequence(const WriteBatch& batch) noexcept;
  static void SetSequence(WriteBatch& batch, SequenceNumber sequence) noexcept;

  // The encoded bytes, e.g. to append to the write-ahead log.
  [[nodiscard]] static std::string_view Contents(const WriteBatch& batch) noexcept {
    return batch.rep_;
  }

  // Replaces the batch with `contents`, e.g. a record read back from the log.
  // The bytes are untrusted: Corruption if they are too short to hold a
  // header (the batch is then left empty). Everything else is checked by
  // Iterate() and InsertInto(), which reject malformed records.
  [[nodiscard]] static Status SetContents(WriteBatch& batch, std::string_view contents);

  // Applies every operation to `memtable` with sequence numbers Sequence(),
  // Sequence() + 1, ... The caller must be the memtable's only writer.
  // Returns Corruption, having changed nothing, if the batch is malformed or
  // its sequence numbers would exceed kMaxSequenceNumber.
  [[nodiscard]] static Status InsertInto(const WriteBatch& batch, MemTable& memtable);
};

}  // namespace forgedb
