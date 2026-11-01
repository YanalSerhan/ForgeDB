// forgedb_bench's options (TODO 5.1), parsed from "--name=value" arguments
// as in LevelDB's and RocksDB's db_bench.
//
//   --engine=forgedb          engines to run, comma-separated
//   --workloads=a,b,...       default: all of kWorkloads, in that order
//   --num=1000000             keys in the database (and operations per workload)
//   --key_size=16             bytes; keys are zero-padded decimal numbers
//   --value_size=100          bytes
//   --sync=0                  1: every write is synced before it returns
//   --threads=1               threads running each workload's operations
//   --cache_mb=8              block cache, MiB (same for every engine)
//   --runs=1                  times each workload runs, on a fresh database
//   --seed=1                  for keys and values
//   --db=<dir>                where the databases go (default: a new folder in %TEMP%)
//   --json=<file>             also write the results as JSON
#pragma once

#include <array>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "forgedb/result.h"

namespace forgedb::bench {

// Every workload, in the default order.
//   fillseq      write every key in key order, into an empty database
//   fillrandom   write every key once, in random order, into an empty database
//   overwrite    random writes to existing keys
//   readrandom   random Gets of existing keys
//   readmissing  random Gets of keys that are not there (between existing ones)
//   readseq      one full scan per thread
//   seekrandom   seek to a random existing key, then read 10 entries
//   mixed        random existing keys: 90% Gets, 10% writes
// All but the two fills first fill a database with fillrandom (not
// measured) and wait for its compactions to finish.
inline constexpr std::array<std::string_view, 8> kWorkloads = {
    "fillseq",     "fillrandom", "overwrite",  "readrandom",
    "readmissing", "readseq",    "seekrandom", "mixed"};

struct BenchOptions {
  std::vector<std::string> engines = {"forgedb"};
  std::vector<std::string> workloads{kWorkloads.begin(), kWorkloads.end()};
  std::uint64_t num = 1000000;
  std::uint64_t key_size = 16;
  std::uint64_t value_size = 100;
  bool sync = false;
  std::uint64_t threads = 1;
  std::uint64_t cache_mb = 8;
  std::uint64_t runs = 1;
  std::uint64_t seed = 1;
  std::optional<std::filesystem::path> db;
  std::optional<std::filesystem::path> json;
};

// The options, or InvalidArgument naming the problem (an unknown option, a
// value out of range, keys too short for --num keys).
[[nodiscard]] Result<BenchOptions> ParseBenchOptions(std::span<const std::string> args,
                                                     std::span<const std::string> known_engines);

// Usage text, listing `known_engines`.
[[nodiscard]] std::string BenchUsage(std::span<const std::string> known_engines);

}  // namespace forgedb::bench
