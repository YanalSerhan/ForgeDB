// Options for opening a database, reading and writing.
//
// Each struct starts empty and gains fields only when the engine can honour
// them, so nothing here is a setting that silently does nothing:
//   Options:       buffer sizes, block cache (Phases 2-3)
//   ReadOptions:   `snapshot`, read as of an earlier point in time (Phase 4)
#pragma once

namespace forgedb {

// How to open a database. Phase 1 has no settings yet.
struct Options {};

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

// Per-read settings (Get and NewIterator). Phase 1 has no settings yet.
struct ReadOptions {};

}  // namespace forgedb
