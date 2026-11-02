#include "tools/bench/sqlite_engine.h"

#include <sqlite3.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include "forgedb/iterator.h"
#include "forgedb/result.h"
#include "forgedb/status.h"

#include "tools/bench/engine.h"
#include "tools/tool_util.h"
#include "util/mutex.h"
#include "util/thread_annotations.h"

namespace forgedb::bench {
namespace {

constexpr std::uint64_t kPageBytes = 4096;
constexpr int kBusyTimeoutMs = 60000;

struct CloseDatabase {
  void operator()(sqlite3* db) const noexcept { sqlite3_close(db); }
};
struct FinalizeStatement {
  void operator()(sqlite3_stmt* statement) const noexcept { sqlite3_finalize(statement); }
};
using Database = std::unique_ptr<sqlite3, CloseDatabase>;
using Statement = std::unique_ptr<sqlite3_stmt, FinalizeStatement>;

Status Error(sqlite3* db, std::string_view what) {
  return Status::IOError("sqlite: " + std::string(what),
                         db == nullptr ? "out of memory" : sqlite3_errmsg(db));
}

// Resets a statement when it goes out of scope, ending its read or write.
class ResetOnExit {
 public:
  explicit ResetOnExit(sqlite3_stmt* statement) : statement_(statement) {}
  ~ResetOnExit() { sqlite3_reset(statement_); }
  ResetOnExit(const ResetOnExit&) = delete;
  ResetOnExit& operator=(const ResetOnExit&) = delete;
  ResetOnExit(ResetOnExit&&) = delete;
  ResetOnExit& operator=(ResetOnExit&&) = delete;

 private:
  sqlite3_stmt* statement_;
};

Status Exec(sqlite3* db, const std::string& sql) {
  return sqlite3_exec(db, sql.c_str(), nullptr, nullptr, nullptr) == SQLITE_OK ? Status::OK()
                                                                               : Error(db, sql);
}

Result<Statement> Prepare(sqlite3* db, std::string_view sql) {
  sqlite3_stmt* statement = nullptr;
  if (sqlite3_prepare_v3(db, sql.data(), static_cast<int>(sql.size()), SQLITE_PREPARE_PERSISTENT,
                         &statement, nullptr) != SQLITE_OK) {
    return Error(db, sql);
  }
  return Statement(statement);
}

// Binds `bytes` as a BLOB (an empty one too: a null pointer would bind NULL).
Status Bind(sqlite3* db, sqlite3_stmt* statement, int index, std::string_view bytes) {
  const int result = bytes.empty()
                         ? sqlite3_bind_zeroblob(statement, index, 0)
                         : sqlite3_bind_blob(statement, index, bytes.data(),
                                             static_cast<int>(bytes.size()), SQLITE_STATIC);
  return result == SQLITE_OK ? Status::OK() : Error(db, "binding a parameter");
}

// A BLOB column of the current row; valid until the statement moves.
std::string_view Column(sqlite3_stmt* statement, int column) {
  const void* data = sqlite3_column_blob(statement, column);  // before _bytes, as SQLite asks
  const int size = sqlite3_column_bytes(statement, column);
  return size == 0
             ? std::string_view()
             : std::string_view(static_cast<const char*>(data), static_cast<std::size_t>(size));
}

// What every connection is opened with.
struct Settings {
  std::string file;  // UTF-8
  bool sync = false;
  std::uint64_t cache_kib = 0;
  std::uint64_t checkpoint_pages = 0;
};

struct Connection {
  // Declared first, so the statements are finalized before it closes.
  Database db;
  Statement get;   // ?1 = key
  Statement put;   // ?1 = key, ?2 = value
  Statement scan;  // ?1 = first key
};

// A connection with the settings applied but no statements yet.
Result<std::unique_ptr<Connection>> OpenConnection(const Settings& settings) {
  sqlite3* raw = nullptr;
  const int opened =
      sqlite3_open_v2(settings.file.c_str(), &raw,
                      SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_NOMUTEX, nullptr);
  auto connection = std::make_unique<Connection>();
  connection->db.reset(raw);  // closed even if opening failed, as SQLite asks
  if (opened != SQLITE_OK) {
    return Error(raw, "opening " + settings.file);
  }
  sqlite3_busy_timeout(raw, kBusyTimeoutMs);
  FORGEDB_RETURN_IF_ERROR(
      Exec(raw, "PRAGMA synchronous = " + std::string(settings.sync ? "FULL" : "NORMAL") +
                    "; PRAGMA cache_size = -" + std::to_string(settings.cache_kib) +
                    "; PRAGMA wal_autocheckpoint = " + std::to_string(settings.checkpoint_pages)));
  return connection;
}

Status PrepareStatements(Connection& connection) {
  sqlite3* const db = connection.db.get();
  FORGEDB_ASSIGN_OR_RETURN(connection.get, Prepare(db, "SELECT v FROM kv WHERE k = ?1"));
  FORGEDB_ASSIGN_OR_RETURN(
      connection.put,
      Prepare(db, "INSERT INTO kv (k, v) VALUES (?1, ?2) ON CONFLICT (k) DO UPDATE SET v = ?2"));
  FORGEDB_ASSIGN_OR_RETURN(connection.scan,
                           Prepare(db, "SELECT k, v FROM kv WHERE k >= ?1 ORDER BY k"));
  return Status::OK();
}

Result<std::string> QueryText(sqlite3* db, std::string_view sql) {
  FORGEDB_ASSIGN_OR_RETURN(const Statement statement, Prepare(db, sql));
  if (sqlite3_step(statement.get()) != SQLITE_ROW) {
    return Error(db, sql);
  }
  return std::string(Column(statement.get(), 0));
}

class SQLiteEngine;

// A connection taken from the engine's pool, returned when this is destroyed.
class Lease {
 public:
  Lease(SQLiteEngine* engine, Connection* connection) : engine_(engine), connection_(connection) {}
  ~Lease();
  Lease(Lease&& other) noexcept
      : engine_(other.engine_), connection_(std::exchange(other.connection_, nullptr)) {}
  Lease& operator=(Lease&&) = delete;
  Lease(const Lease&) = delete;
  Lease& operator=(const Lease&) = delete;

