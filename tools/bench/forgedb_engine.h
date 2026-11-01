// The ForgeDB adapter for forgedb_bench (TODO 5.1).
//
// EngineOptions map to ForgeDB's Options as: cache_bytes -> block_cache_bytes,
// bloom_bits_per_key -> filter_bits_per_key, sync -> WriteOptions::sync on
// every write. Everything else is ForgeDB's default (forgedb/options.h).
// WaitForBackgroundWork waits until no flush is queued and no level scores 1
// or more (DBImpl::WaitForBackgroundWorkForTesting).
#pragma once

#include <filesystem>
#include <memory>

#include "forgedb/result.h"

#include "env/file_system.h"
#include "tools/bench/engine.h"

namespace forgedb::bench {

// ForgeDB in folder `dir` (created if needed) on `fs`.
[[nodiscard]] Result<std::unique_ptr<Engine>> OpenForgeDB(std::shared_ptr<FileSystem> fs,
                                                          const std::filesystem::path& dir,
                                                          const EngineOptions& options);

// A factory with ForgeDB only, on `fs`; `real_files` says whether `fs`
// writes real files (EngineFactory::RealFiles). Tests use MemFileSystem.
[[nodiscard]] std::unique_ptr<EngineFactory> NewForgeDBFactory(std::shared_ptr<FileSystem> fs,
                                                               bool real_files);

}  // namespace forgedb::bench
