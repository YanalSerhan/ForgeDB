// CRC-32C throughput: table-driven vs SSE4.2, for buffer sizes from a small
// record (64 B) to a large block (1 MiB). Google Benchmark reports bytes/second.
//
//   out\build\msvc-release\bench\forgedb_microbench.exe --benchmark_filter=Crc32c
#include <benchmark/benchmark.h>

#include <cstdint>
#include <random>
#include <string>

#include "util/crc32c.h"

namespace forgedb {
namespace {

std::string RandomBytes(std::size_t size) {
  std::string data(size, '\0');
  std::mt19937 rng(1);
  for (char& c : data) {
    c = static_cast<char>(rng());
  }
  return data;
}

void BM_Crc32cPortable(benchmark::State& state) {
  const std::string data = RandomBytes(static_cast<std::size_t>(state.range(0)));
  for (auto _ : state) {
    benchmark::DoNotOptimize(crc32c::ExtendPortable(0, data.data(), data.size()));
  }
  state.SetBytesProcessed(static_cast<std::int64_t>(state.iterations()) * state.range(0));
}

void BM_Crc32cSse42(benchmark::State& state) {
  if (!crc32c::Sse42Supported()) {
    state.SkipWithError("CPU has no SSE4.2");
    return;
  }
  const std::string data = RandomBytes(static_cast<std::size_t>(state.range(0)));
  for (auto _ : state) {
    benchmark::DoNotOptimize(crc32c::ExtendSse42(0, data.data(), data.size()));
  }
  state.SetBytesProcessed(static_cast<std::int64_t>(state.iterations()) * state.range(0));
}

BENCHMARK(BM_Crc32cPortable)->RangeMultiplier(16)->Range(64, 1 << 20);
BENCHMARK(BM_Crc32cSse42)->RangeMultiplier(16)->Range(64, 1 << 20);

}  // namespace
}  // namespace forgedb