  Connection* operator->() const { return connection_; }
  [[nodiscard]] sqlite3* Handle() const { return connection_->db.get(); }

 private:
  SQLiteEngine* engine_;
  Connection* connection_;
};

// An iterator that could not get a connection.
class FailedIterator final : public Iterator {
 public:
  explicit FailedIterator(Status status) : status_(std::move(status)) {}
  [[nodiscard]] bool Valid() const override { return false; }
  void SeekToFirst() override {}
  void Seek(std::string_view /*target*/) override {}
  void Next() override {}
  [[nodiscard]] std::string_view key() const override { return {}; }
  [[nodiscard]] std::string_view value() const override { return {}; }
  [[nodiscard]] Status status() const override { return status_; }

 private:
  Status status_;
};

class SQLiteIterator final : public Iterator {
 public:
  explicit SQLiteIterator(Lease lease) : lease_(std::move(lease)) {}
  ~SQLiteIterator() override { sqlite3_reset(Scan()); }
  SQLiteIterator(const SQLiteIterator&) = delete;
  SQLiteIterator& operator=(const SQLiteIterator&) = delete;
  SQLiteIterator(SQLiteIterator&&) = delete;
  SQLiteIterator& operator=(SQLiteIterator&&) = delete;

  [[nodiscard]] bool Valid() const override { return valid_; }
  // Every key is at least the empty BLOB.
  void SeekToFirst() override { Seek({}); }
  void Seek(std::string_view target) override {
    sqlite3_reset(Scan());
    valid_ = false;
    status_ = Bind(lease_.Handle(), Scan(), 1, target);
    if (status_.ok()) {
      Step();
    }
  }
  void Next() override { Step(); }
  [[nodiscard]] std::string_view key() const override { return Column(Scan(), 0); }
  [[nodiscard]] std::string_view value() const override { return Column(Scan(), 1); }
  [[nodiscard]] Status status() const override { return status_; }

 private:
  [[nodiscard]] sqlite3_stmt* Scan() const { return lease_->scan.get(); }

  void Step() {
    const int result = sqlite3_step(Scan());
    valid_ = result == SQLITE_ROW;
    if (!valid_ && result != SQLITE_DONE) {
      status_ = Error(lease_.Handle(), "scanning");
    }
  }

  Lease lease_;
  bool valid_ = false;
  Status status_;
};

class SQLiteEngine final : public Engine {
 public:
  SQLiteEngine(Settings settings, std::unique_ptr<Connection> first, std::filesystem::path dir)
      : settings_(std::move(settings)), dir_(std::move(dir)) {
    idle_.push_back(first.get());
    all_.push_back(std::move(first));
  }

  Status Put(std::string_view key, std::string_view value) override {
    const MutexLock writing(&write_mutex_);
    FORGEDB_ASSIGN_OR_RETURN(const Lease lease, Acquire());
    sqlite3_stmt* const put = lease->put.get();
    const ResetOnExit reset(put);
    FORGEDB_RETURN_IF_ERROR(Bind(lease.Handle(), put, 1, key));
    FORGEDB_RETURN_IF_ERROR(Bind(lease.Handle(), put, 2, value));
    return sqlite3_step(put) == SQLITE_DONE ? Status::OK() : Error(lease.Handle(), "writing");
  }

  Result<bool> Get(std::string_view key, std::string& value) override {
    FORGEDB_ASSIGN_OR_RETURN(const Lease lease, Acquire());
    sqlite3_stmt* const get = lease->get.get();
    const ResetOnExit reset(get);
    FORGEDB_RETURN_IF_ERROR(Bind(lease.Handle(), get, 1, key));
    const int result = sqlite3_step(get);
    if (result == SQLITE_ROW) {
      value.assign(Column(get, 0));
      return true;
    }
    if (result == SQLITE_DONE) {
      return false;
    }
    return Error(lease.Handle(), "reading");
  }

