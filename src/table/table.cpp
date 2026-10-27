#include "table/table.h"

#include <atomic>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <variant>

#include "forgedb/check.h"
#include "forgedb/iterator.h"
#include "forgedb/result.h"
#include "forgedb/status.h"

#include "db/internal_key.h"
#include "table/block.h"
#include "table/bloom_filter.h"
#include "table/table_format.h"
#include "util/coding.h"

namespace forgedb {

namespace {

// Decodes the handle stored in an index entry.
Result<BlockHandle> IndexEntryHandle(std::string_view value) {
  const std::optional<BlockHandle> handle = DecodeBlockHandle(value);
  if (!handle) {
    return Status::Corruption("malformed block handle in table index");
  }
  return *handle;
}

// Index iterator -> data block iterator. Forward-only; the first error stops
// it for good (sticky), so a damaged block is never skipped silently.
class TableIterator final : public Iterator {
 public:
  TableIterator(const Table* table, std::unique_ptr<Iterator> index)
      : table_(table), index_(std::move(index)) {}

  [[nodiscard]] bool Valid() const override {
    return status_.ok() && data_ != nullptr && data_->Valid();
  }

  void SeekToFirst() override {
    if (!status_.ok()) {
      return;
    }
    index_->SeekToFirst();
    if (LoadBlockAtIndex()) {
      data_->SeekToFirst();
    }
    SkipFinishedBlocks();
  }

  void Seek(std::string_view target) override {
    if (!status_.ok()) {
      return;
    }
    index_->Seek(target);
    if (LoadBlockAtIndex()) {
      data_->Seek(target);
    }
    SkipFinishedBlocks();
  }

  void Next() override {
    FORGEDB_DCHECK(Valid());
    data_->Next();
    SkipFinishedBlocks();
  }

  [[nodiscard]] std::string_view key() const override {
    FORGEDB_DCHECK(Valid());
    return data_->key();
  }

  [[nodiscard]] std::string_view value() const override {
    FORGEDB_DCHECK(Valid());
    return data_->value();
  }

  [[nodiscard]] Status status() const override { return status_; }

 private:
  // Opens the data block the index points at. False at the end of the index
  // or on error (recorded in status_).
  bool LoadBlockAtIndex() {
    data_.reset();
    block_.reset();
    if (!index_->Valid()) {
      status_ = index_->status();
      return false;
    }
    Result<BlockHandle> handle = IndexEntryHandle(index_->value());
    if (!handle.ok()) {
      status_ = std::move(handle).status();
      return false;
    }
    Result<Table::BlockRef> block = table_->ReadDataBlock(*handle);
    if (!block.ok()) {
      status_ = std::move(block).status();
      return false;
    }
    block_.emplace(std::move(block).value());
    data_ = BlockOf(*block_).NewIterator(&CompareInternalKeys);
    return true;
  }

  // While the current block is used up, moves to the next one.
  void SkipFinishedBlocks() {
    while (status_.ok() && data_ != nullptr && !data_->Valid()) {
      if (!data_->status().ok()) {
        status_ = data_->status();
        return;
      }
      index_->Next();
      if (LoadBlockAtIndex()) {
        data_->SeekToFirst();
      }
    }
  }

