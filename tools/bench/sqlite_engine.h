// The SQLite adapter for forgedb_bench (TODO 5.1b; builds with
// FORGEDB_BENCH_ENGINES, SQLite from vcpkg).
//
// The database is one file, kv.sqlite, with one table:
//   CREATE TABLE kv (k BLOB PRIMARY KEY, v BLOB) WITHOUT ROWID
// (a B-tree keyed by k; BLOBs compare bytewise, as the other engines' keys),
// in WAL journal mode with 4 KiB pages. Each Put is its own transaction (an
// INSERT ... ON CONFLICT DO UPDATE); Get and the iterator are SELECTs.
//
// EngineOptions map to SQLite's settings as:
//   sync               -> PRAGMA synchronous = FULL (the log is synced at every
//                         commit); off: NORMAL (synced only at checkpoints;
//                         like the other engines' unsynced writes, a commit
//                         survives a process crash but maybe not a power loss)
//   cache_bytes        -> PRAGMA cache_size, per connection
//   memtable_bytes     -> PRAGMA wal_autocheckpoint, in pages: the log is
//                         copied into the database file once it is that big
//   bloom_bits_per_key -> nothing: a B-tree has no filters
// SQLite has no compression.
//
// Threads: SQLite connections are single-threaded (opened with NOMUTEX), so
// the engine keeps a pool and each operation uses a connection no other
// thread is using; with N threads there are N connections, each with its own
// page cache. Writes are serialized (one writer at a time, as SQLite allows);
// reads run in parallel. An iterator holds a connection until it is destroyed,
// and reads the database as of its last Seek.
//
// SQLite has no background work. WaitForBackgroundWork runs the deferred work
// it does have: a checkpoint (TRUNCATE) that copies the log into the database
// file and empties the log. Counters are page-cache hits, misses and writes,
// summed over the connections, and the number of connections; call it with
// no operation running.
#pragma once

#include <filesystem>
#include <memory>

#include "forgedb/result.h"

#include "tools/bench/engine.h"

namespace forgedb::bench {

// A new SQLite database in folder `dir` (a DiskEngineOpener).
[[nodiscard]] Result<std::unique_ptr<Engine>> OpenSQLite(const std::filesystem::path& dir,
                                                         const EngineOptions& options);

}  // namespace forgedb::bench
