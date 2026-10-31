// Write amplification of a 1,000,000-key random-write workload (Phase 4 exit
// gate): 1,000,000 distinct random 16-byte keys, 100-byte values, unsynced
// writes, default Options, on the real file system. Reports write
// amplification (table bytes written by flushes and compactions / user
// bytes; DBStats::WriteAmplification) twice:
//   - wamp_at_end:  when the last write returns (compaction may be behind);
//   - wamp_settled: after the background thread has finished every flush
//                   and compaction that the levels call for.
// Also: write rate (the measured time covers the writes only), flushes,
// compactions, stall time, and a per-level line on stdout.
//
//   out\build\msvc-release\bench\forgedb_microbench.exe --benchmark_filter=FillRandom
//
// The database lives in a fresh directory under %TEMP%, deleted afterwards.
#include <benchmark/benchmark.h>

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <memory>
#include <random>
#include <string>
#include <string_view>
#include <system_error>

#include "forgedb/db.h"
#include "forgedb/options.h"
#include "forgedb/result.h"
#include "forgedb/stats.h"
#include "forgedb/status.h"

#include "db/db_impl.h"

namespace forgedb {
namespace {

constexpr std::size_t kValueSize = 100;

// 16 hex digits of a 64-bit number: random numbers give distinct keys (a
// repeat among 1,000,000 has a chance of about 3 in 100 million).
std::string HexKey(std::uint64_t n) {
  static constexpr std::string_view kDigits = "0123456789abcdef";
  std::string key(16, '0');
  for (int i = 15; i >= 0; --i) {
    key[static_cast<std::size_t>(i)] = kDigits[n & 0xFU];
    n >>= 4U;
  }
  return key;
}

std::uint64_t Compactions(const DBStats& stats) {
  std::uint64_t compactions = 0;
  for (const LevelStats& level : stats.levels) {
    compactions += level.compactions.count + level.compactions.trivial_moves;
  }
  return compactions;
}

double StallSeconds(const DBStats& stats) {
  return static_cast<double>(stats.memtable_full.microseconds + stats.level0_slowdown.microseconds +
                             stats.level0_stop.microseconds) /
         1e6;
}

void PrintLevels(const DBStats& stats) {
  for (std::size_t level = 0; level < stats.levels.size(); ++level) {
    const LevelStats& l = stats.levels[level];
    if (l.tables == 0 && l.compactions.count == 0 && l.compactions.trivial_moves == 0) {
      continue;
    }
    std::printf(  // NOLINT(*-vararg)
        "[ fill     ] level %zu: %zu tables, %.1f MB; compactions into the next: %llu (+%llu "
        "moves), read %.1f MB, wrote %.1f MB, %.1f s\n",
        level, l.tables, static_cast<double>(l.bytes) / 1e6,
        static_cast<unsigned long long>(l.compactions.count),
        static_cast<unsigned long long>(l.compactions.trivial_moves),
        static_cast<double>(l.compactions.bytes_read) / 1e6,
        static_cast<double>(l.compactions.bytes_written) / 1e6,
        static_cast<double>(l.compactions.microseconds) / 1e6);
  }
}

void BM_FillRandomWriteAmplification(benchmark::State& state) {
  const auto keys = static_cast<std::uint64_t>(state.range(0));
  std::random_device random_device;
  const std::uint64_t seed = (std::uint64_t{random_device()} << 32U) | random_device();
  std::error_code error;
  const std::filesystem::path dir =
      std::filesystem::temp_directory_path(error) / ("forgedb-fill-" + std::to_string(seed));
  std::filesystem::remove_all(dir, error);

  for (auto _ : state) {
    Result<std::unique_ptr<DB>> opened = DB::Open(Options{}, dir);
    if (!opened.ok()) {
      state.SkipWithError(opened.status().ToString());
      break;
    }
    const std::unique_ptr<DB> db = std::move(opened).value();
    std::mt19937_64 rng(seed);
    std::string value(kValueSize, 'v');

    const auto start = std::chrono::steady_clock::now();
    for (std::uint64_t i = 0; i < keys; ++i) {
      for (char& c : value) {  // values vary; their content does not matter (no compression)
        c = static_cast<char>('a' + (rng() % 26));
      }
      if (const Status status = db->Put({}, HexKey(rng()), value); !status.ok()) {
        state.SkipWithError(status.ToString());
        break;
      }
    }
    const double write_seconds =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    state.SetIterationTime(write_seconds);
    const DBStats at_end = db->GetStats();

    const auto settle_start = std::chrono::steady_clock::now();
    dynamic_cast<DBImpl&>(*db).WaitForBackgroundWorkForTesting();
    const double settle_seconds =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - settle_start).count();
    const DBStats settled = db->GetStats();

    state.counters["keys"] = static_cast<double>(keys);
    state.counters["writes_per_s"] = static_cast<double>(keys) / write_seconds;
    state.counters["user_MB"] = static_cast<double>(settled.user_bytes) / 1e6;
    state.counters["wamp_at_end"] = at_end.WriteAmplification().value_or(0);
    state.counters["wamp_settled"] = settled.WriteAmplification().value_or(0);
    state.counters["settle_s"] = settle_seconds;
    state.counters["flushes"] = static_cast<double>(settled.flushes);
    state.counters["compactions"] = static_cast<double>(Compactions(settled));
    state.counters["stall_s"] = StallSeconds(settled);
    std::printf(  // NOLINT(*-vararg)
        "[ fill     ] %llu keys: %.1f MB of keys and values, %.1f MB of log; tables written: "
        "%.1f MB by %llu flushes, %.1f MB by compactions\n",
        static_cast<unsigned long long>(keys), static_cast<double>(settled.user_bytes) / 1e6,
        static_cast<double>(settled.log_bytes) / 1e6,
        static_cast<double>(settled.flush_bytes_written) / 1e6,
        static_cast<unsigned long long>(settled.flushes),
        static_cast<double>(settled.TableBytesWritten() - settled.flush_bytes_written) / 1e6);
    PrintLevels(settled);
  }
  std::filesystem::remove_all(dir, error);
}

BENCHMARK(BM_FillRandomWriteAmplification)
    ->Arg(1000000)  // keys
    ->Iterations(1)
    ->Repetitions(3)
    ->UseManualTime()
    ->Unit(benchmark::kSecond);

}  // namespace
}  // namespace forgedb
