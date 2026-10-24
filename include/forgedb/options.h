// Options for opening a database, reading and writing.
//
// Each struct starts empty and gains fields only when the engine can honour
// them, so nothing here is a setting that silently does nothing:
//   Options:       file system, buffer sizes, block cache (Phases 2-3)
//   WriteOptions:  `sync`, flush the write-ahead log before returning (Phase 2)
//   ReadOptions:   `snapshot`, read as of an earlier point in time (Phase 4)
#pragma once

namespace forgedb {

// How to open a database. Phase 1 has no settings yet.
struct Options {};

// Per-write settings. Phase 1 has no settings yet.
struct WriteOptions {};

// Per-read settings (Get and NewIterator). Phase 1 has no settings yet.
struct ReadOptions {};

}  // namespace forgedb
