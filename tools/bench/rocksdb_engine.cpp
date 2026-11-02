#include "tools/bench/rocksdb_engine.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <utility>

#include "forgedb/iterator.h"
#include "forgedb/result.h"
#include "forgedb/status.h"

#include "tools/bench/engine.h"

#include <rocksdb/cache.h>
#include <rocksdb/db.h>
#include <rocksdb/filter_policy.h>
#include <rocksdb/iterator.h>
#include <rocksdb/options.h>
#include <rocksdb/slice.h>
#include <rocksdb/statistics.h>
#include <rocksdb/status.h>
#include <rocksdb/table.h>

namespace forgedb::bench {
namespace {

Status FromRocks(const rocksdb::Status& status) {
  if (status.ok()) {
    return Status::OK();
  }
  if (status.IsCorruption()) {
    return Status::Corruption("rocksdb", status.ToString());
  }
  return Status::IOError("rocksdb", status.ToString());
}

rocksdb::Slice ToSlice(std::string_view text) { return {text.data(), text.size()}; }
std::string_view ToView(const rocksdb::Slice& slice) { return {slice.data(), slice.size()}; }

class RocksIterator final : public Iterator {
 public:
  explicit RocksIterator(std::unique_ptr<rocksdb::Iterator> it) : it_(std::move(it)) {}

  [[nodiscard]] bool Valid() const override { return it_->Valid(); }
  void SeekToFirst() override { it_->SeekToFirst(); }
  void Seek(std::string_view target) override { it_->Seek(ToSlice(target)); }
  void Next() override { it_->Next(); }
  [[nodiscard]] std::string_view key() const override { return ToView(it_->key()); }
  [[nodiscard]] std::string_view value() const override { return ToView(it_->value()); }
  [[nodiscard]] Status status() const override { return FromRocks(it_->status()); }

 private:
  std::unique_ptr<rocksdb::Iterator> it_;
};

// The statistics tickers Counters() reports.
constexpr std::array<rocksdb::Tickers, 5> kTickers = {
    rocksdb::BYTES_WRITTEN,       rocksdb::WAL_FILE_BYTES, rocksdb::FLUSH_WRITE_BYTES,
    rocksdb::COMPACT_WRITE_BYTES, rocksdb::STALL_MICROS,
};

class RocksEngine final : public Engine {
 public:
  RocksEngine(std::unique_ptr<rocksdb::DB> db, std::shared_ptr<rocksdb::Statistics> statistics,
              std::filesystem::path dir, bool sync)
      : db_(std::move(db)), statistics_(std::move(statistics)), dir_(std::move(dir)) {
    write_.sync = sync;
  }

  Status Put(std::string_view key, std::string_view value) override {
    return FromRocks(db_->Put(write_, ToSlice(key), ToSlice(value)));
  }

  Result<bool> Get(std::string_view key, std::string& value) override {
    const rocksdb::Status status = db_->Get(rocksdb::ReadOptions(), ToSlice(key), &value);
    if (status.IsNotFound()) {
      return false;
    }
    FORGEDB_RETURN_IF_ERROR(FromRocks(status));
    return true;
  }

  std::unique_ptr<Iterator> NewIterator() override {
    return std::make_unique<RocksIterator>(
        std::unique_ptr<rocksdb::Iterator>(db_->NewIterator(rocksdb::ReadOptions())));
  }

  Status WaitForBackgroundWork() override {
    return FromRocks(db_->WaitForCompact(rocksdb::WaitForCompactOptions()));
  }

  Result<std::uint64_t> DiskBytes() override { return FolderBytes(dir_); }

  std::map<std::string, double> Counters() override {
    std::map<std::string, double> counters;
    for (const auto& [ticker, name] : rocksdb::TickersNameMap) {
      for (const rocksdb::Tickers wanted : kTickers) {
        if (ticker == wanted) {
          counters[name] = static_cast<double>(statistics_->getTickerCount(ticker));
        }
      }
    }
    return counters;
  }

 private:
  std::unique_ptr<rocksdb::DB> db_;
  std::shared_ptr<rocksdb::Statistics> statistics_;
  std::filesystem::path dir_;
  rocksdb::WriteOptions write_;
};

}  // namespace

Result<std::unique_ptr<Engine>> OpenRocksDB(const std::filesystem::path& dir,
                                            const EngineOptions& options) {
  rocksdb::BlockBasedTableOptions table;
  if (options.cache_bytes == 0) {
    table.no_block_cache = true;
  } else {
    table.block_cache = rocksdb::NewLRUCache(static_cast<std::size_t>(options.cache_bytes));
  }
  if (options.bloom_bits_per_key > 0) {
    table.filter_policy.reset(rocksdb::NewBloomFilterPolicy(options.bloom_bits_per_key));
  }

  rocksdb::Options db_options;
  db_options.create_if_missing = true;
  db_options.error_if_exists = true;
  db_options.compression = rocksdb::kNoCompression;
  db_options.write_buffer_size = static_cast<std::size_t>(options.memtable_bytes);
  db_options.table_factory.reset(rocksdb::NewBlockBasedTableFactory(table));
  db_options.statistics = rocksdb::CreateDBStatistics();

  // RocksDB's Windows build takes file names in the ANSI code page, which is
  // what path::string() gives.
  std::unique_ptr<rocksdb::DB> db;
  FORGEDB_RETURN_IF_ERROR(FromRocks(rocksdb::DB::Open(db_options, dir.string(), &db)));
  return std::unique_ptr<Engine>(
      std::make_unique<RocksEngine>(std::move(db), db_options.statistics, dir, options.sync));
}

}  // namespace forgedb::bench