  std::unique_ptr<Iterator> NewIterator() override {
    Result<Lease> lease = Acquire();
    if (!lease.ok()) {
      return std::make_unique<FailedIterator>(lease.status());
    }
    return std::make_unique<SQLiteIterator>(std::move(lease).value());
  }

  Status WaitForBackgroundWork() override {
    const MutexLock writing(&write_mutex_);
    FORGEDB_ASSIGN_OR_RETURN(const Lease lease, Acquire());
    return sqlite3_wal_checkpoint_v2(lease.Handle(), nullptr, SQLITE_CHECKPOINT_TRUNCATE, nullptr,
                                     nullptr) == SQLITE_OK
               ? Status::OK()
               : Error(lease.Handle(), "checkpointing");
  }

  Result<std::uint64_t> DiskBytes() override { return FolderBytes(dir_); }

  std::map<std::string, double> Counters() override {
    constexpr std::array<std::pair<int, std::string_view>, 3> kCounters = {{
        {SQLITE_DBSTATUS_CACHE_HIT, "cache_hits"},
        {SQLITE_DBSTATUS_CACHE_MISS, "cache_misses"},
        {SQLITE_DBSTATUS_CACHE_WRITE, "cache_writes"},
    }};
    std::map<std::string, double> counters;
    const MutexLock lock(&mutex_);
    for (const std::unique_ptr<Connection>& connection : all_) {
      for (const auto& [op, name] : kCounters) {
        int current = 0;
        int highest = 0;
        if (sqlite3_db_status(connection->db.get(), op, &current, &highest, 0) == SQLITE_OK) {
          counters[std::string(name)] += current;
        }
      }
    }
    counters["connections"] = static_cast<double>(all_.size());
    return counters;
  }

  void Release(Connection* connection) {
    const MutexLock lock(&mutex_);
    idle_.push_back(connection);
  }

 private:
  Result<Lease> Acquire() {
    {
      const MutexLock lock(&mutex_);
      if (!idle_.empty()) {
        Connection* const connection = idle_.back();
        idle_.pop_back();
        return Lease(this, connection);
      }
    }
    // A new connection, opened outside the lock (it reads the file).
    FORGEDB_ASSIGN_OR_RETURN(std::unique_ptr<Connection> connection, OpenConnection(settings_));
    FORGEDB_RETURN_IF_ERROR(PrepareStatements(*connection));
    const MutexLock lock(&mutex_);
    all_.push_back(std::move(connection));
    return Lease(this, all_.back().get());
  }

  const Settings settings_;
  const std::filesystem::path dir_;
  Mutex write_mutex_;  // held by each write and checkpoint: one writer at a time
  Mutex mutex_;
  std::vector<std::unique_ptr<Connection>> all_ FORGEDB_GUARDED_BY(mutex_);
  std::vector<Connection*> idle_ FORGEDB_GUARDED_BY(mutex_);  // in all_, not leased
};

Lease::~Lease() {
  if (connection_ != nullptr) {
    engine_->Release(connection_);
  }
}

Status FileError(std::string_view what, const std::filesystem::path& path,
                 const std::error_code& error) {
  return Status::IOError(std::string(what) + " " + path.generic_string(), error.message());
}

}  // namespace

Result<std::unique_ptr<Engine>> OpenSQLite(const std::filesystem::path& dir,
                                           const EngineOptions& options) {
  if (sqlite3_threadsafe() == 0) {
    return Status::NotSupported("this SQLite was built without thread support");
  }
  std::error_code error;
  std::filesystem::create_directories(dir, error);
  if (error) {
    return FileError("creating", dir, error);
  }
  const std::filesystem::path file = dir / "kv.sqlite";
  if (std::filesystem::exists(file, error) || error) {
    return error ? FileError("checking", file, error)
                 : Status::InvalidArgument("database exists", file.generic_string());
  }
  Settings settings{
      .file = tools::PathToUtf8(file),
      .sync = options.sync,
      .cache_kib = options.cache_bytes / 1024,
      .checkpoint_pages = std::max<std::uint64_t>(1, options.memtable_bytes / kPageBytes),
  };
  FORGEDB_ASSIGN_OR_RETURN(std::unique_ptr<Connection> first, OpenConnection(settings));
  sqlite3* const db = first->db.get();
  FORGEDB_RETURN_IF_ERROR(
      Exec(db, "PRAGMA page_size = " + std::to_string(kPageBytes) + "; PRAGMA journal_mode = WAL"));
  FORGEDB_ASSIGN_OR_RETURN(const std::string mode, QueryText(db, "PRAGMA journal_mode"));
  if (mode != "wal") {
    return Status::NotSupported("SQLite did not switch to WAL mode", mode);
  }
  FORGEDB_RETURN_IF_ERROR(Exec(db, "CREATE TABLE kv (k BLOB PRIMARY KEY, v BLOB) WITHOUT ROWID"));
  FORGEDB_RETURN_IF_ERROR(PrepareStatements(*first));
  return std::unique_ptr<Engine>(
      std::make_unique<SQLiteEngine>(std::move(settings), std::move(first), dir));
}

}  // namespace forgedb::bench
