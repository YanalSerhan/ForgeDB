// forgedb_bench (TODO 5.1): runs the workloads of bench_options.h on each
// engine and reports, per run:
//   - operations per second and MB/s (keys and values written or read);
//   - latency p50, p99 and max, per operation (not for readseq, whose
//     operations are single iterator steps);
//   - write amplification, for workloads that write: bytes this process
//     wrote to files from the start of the workload until the engine's
//     background work settled, divided by the keys and values written
//     (process I/O counters, src/env/process_io.h; the log is included, so
//     it is higher than DBStats::WriteAmplification, which counts tables);
//   - space amplification: the database's files after settling, divided by
//     the live keys and values (--num of each);
//   - each engine's own counters (for ForgeDB: DBStats), as growth during the
//     workload.
// The measured time covers the operations only; waiting for compactions to
// settle afterwards is reported separately.
//
// Reads also check their answers: every existing key is found, every missing
// key is not, scans see every key once and in order. A wrong answer fails
// the run (exit code 1).
#pragma once

#include <cstdint>
#include <filesystem>
#include <map>
#include <optional>
#include <ostream>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "forgedb/result.h"

#include "tools/bench/bench_options.h"
#include "tools/bench/engine.h"
#include "tools/bench/histogram.h"

namespace forgedb::bench {

struct RunResult {
  std::string engine;
  std::string workload;
  std::uint64_t run = 0;  // 1-based
  std::uint64_t ops = 0;
  double seconds = 0;       // the operations, all threads, wall clock
  std::uint64_t bytes = 0;  // keys and values written or read
  bool has_latency = false;
  LatencyHistogram latency;  // nanoseconds
  std::optional<double> write_amplification;
  std::optional<double> space_amplification;
  double settle_seconds = 0;
  std::map<std::string, double> counters;  // growth during the workload
};

// One run of `workload` on a new database of `engine` in root/<engine>-<workload>-<run>,
// removed afterwards.
[[nodiscard]] Result<RunResult> RunWorkload(EngineFactory& factory, std::string_view engine,
                                            std::string_view workload, std::uint64_t run,
                                            const BenchOptions& options,
                                            const std::filesystem::path& root);

// The results as JSON: the options, the build, and every run.
[[nodiscard]] std::string ResultsJson(const BenchOptions& options,
                                      std::span<const RunResult> results);

// One line about a run, for people.
[[nodiscard]] std::string FormatRun(const RunResult& result);

// The whole tool. Exit code 0: every run passed; 1: a run failed; 2: usage.
[[nodiscard]] int RunBench(std::span<const std::string> args, EngineFactory& factory,
                           std::ostream& out, std::ostream& err);

}  // namespace forgedb::bench
