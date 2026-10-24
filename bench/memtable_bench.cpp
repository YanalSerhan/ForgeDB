// Memtable throughput with realistic entries: 16-byte keys and 100-byte
// values (the shape of db_bench's default workload). Adds/second building a
// memtable from scratch, and Gets/second (hits, random order) on a full one.
// Compared with skip_list_bench, the difference is the cost of the entry
// encoding and of comparing encoded internal keys instead of integers.
//
//   out\build\msvc-release\bench\forgedb_microbench.exe --benchmark_filter=MemTable
#include <algorithm>

#include <benchmark/benchmark.h>

#include <cstddef>
#include <cstdint>
#include <random>
#include <string>
#include <vector>

#include "db/internal_key.h"
#include "memtable/memtable.h"

namespace forgedb {
namespace {

constexpr std::size_t kValueSize = 100;

// `count` distinct 16-byte keys ("key-" + 12 digits) in random order.
std::vector<std::string> RandomKeys(std::size_t count, std::uint64_t seed) {
  std::vector<std::string> keys;
  keys.reserve(count);
  for (std::size_t i = 0; i < count; ++i) {
    const std::string digits = std::to_string(i);  // at most 7 digits here
    keys.push_back("key-" + std::string(12 - digits.size(), '0') + digits);
  }
  std::shuffle(keys.begin(), keys.end(), std::mt19937_64(seed));
  return keys;
}

void BM_MemTableAdd(benchmark::State& state) {
  const std::vector<std::string> keys = RandomKeys(static_cast<std::size_t>(state.range(0)), 1);
  const std::string value(kValueSize, 'v');
  for (auto _ : state) {
    const MemTableRef table = MemTable::Create();
    SequenceNumber sequence = 0;
    for (const std::string& key : keys) {
      table->Add(++sequence, ValueType::kValue, key, value);
    }
    benchmark::ClobberMemory();
  }
  state.SetItemsProcessed(static_cast<std::int64_t>(state.iterations()) * state.range(0));
}

void BM_MemTableGet(benchmark::State& state) {
  const std::vector<std::string> keys = RandomKeys(static_cast<std::size_t>(state.range(0)), 1);
  const std::string value(kValueSize, 'v');
  const MemTableRef table = MemTable::Create();
  SequenceNumber sequence = 0;
  for (const std::string& key : keys) {
    table->Add(++sequence, ValueType::kValue, key, value);
  }
  const std::vector<std::string> probes = RandomKeys(keys.size(), 2);  // different order
  std::string result;
  std::size_t i = 0;
  for (auto _ : state) {
    const LookupKey lookup(probes[i], sequence);
    benchmark::DoNotOptimize(table->Get(lookup, &result));
    if (++i == probes.size()) {
      i = 0;
    }
  }
  state.SetItemsProcessed(static_cast<std::int64_t>(state.iterations()));
}

BENCHMARK(BM_MemTableAdd)->Arg(100'000)->Arg(1'000'000)->Unit(benchmark::kMillisecond);
BENCHMARK(BM_MemTableGet)->Arg(100'000)->Arg(1'000'000);

}  // namespace
}  // namespace forgedb
