// Skip list throughput at 100k and 1M entries: inserts/second (building a
// list from scratch in random key order) and lookups/second (Contains on a
// prebuilt list, hits in random order). Google Benchmark reports items/second.
//
//   out\build\msvc-release\bench\forgedb_microbench.exe --benchmark_filter=SkipList
//
// Keys are 64-bit integers, so these numbers measure the structure itself.
// The memtable's keys are pointers to encoded entries, which adds a pointer
// chase and a memcmp per comparison; the memtable gets its own benchmark.
#include <algorithm>

#include <benchmark/benchmark.h>

#include <compare>
#include <cstddef>
#include <cstdint>
#include <random>
#include <vector>

#include "memtable/arena.h"
#include "memtable/skip_list.h"

namespace forgedb {
namespace {

using Key = std::uint64_t;

struct Ascending {
  std::strong_ordering operator()(Key a, Key b) const noexcept { return a <=> b; }
};

using List = SkipList<Key, Ascending>;

// `count` distinct keys in random order (fixed seed: identical between runs).
std::vector<Key> RandomDistinctKeys(std::size_t count, std::uint64_t seed) {
  std::vector<Key> keys(count);
  for (std::size_t i = 0; i < count; ++i) {
    keys[i] = static_cast<Key>(i) * 2654435761U;  // distinct, spread out
  }
  std::shuffle(keys.begin(), keys.end(), std::mt19937_64(seed));
  return keys;
}

void BM_SkipListInsert(benchmark::State& state) {
  const auto count = static_cast<std::size_t>(state.range(0));
  const std::vector<Key> keys = RandomDistinctKeys(count, 1);
  for (auto _ : state) {
    // Each iteration builds a full list; creating and destroying the arena is
    // part of the cost, as it is for a real memtable.
    Arena arena;
    List list(Ascending{}, &arena);
    for (const Key key : keys) {
      list.Insert(key);
    }
    benchmark::ClobberMemory();
  }
  state.SetItemsProcessed(static_cast<std::int64_t>(state.iterations()) * state.range(0));
}

void BM_SkipListLookup(benchmark::State& state) {
  const auto count = static_cast<std::size_t>(state.range(0));
  const std::vector<Key> keys = RandomDistinctKeys(count, 1);
  Arena arena;
  List list(Ascending{}, &arena);
  for (const Key key : keys) {
    list.Insert(key);
  }
  // Probe in a different random order than insertion.
  const std::vector<Key> probes = RandomDistinctKeys(count, 2);
  std::size_t i = 0;
  for (auto _ : state) {
    benchmark::DoNotOptimize(list.Contains(probes[i]));
    if (++i == probes.size()) {
      i = 0;
    }
  }
  state.SetItemsProcessed(static_cast<std::int64_t>(state.iterations()));
}

BENCHMARK(BM_SkipListInsert)->Arg(100'000)->Arg(1'000'000)->Unit(benchmark::kMillisecond);
BENCHMARK(BM_SkipListLookup)->Arg(100'000)->Arg(1'000'000);

}  // namespace
}  // namespace forgedb
