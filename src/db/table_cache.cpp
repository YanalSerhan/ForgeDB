#include "db/table_cache.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
#include <utility>

#include "forgedb/check.h"
#include "forgedb/iterator.h"
#include "forgedb/result.h"
#include "forgedb/status.h"

#include "db/filename.h"
#include "util/coding.h"

namespace forgedb {

namespace {

std::string CacheKey(std::uint64_t file_number) {
  std::string key;
  PutFixed64(key, file_number);
  return key;
}

// Wraps a table's iterator and holds the cache handle that keeps the table
// open while the iterator exists.
template <typename Handle>
class TableHandleIterator final : public Iterator {
 public:
  TableHandleIterator(Handle handle, std::unique_ptr<Iterator> inner)
      : handle_(std::move(handle)), inner_(std::move(inner)) {}

  [[nodiscard]] bool Valid() const override { return inner_->Valid(); }
  void SeekToFirst() override { inner_->SeekToFirst(); }
  void Seek(std::string_view target) override { inner_->Seek(target); }
  void Next() override { inner_->Next(); }
  [[nodiscard]] std::string_view key() const override { return inner_->key(); }
  [[nodiscard]] std::string_view value() const override { return inner_->value(); }
  [[nodiscard]] Status status() const override { return inner_->status(); }

 private:
  Handle handle_;                    // destroyed after inner_
  std::unique_ptr<Iterator> inner_;  // points into the table handle_ keeps open
};

}  // namespace

TableCache::TableCache(FileSystem* fs, std::filesystem::path db_dir, std::size_t max_open_tables,
                       BlockCache* block_cache)
    : fs_(fs),
      db_dir_(std::move(db_dir)),
      block_cache_(block_cache),
      cache_(max_open_tables,
             std::clamp<std::size_t>(max_open_tables / 64, 1, kDefaultCacheShards)) {
  FORGEDB_CHECK(max_open_tables >= 1);
}

Result<TableCache::Cache::Handle> TableCache::FindTable(std::uint64_t file_number,
                                                        std::uint64_t file_size) {
  const std::string key = CacheKey(file_number);
  if (Cache::Handle handle = cache_.Lookup(key)) {
    return handle;
  }
  // Two threads missing the same table both open it; the second Insert
  // replaces the first, which closes when its last user is done.
  FORGEDB_ASSIGN_OR_RETURN(std::unique_ptr<RandomAccessFile> file,
                           fs_->NewRandomAccessFile(TableFileName(db_dir_, file_number)));
  FORGEDB_ASSIGN_OR_RETURN(std::unique_ptr<Table> table,
                           Table::Open(std::move(file), file_size, block_cache_));
  return cache_.Insert(key, std::move(table), 1);
}

Result<TableLookup> TableCache::Get(std::uint64_t file_number, std::uint64_t file_size,
                                    const LookupKey& key, std::string* value) {
  FORGEDB_ASSIGN_OR_RETURN(const Cache::Handle handle, FindTable(file_number, file_size));
  return (*handle)->Get(key, value);
}

Result<std::unique_ptr<Iterator>> TableCache::NewIterator(std::uint64_t file_number,
                                                          std::uint64_t file_size) {
  FORGEDB_ASSIGN_OR_RETURN(Cache::Handle handle, FindTable(file_number, file_size));
  std::unique_ptr<Iterator> inner = (*handle)->NewIterator();
  return std::unique_ptr<Iterator>(
      std::make_unique<TableHandleIterator<Cache::Handle>>(std::move(handle), std::move(inner)));
}

void TableCache::Evict(std::uint64_t file_number) { cache_.Erase(CacheKey(file_number)); }

}  // namespace forgedb
