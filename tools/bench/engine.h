// Engine: what forgedb_bench needs from a key-value store (TODO 5.1). Every
// workload runs through this interface, so each engine runs the same code;
// an adapter per engine translates (ForgeDB here; RocksDB, LevelDB and
// SQLite in 5.1b).
//
// Engines are opened by an EngineFactory, which also says where their files
// are and how to remove them, so tests can run everything on an in-memory
// file system.
#pragma once

#include <cstdint>
#include <filesystem>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "forgedb/iterator.h"
#include "forgedb/result.h"
#include "forgedb/status.h"

namespace forgedb::bench {

// The settings every engine gets (each maps them to its own).
struct EngineOptions {
  bool sync = false;              // every write is synced before it returns
  std::uint64_t cache_bytes = 0;  // block cache
  int bloom_bits_per_key = 10;
};

class Engine {
 public:
  Engine() = default;
  virtual ~Engine();
  Engine(const Engine&) = delete;
  Engine& operator=(const Engine&) = delete;
  Engine(Engine&&) = delete;
  Engine& operator=(Engine&&) = delete;

  [[nodiscard]] virtual Status Put(std::string_view key, std::string_view value) = 0;
  // True and the value if `key` has one; false if not.
  [[nodiscard]] virtual Result<bool> Get(std::string_view key, std::string& value) = 0;
  // Over the whole database, in key order.
  [[nodiscard]] virtual std::unique_ptr<Iterator> NewIterator() = 0;
  // Waits until the engine has no flush or compaction it wants to run.
  [[nodiscard]] virtual Status WaitForBackgroundWork() = 0;
  // Bytes of the database's files now.
  [[nodiscard]] virtual Result<std::uint64_t> DiskBytes() = 0;
  // The engine's own counters, for the report (e.g. ForgeDB's DBStats);
  // the benchmark reports how much each grew during a workload.
  [[nodiscard]] virtual std::map<std::string, double> Counters() = 0;
};

class EngineFactory {
 public:
  EngineFactory() = default;
  virtual ~EngineFactory();
  EngineFactory(const EngineFactory&) = delete;
  EngineFactory& operator=(const EngineFactory&) = delete;
  EngineFactory(EngineFactory&&) = delete;
  EngineFactory& operator=(EngineFactory&&) = delete;

  [[nodiscard]] virtual std::vector<std::string> Engines() const = 0;
  // Creates the folder `dir` (its parent exists) and opens a new database there.
  [[nodiscard]] virtual Result<std::unique_ptr<Engine>> Open(std::string_view engine,
                                                             const std::filesystem::path& dir,
                                                             const EngineOptions& options) = 0;
  // Removes `dir` and everything in it.
  [[nodiscard]] virtual Status Remove(const std::filesystem::path& dir) = 0;
  // Creates the folder that will hold the databases.
  [[nodiscard]] virtual Status CreateRoot(const std::filesystem::path& dir) = 0;
  // Whether this process's I/O counters measure the engines' writes (false
  // for an in-memory file system): then write amplification is reported.
  [[nodiscard]] virtual bool RealFiles() const = 0;
};

// ForgeDB on the Windows file system, in folders on disk (forgedb_bench).
[[nodiscard]] std::unique_ptr<EngineFactory> NewDiskEngineFactory();

}  // namespace forgedb::bench
