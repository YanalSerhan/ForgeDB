// WriteBatch: a group of Puts and Deletes applied atomically, in order.
//
//   forgedb::WriteBatch batch;
//   batch.Delete("old-key");
//   batch.Put("new-key", "value");
//   FORGEDB_RETURN_IF_ERROR(db->Write(write_options, batch));  // Phase 1.6
//
// Either every operation in a batch becomes visible or none does, and within a
// batch later operations win (Put then Delete of one key deletes it).
//
// A batch is just its encoded bytes, which are also what the write-ahead log
// stores (docs/file-formats.md, "Write batch"). Not thread-safe: a batch is
// built by one thread, then handed to the DB.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

#include "forgedb/status.h"

namespace forgedb {

class WriteBatch {
 public:
  // Receives a batch's operations, in order, from Iterate().
  class Handler {
   public:
    Handler() = default;
    virtual ~Handler();  // defined in the library, so the vtable lives there once
    Handler(const Handler&) = delete;
    Handler& operator=(const Handler&) = delete;
    Handler(Handler&&) = delete;
    Handler& operator=(Handler&&) = delete;

    virtual void Put(std::string_view key, std::string_view value) = 0;
    virtual void Delete(std::string_view key) = 0;
  };

  WriteBatch();

  // Records "set key to value" / "remove key". Keys and values must each be
  // smaller than 4 GiB (lengths are stored in 32 bits); the DB enforces much
  // lower limits before a batch gets this far.
  void Put(std::string_view key, std::string_view value);
  void Delete(std::string_view key);

  // Removes every operation, leaving an empty batch.
  void Clear();

  // Number of operations (Puts plus Deletes).
  [[nodiscard]] std::uint32_t Count() const noexcept;

  // Size of the encoded batch in bytes; what it costs to log it.
  [[nodiscard]] std::size_t ApproximateSize() const noexcept { return rep_.size(); }

  // Calls `handler` for every operation, in order. The bytes are validated
  // first, so on a malformed batch this returns a Corruption status without
  // having called the handler at all: a handler sees all of a batch or none.
  // Batches built with Put/Delete are always well-formed; malformed ones come
  // from damaged bytes (e.g. a write-ahead log being recovered).
  [[nodiscard]] Status Iterate(Handler& handler) const;

 private:
  friend class WriteBatchInternal;  // sequence number and raw bytes, for the engine

  std::string rep_;  // the encoded batch: 12-byte header, then the records
};

}  // namespace forgedb
