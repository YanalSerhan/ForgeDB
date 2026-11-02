#include "tools/bench/forgedb_engine.h"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "forgedb/db.h"
#include "forgedb/iterator.h"
#include "forgedb/options.h"
#include "forgedb/result.h"
#include "forgedb/stats.h"
#include "forgedb/status.h"

#include "db/db_impl.h"
#include "env/file_system.h"
#include "env/win_file_system.h"
#include "tools/bench/engine.h"

namespace forgedb::bench {

Engine::~Engine() = default;
EngineFactory::~EngineFactory() = default;

namespace {

class ForgeDBEngine final : public Engine {
 public:
  ForgeDBEngine(std::unique_ptr<DB> db, std::shared_ptr<FileSystem> fs, std::filesystem::path dir,
                bool sync)
      : db_(std::move(db)), fs_(std::move(fs)), dir_(std::move(dir)), write_{.sync = sync} {}

  Status Put(std::string_view key, std::string_view value) override {
    return db_->Put(write_, key, value);
  }

  Result<bool> Get(std::string_view key, std::string& value) override {
    Result<std::string> found = db_->Get({}, key);
    if (found.ok()) {
      value = std::move(found).value();
      return true;
    }
    if (found.status().code() == Status::Code::kNotFound) {
      return false;
    }
    return found.status();
  }

  std::unique_ptr<Iterator> NewIterator() override { return db_->NewIterator({}); }

  Status WaitForBackgroundWork() override {
    dynamic_cast<DBImpl&>(*db_).WaitForBackgroundWorkForTesting();
    return Status::OK();
  }

  Result<std::uint64_t> DiskBytes() override {
    FORGEDB_ASSIGN_OR_RETURN(const std::vector<std::string> children, fs_->GetChildren(dir_));
    std::uint64_t bytes = 0;
    for (const std::string& child : children) {
      FORGEDB_ASSIGN_OR_RETURN(const std::uint64_t size, fs_->GetFileSize(dir_ / child));
      bytes += size;
    }
    return bytes;
  }

  std::map<std::string, double> Counters() override {
    const DBStats stats = db_->GetStats();
    std::uint64_t compactions = 0;
    for (const LevelStats& level : stats.levels) {
      compactions += level.compactions.count + level.compactions.trivial_moves;
    }
    const auto seconds = [](std::uint64_t microseconds) {
      return static_cast<double>(microseconds) / 1e6;
    };
    return {
        {"user_bytes", static_cast<double>(stats.user_bytes)},
        {"log_bytes", static_cast<double>(stats.log_bytes)},
        {"table_bytes_written", static_cast<double>(stats.TableBytesWritten())},
        {"flushes", static_cast<double>(stats.flushes)},
        {"compactions", static_cast<double>(compactions)},
        {"stall_seconds",
         seconds(stats.memtable_full.microseconds + stats.level0_slowdown.microseconds +
                 stats.level0_stop.microseconds)},
    };
  }

 private:
  std::unique_ptr<DB> db_;
  std::shared_ptr<FileSystem> fs_;
  std::filesystem::path dir_;
  WriteOptions write_;
};

class ForgeDBFactory final : public EngineFactory {
 public:
  explicit ForgeDBFactory(std::shared_ptr<FileSystem> fs) : fs_(std::move(fs)) {}

  [[nodiscard]] std::vector<std::string> Engines() const override { return {"forgedb"}; }

  Result<std::unique_ptr<Engine>> Open(std::string_view engine, const std::filesystem::path& dir,
                                       const EngineOptions& options) override {
    if (engine != "forgedb") {
      return Status::InvalidArgument("unknown engine", engine);
    }
    return OpenForgeDB(fs_, dir, options);
  }

  Status Remove(const std::filesystem::path& dir) override {
    Result<std::vector<std::string>> children = fs_->GetChildren(dir);
    if (!children.ok()) {
      return Status::OK();  // nothing there
    }
    for (const std::string& child : *children) {
      FORGEDB_RETURN_IF_ERROR(fs_->RemoveFile(dir / child));
    }
    return Status::OK();
  }

  Status CreateRoot(const std::filesystem::path& dir) override { return fs_->CreateDir(dir); }

  [[nodiscard]] bool RealFiles() const override { return false; }

 private:
  std::shared_ptr<FileSystem> fs_;
};

}  // namespace

Result<std::unique_ptr<Engine>> OpenForgeDB(std::shared_ptr<FileSystem> fs,
                                            const std::filesystem::path& dir,
                                            const EngineOptions& options) {
  Options db_options;
  db_options.block_cache_bytes = static_cast<std::size_t>(options.cache_bytes);
  db_options.memtable_bytes = static_cast<std::size_t>(options.memtable_bytes);
  db_options.filter_bits_per_key = options.bloom_bits_per_key;
  FORGEDB_ASSIGN_OR_RETURN(std::unique_ptr<DB> db, OpenDB(db_options, dir, fs));
  return std::unique_ptr<Engine>(
      std::make_unique<ForgeDBEngine>(std::move(db), std::move(fs), dir, options.sync));
}

Result<std::unique_ptr<Engine>> OpenForgeDBOnDisk(const std::filesystem::path& dir,
                                                  const EngineOptions& options) {
  return OpenForgeDB(std::make_shared<WinFileSystem>(), dir, options);
}

std::unique_ptr<EngineFactory> NewForgeDBFactory(std::shared_ptr<FileSystem> fs) {
  return std::make_unique<ForgeDBFactory>(std::move(fs));
}

}  // namespace forgedb::bench
