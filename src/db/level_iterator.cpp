#include "db/level_iterator.h"

#include <algorithm>
#include <cstddef>
#include <memory>
#include <string_view>
#include <utility>
#include <vector>

#include "forgedb/check.h"
#include "forgedb/iterator.h"
#include "forgedb/result.h"
#include "forgedb/status.h"

#include "db/internal_key.h"
#include "db/table_cache.h"
#include "db/version_edit.h"
#include "db/version_set.h"

namespace forgedb {
namespace {

class LevelIterator final : public Iterator {
 public:
  LevelIterator(TableCache* table_cache, std::shared_ptr<const Version> version, int level)
      : table_cache_(table_cache),
        version_(std::move(version)),
        files_(&version_->files.at(static_cast<std::size_t>(level))) {}

  [[nodiscard]] bool Valid() const override {
    return status_.ok() && table_ != nullptr && table_->Valid();
  }

  void SeekToFirst() override {
    if (status_.ok() && Open(0)) {
      table_->SeekToFirst();
      SkipFinishedTables();
    }
  }

  void Seek(std::string_view target) override {
    if (!status_.ok()) {
      return;
    }
    // The first table whose largest key is at or after the target.
    const auto it = std::ranges::partition_point(*files_, [target](const FileMetaData& file) {
      return CompareInternalKeys(file.largest, target) < 0;
    });
    if (Open(static_cast<std::size_t>(it - files_->begin()))) {
      table_->Seek(target);
      SkipFinishedTables();
    }
  }

  void Next() override {
    FORGEDB_DCHECK(Valid());
    table_->Next();
    SkipFinishedTables();
  }

  [[nodiscard]] std::string_view key() const override {
    FORGEDB_DCHECK(Valid());
    return table_->key();
  }

  [[nodiscard]] std::string_view value() const override {
    FORGEDB_DCHECK(Valid());
    return table_->value();
  }

  [[nodiscard]] Status status() const override { return status_; }

 private:
  // Opens table `index` (false past the last one, or on error).
  bool Open(std::size_t index) {
    table_.reset();
    index_ = index;
    if (index >= files_->size()) {
      return false;
    }
    const FileMetaData& file = (*files_)[index];
    Result<std::unique_ptr<Iterator>> table =
        table_cache_->NewIterator(file.number, file.file_size);
    if (!table.ok()) {
      status_ = table.status();
      return false;
    }
    table_ = std::move(table).value();
    return true;
  }

  // While the current table is used up, moves to the next one.
  void SkipFinishedTables() {
    while (status_.ok() && table_ != nullptr && !table_->Valid()) {
      if (!table_->status().ok()) {
        status_ = table_->status();
        return;
      }
      if (Open(index_ + 1)) {
        table_->SeekToFirst();
      }
    }
  }

  TableCache* table_cache_;
  std::shared_ptr<const Version> version_;
  const std::vector<FileMetaData>* files_;  // in version_
  std::size_t index_ = 0;
  std::unique_ptr<Iterator> table_;  // the current table's iterator; null past the end
  Status status_;
};

}  // namespace

std::unique_ptr<Iterator> NewLevelIterator(TableCache* table_cache,
                                           std::shared_ptr<const Version> version, int level) {
  return std::make_unique<LevelIterator>(table_cache, std::move(version), level);
}

}  // namespace forgedb
