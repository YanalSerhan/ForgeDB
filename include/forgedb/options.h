// Options for opening a database, reading and writing.
//
// Each struct starts empty and gains fields only when the engine can honour
// them, so nothing here is a setting that silently does nothing:
//   Options:       buffer sizes, caches, table settings (Phase 3)
//   ReadOptions:   `snapshot`, read as of an earlier point in time (Phase 4.1)
#pragma once

#include <cstddef>
#include <memory>

namespace forgedb {

class Snapshot;

// How to open a database. DB::Open checks every field (InvalidArgument if one
// is out of range).
struct Options {
  // When the memtable's memory reaches this, its contents are written to a
  // new table file and a new memtable and log start. Larger: fewer, larger
  // tables and longer recovery replays. At least 1; values below a few KiB
  // flush after nearly every write, which only tests want.
  std::size_t memtable_bytes = std::size_t{4} << 20;

  // Memory for recently read table blocks, shared by all tables; 0 disables
  // the block cache.
  std::size_t block_cache_bytes = std::size_t{8} << 20;

  // Table files kept open (each holds its index and bloom filter in memory).
  // At least 1.
  std::size_t max_open_tables = 1000;

  // Bloom filter bits per key in new tables (about 0.8% false positives at
  // 10); 0 writes tables without filters. At most 30.
  int filter_bits_per_key = 10;

  // Target size of the data blocks in new tables, in bytes. 256 B to 1 MiB.
  std::size_t block_size = 4096;
};

// Per-write settings.
struct WriteOptions {
  // If true, the write is forced to stable storage (the write-ahead log is
  // synced) before Write returns, so it survives an OS crash or power loss
  // (on a drive that honors flush requests; see docs/adr/0002). If false, it
  // survives a crash of this process but may be lost on power loss.
  // Concurrent writes share one sync (group commit), so a sync costs each
  // writer less when many are writing.
  bool sync = false;
};

// Per-read settings (Get and NewIterator).
struct ReadOptions {
  // Read the database as it was when this snapshot was taken (see
  // forgedb/snapshot.h); null reads the latest data. It must come from the
  // same DB (InvalidArgument otherwise).
  std::shared_ptr<const Snapshot> snapshot;
};

}  // namespace forgedb