  const Table* table_;
  std::unique_ptr<Iterator> index_;
  // Declared before data_, so data_ (which points into the block) goes first.
  std::optional<Table::BlockRef> block_;
  std::unique_ptr<Iterator> data_;
  Status status_;
};

}  // namespace

const Block& BlockOf(const Table::BlockRef& ref) noexcept {
  if (const auto* handle = std::get_if<BlockCache::Handle>(&ref)) {
    return **handle;
  }
  return *std::get<std::unique_ptr<Block>>(ref);
}

Table::Table(std::unique_ptr<RandomAccessFile> file, BlockCache* block_cache, Block index,
             std::optional<std::string> filter, std::uint64_t data_end) noexcept
    : file_(std::move(file)),
      block_cache_(block_cache),
      cache_id_(block_cache != nullptr ? block_cache->NewId() : 0),
      index_(std::move(index)),
      filter_(std::move(filter)),
      data_end_(data_end) {}

Result<std::unique_ptr<Table>> Table::Open(std::unique_ptr<RandomAccessFile> file,
                                           std::uint64_t file_size, BlockCache* block_cache) {
  if (file_size < kTableFooterSize) {
    return Status::Corruption("file too short to be a table", std::to_string(file_size) + " bytes");
  }
  std::string footer_bytes(kTableFooterSize, '\0');
  FORGEDB_ASSIGN_OR_RETURN(const std::size_t read,
                           file->Read(file_size - kTableFooterSize, std::span<char>(footer_bytes)));
  if (read != kTableFooterSize) {
    return Status::Corruption("truncated table footer read");
  }
  FORGEDB_ASSIGN_OR_RETURN(const TableFooter footer, DecodeTableFooter(footer_bytes, file_size));

  const std::uint64_t footer_start = file_size - kTableFooterSize;
  FORGEDB_ASSIGN_OR_RETURN(std::string index_contents,
                           ReadBlockContents(*file, footer.index, footer_start));
  FORGEDB_ASSIGN_OR_RETURN(Block index, Block::Open(std::move(index_contents)));

  std::optional<std::string> filter;
  std::uint64_t data_end = footer.index.offset;
  if (footer.filter.size != 0) {
    FORGEDB_ASSIGN_OR_RETURN(std::string filter_contents,
                             ReadBlockContents(*file, footer.filter, footer.index.offset));
    filter = std::move(filter_contents);
    data_end = footer.filter.offset;
  }
  return std::unique_ptr<Table>(
      new Table(std::move(file), block_cache, std::move(index), std::move(filter), data_end));
}

Result<Table::BlockRef> Table::ReadDataBlock(const BlockHandle& handle) const {
  std::string key;
  if (block_cache_ != nullptr) {
    PutFixed64(key, cache_id_);
    PutFixed64(key, handle.offset);
    if (BlockCache::Handle cached = block_cache_->Lookup(key)) {
      data_block_cache_hits_.fetch_add(1, std::memory_order_relaxed);
      return BlockRef(std::move(cached));
    }
  }
  FORGEDB_ASSIGN_OR_RETURN(std::string contents, ReadBlockContents(*file_, handle, data_end_));
  data_block_reads_.fetch_add(1, std::memory_order_relaxed);
  FORGEDB_ASSIGN_OR_RETURN(Block block, Block::Open(std::move(contents)));
  if (block_cache_ != nullptr) {
    const std::size_t charge = block.size();
    return BlockRef(block_cache_->Insert(key, std::move(block), charge));
  }
  return BlockRef(std::make_unique<Block>(std::move(block)));
}

Result<TableLookup> Table::Get(const LookupKey& key, std::string* value) const {
  if (filter_ && !BloomFilterMayMatch(*filter_, key.UserKey())) {
    filter_rejections_.fetch_add(1, std::memory_order_relaxed);
    return TableLookup::kNotFound;
  }
  const std::unique_ptr<Iterator> index = index_.NewIterator(&CompareInternalKeys);
  index->Seek(key.InternalKey());
  if (!index->Valid()) {
    FORGEDB_RETURN_IF_ERROR(index->status());
    return TableLookup::kNotFound;  // past the last key of the table
  }
  FORGEDB_ASSIGN_OR_RETURN(const BlockHandle handle, IndexEntryHandle(index->value()));
  FORGEDB_ASSIGN_OR_RETURN(const BlockRef block, ReadDataBlock(handle));

  const std::unique_ptr<Iterator> it = BlockOf(block).NewIterator(&CompareInternalKeys);
  it->Seek(key.InternalKey());
  if (!it->Valid()) {
    // The index said this block holds a key >= the target; if not, the
    // block and the index disagree.
    FORGEDB_RETURN_IF_ERROR(it->status());
    return Status::Corruption("table data block ends before its index key");
  }
  const std::optional<ParsedInternalKey> found = ParseInternalKey(it->key());
  if (!found) {
    return Status::Corruption("malformed internal key in table");
  }
  if (found->user_key != key.UserKey()) {
    return TableLookup::kNotFound;
  }
  if (found->type == ValueType::kDeletion) {
    return TableLookup::kDeleted;
  }
  value->assign(it->value());
  return TableLookup::kFound;
}

std::unique_ptr<Iterator> Table::NewIterator() const {
  return std::make_unique<TableIterator>(this, index_.NewIterator(&CompareInternalKeys));
}

TableStats Table::GetStats() const noexcept {
  return {
      .filter_rejections = filter_rejections_.load(std::memory_order_relaxed),
      .data_block_reads = data_block_reads_.load(std::memory_order_relaxed),
      .data_block_cache_hits = data_block_cache_hits_.load(std::memory_order_relaxed),
  };
}

}  // namespace forgedb
