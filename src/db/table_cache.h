// TableCache: keeps recently used tables open, keyed by file number.
//
// Opening a table reads its footer, index and filter, so the DB keeps up to
// `max_open_tables` of them open, least recently used first out (exactly for
// small limits: one cache shard per 64 tables, at most 16 shards). A miss
// opens NNNNNN.sst through the FileSystem. Every table shares one block
// cache.
//
// Thread safety: every method may be called concurrently. Iterators keep
// their table open (they hold a cache handle) even if it is evicted; they must
// be destroyed before the TableCache, which must be destroyed before the
// block cache.
#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>

#include "forgedb/iterator.h"
#include "forgedb/result.h"

#include "db/internal_key.h"
#include "env/file_system.h"
#include "table/table.h"
#include "util/lru_cache.h"

namespace forgedb {

class TableCache {
 public:
  // `fs` and `block_cache` (which may be null) must outlive the cache.
  // Precondition: max_open_tables >= 1.
  TableCache(FileSystem* fs, std::filesystem::path db_dir, std::size_t max_open_tables,
             BlockCache* block_cache);

  // Table::Get on table `file_number`, `file_size` bytes long (the size comes
  // from the MANIFEST, so a truncated file is detected).
  [[nodiscard]] Result<TableLookup> Get(std::uint64_t file_number, std::uint64_t file_size,
                                        const LookupKey& key, std::string* value);

  // An iterator over table `file_number`, which it keeps open.
  [[nodiscard]] Result<std::unique_ptr<Iterator>> NewIterator(std::uint64_t file_number,
                                                              std::uint64_t file_size);

  // Closes table `file_number` once no iterator uses it (its file is gone).
  void Evict(std::uint64_t file_number);

  [[nodiscard]] CacheStats GetStats() const { return cache_.GetStats(); }

 private:
  using Cache = ShardedLruCache<std::unique_ptr<Table>>;

  [[nodiscard]] Result<Cache::Handle> FindTable(std::uint64_t file_number, std::uint64_t file_size);

  FileSystem* fs_;
  std::filesystem::path db_dir_;
  BlockCache* block_cache_;
  Cache cache_;
};

}  // namespace forgedb
