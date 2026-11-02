#include "tools/bench/leveldb_engine.h"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <thread>
#include <utility>

#include "forgedb/iterator.h"
#include "forgedb/result.h"
#include "forgedb/status.h"

#include "tools/bench/engine.h"
#include "tools/bench/leveldb_properties.h"

#include <leveldb/cache.h>
#include <leveldb/db.h>
#include <leveldb/filter_policy.h>
#include <leveldb/iterator.h>
#include <leveldb/options.h>
#include <leveldb/slice.h>
#include <leveldb/status.h>

namespace forgedb::bench {
namespace {

Status FromLevelDB(const leveldb::Status& status) {
  if (status.ok()) {
    return Status::OK();
  }
  if (status.IsCorruption()) {
    return Status::Corruption("leveldb", status.ToString());
  }
  return Status::IOError("leveldb", status.ToString());
}

leveldb::Slice ToSlice(std::string_view text) { return {text.data(), text.size()}; }
std::string_view ToView(const leveldb::Slice& slice) { return {slice.data(), slice.size()}; }

class LevelDBIterator final : public Iterator {
 public:
  explicit LevelDBIterator(std::unique_ptr<leveldb::Iterator> it) : it_(std::move(it)) {}

  [[nodiscard]] bool Valid() const override { return it_->Valid(); }
  void SeekToFirst() override { it_->SeekToFirst(); }
  void Seek(std::string_view target) override { it_->Seek(ToSlice(target)); }
  void Next() override { it_->Next(); }
  [[nodiscard]] std::string_view key() const override { return ToView(it_->key()); }
  [[nodiscard]] std::string_view value() const override { return ToView(it_->value()); }
  [[nodiscard]] Status status() const override { return FromLevelDB(it_->status()); }

 private:
  std::unique_ptr<leveldb::Iterator> it_;
};

class LevelDBEngine final : public Engine {
 public:
  // `cache` and `filter` (either may be null) are the ones in the options
  // `db` was opened with; they must outlive it.
  LevelDBEngine(std::unique_ptr<leveldb::Cache> cache,
                std::unique_ptr<const leveldb::FilterPolicy> filter,
                std::unique_ptr<leveldb::DB> db, std::filesystem::path dir, bool sync)
      : cache_(std::move(cache)),
        filter_(std::move(filter)),
        db_(std::move(db)),
        dir_(std::move(dir)) {
    write_.sync = sync;
  }

  Status Put(std::string_view key, std::string_view value) override {
    return FromLevelDB(db_->Put(write_, ToSlice(key), ToSlice(value)));
  }

  Result<bool> Get(std::string_view key, std::string& value) override {
    const leveldb::Status status = db_->Get(leveldb::ReadOptions(), ToSlice(key), &value);
    if (status.IsNotFound()) {
      return false;
    }
    FORGEDB_RETURN_IF_ERROR(FromLevelDB(status));
    return true;
  }

  std::unique_ptr<Iterator> NewIterator() override {
    return std::make_unique<LevelDBIterator>(
        std::unique_ptr<leveldb::Iterator>(db_->NewIterator(leveldb::ReadOptions())));
  }

  Status WaitForBackgroundWork() override {
    using Clock = std::chrono::steady_clock;
    constexpr auto kPoll = std::chrono::milliseconds(20);
    constexpr auto kQuiet = std::chrono::seconds(1);
    constexpr auto kLimit = std::chrono::minutes(30);
    const Clock::time_point start = Clock::now();
    Clock::time_point changed = start;
    std::string last;
    for (;;) {
      FORGEDB_ASSIGN_OR_RETURN(std::string tables, Property("leveldb.sstables"));
      FORGEDB_ASSIGN_OR_RETURN(const LevelDBTables parsed, ParseLevelDBTables(tables));
      const Clock::time_point now = Clock::now();
      if (tables != last || LevelDBCompactionScore(parsed) >= 1) {
        last = std::move(tables);
        changed = now;
      } else if (now - changed >= kQuiet) {
        return Status::OK();
      }
      if (now - start >= kLimit) {
        return Status::Busy("LevelDB's compactions did not settle within 30 minutes");
      }
      std::this_thread::sleep_for(kPoll);
    }
  }

  Result<std::uint64_t> DiskBytes() override { return FolderBytes(dir_); }

  std::map<std::string, double> Counters() override {
    std::map<std::string, double> counters;
    const Result<std::string> tables = Property("leveldb.sstables");
    const Result<LevelDBTables> parsed =
        tables.ok() ? ParseLevelDBTables(*tables) : Result<LevelDBTables>(tables.status());
    if (parsed.ok()) {
      double files = 0;
      double bytes = 0;
      for (std::size_t level = 0; level < kLevelDBLevels; ++level) {
        files += static_cast<double>(parsed->files.at(level));
        bytes += static_cast<double>(parsed->bytes.at(level));
      }
      counters["tables"] = files;
      counters["table_bytes"] = bytes;
    }
    const Result<std::string> stats = Property("leveldb.stats");
    const Result<LevelDBCompactionStats> compactions =
        stats.ok() ? ParseLevelDBStats(*stats) : Result<LevelDBCompactionStats>(stats.status());
    if (compactions.ok()) {
      counters["compaction_seconds"] = compactions->seconds;
      counters["compaction_read_mb"] = compactions->read_mb;
      counters["compaction_write_mb"] = compactions->write_mb;
    }
    return counters;
  }

 private:
  Result<std::string> Property(std::string_view name) {
    std::string value;
    if (!db_->GetProperty(ToSlice(name), &value)) {
      return Status::NotSupported("LevelDB has no property", name);
    }
    return value;
  }

  // Declared before db_, so they are destroyed after it.
  std::unique_ptr<leveldb::Cache> cache_;
  std::unique_ptr<const leveldb::FilterPolicy> filter_;
  std::unique_ptr<leveldb::DB> db_;
  std::filesystem::path dir_;
  leveldb::WriteOptions write_;
};

}  // namespace

Result<std::unique_ptr<Engine>> OpenLevelDB(const std::filesystem::path& dir,
                                            const EngineOptions& options) {
  // LevelDB makes its own 8 MiB cache if given none; a zero-sized one caches nothing.
  std::unique_ptr<leveldb::Cache> cache(
      leveldb::NewLRUCache(static_cast<std::size_t>(options.cache_bytes)));
  std::unique_ptr<const leveldb::FilterPolicy> filter;
  if (options.bloom_bits_per_key > 0) {
    filter.reset(leveldb::NewBloomFilterPolicy(options.bloom_bits_per_key));
  }

  leveldb::Options db_options;
  db_options.create_if_missing = true;
  db_options.error_if_exists = true;
  db_options.compression = leveldb::kNoCompression;
  db_options.write_buffer_size = static_cast<std::size_t>(options.memtable_bytes);
  db_options.block_cache = cache.get();
  db_options.filter_policy = filter.get();

  // LevelDB's Windows build takes file names in the ANSI code page, which is
  // what path::string() gives.
  leveldb::DB* raw = nullptr;
  FORGEDB_RETURN_IF_ERROR(FromLevelDB(leveldb::DB::Open(db_options, dir.string(), &raw)));
  std::unique_ptr<leveldb::DB> db(raw);
  return std::unique_ptr<Engine>(std::make_unique<LevelDBEngine>(
      std::move(cache), std::move(filter), std::move(db), dir, options.sync));
}

}  // namespace forgedb::bench
