// DB: an embedded, ordered key-value store.
//
//   FORGEDB_ASSIGN_OR_RETURN(std::unique_ptr<forgedb::DB> db,
//                            forgedb::DB::Open(forgedb::Options{}, L"C:\\data\\mydb"));
//   FORGEDB_RETURN_IF_ERROR(db->Put({}, "key", "value"));
//   forgedb::Result<std::string> value = db->Get({}, "key");
//
// Keys and values are arbitrary bytes. Keys are ordered bytewise (as unsigned
// bytes, like memcmp).
//
// Thread safety: every DB method may be called from any number of threads at
// once. Iterators are not shared: each is used by one thread at a time.
//
// Phase 1: the database lives in memory only. `name` is accepted so the API
// does not change, but nothing is written to disk until the write-ahead log
// (Phase 2); everything is lost when the DB is destroyed.
#pragma once

#include <cstddef>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>

#include "forgedb/iterator.h"
#include "forgedb/options.h"
#include "forgedb/result.h"
#include "forgedb/status.h"
#include "forgedb/write_batch.h"

namespace forgedb {

// Size limits, checked by Put, Delete and Write (InvalidArgument when
// exceeded). Generous for keys, and keep a single value well below the
// 4 GiB the formats could encode.
inline constexpr std::size_t kMaxKeySize = std::size_t{64} * 1024;            // 64 KiB
inline constexpr std::size_t kMaxValueSize = std::size_t{256} * 1024 * 1024;  // 256 MiB

class DB {
 public:
  // Opens the database called `name` (a directory path, from Phase 2 on).
  // InvalidArgument if `name` is empty.
  [[nodiscard]] static Result<std::unique_ptr<DB>> Open(const Options& options,
                                                        const std::filesystem::path& name);

  DB() = default;
  virtual ~DB();
  DB(const DB&) = delete;
  DB& operator=(const DB&) = delete;
  DB(DB&&) = delete;
  DB& operator=(DB&&) = delete;

  // Sets `key` to `value`, replacing any previous value.
  [[nodiscard]] Status Put(const WriteOptions& options, std::string_view key,
                           std::string_view value);

  // Removes `key`. Removing a key that does not exist is not an error.
  [[nodiscard]] Status Delete(const WriteOptions& options, std::string_view key);

  // Applies every operation in `batch` atomically: readers see all of them or
  // none. An empty batch is a no-op.
  [[nodiscard]] virtual Status Write(const WriteOptions& options, const WriteBatch& batch) = 0;

  // The value of `key`, or a NotFound status if it has none.
  [[nodiscard]] virtual Result<std::string> Get(const ReadOptions& options,
                                                std::string_view key) = 0;

  // A new iterator over the whole database, as of now (see forgedb/iterator.h).
  [[nodiscard]] virtual std::unique_ptr<Iterator> NewIterator(const ReadOptions& options) = 0;
};

}  // namespace forgedb
