#include "util/lru_cache.h"

#include <atomic>
#include <cstddef>
#include <cstdint>

#include <gtest/gtest.h>

#include <memory>
#include <random>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace forgedb {
namespace {

// A value that counts how many instances are alive, so tests can see exactly
// when the cache frees one.
class Tracked {
 public:
  Tracked(int value, std::shared_ptr<std::atomic<int>> alive)
      : value_(value), alive_(std::move(alive)) {
    alive_->fetch_add(1);
  }
  ~Tracked() {
    if (alive_ != nullptr) {
      alive_->fetch_sub(1);
    }
  }
  Tracked(const Tracked&) = delete;
  Tracked& operator=(const Tracked&) = delete;
  Tracked(Tracked&& other) noexcept : value_(other.value_), alive_(std::move(other.alive_)) {}
  Tracked& operator=(Tracked&&) = delete;

  [[nodiscard]] int value() const { return value_; }

 private:
  int value_;
  std::shared_ptr<std::atomic<int>> alive_;
};

using Cache = ShardedLruCache<int>;

std::string Key(int i) { return "key" + std::to_string(i); }

// One shard, so LRU order is exact.
TEST(LruCacheTest, HitAndMiss) {
  Cache cache(100, 1);
  EXPECT_FALSE(cache.Lookup("a"));
  cache.Insert("a", 1, 1).Reset();
  {
    const Cache::Handle h = cache.Lookup("a");
    ASSERT_TRUE(h);
    EXPECT_EQ(*h, 1);
  }
  EXPECT_FALSE(cache.Lookup("b"));
  const CacheStats stats = cache.GetStats();
  EXPECT_EQ(stats.hits, 1U);
  EXPECT_EQ(stats.misses, 2U);
  EXPECT_EQ(stats.entries, 1U);
  EXPECT_EQ(stats.charge, 1U);
}

TEST(LruCacheTest, InsertReplaces) {
  Cache cache(100, 1);
  cache.Insert("a", 1, 10).Reset();
  cache.Insert("a", 2, 20).Reset();
  EXPECT_EQ(*cache.Lookup("a"), 2);
  EXPECT_EQ(cache.GetStats().charge, 20U);
  EXPECT_EQ(cache.GetStats().entries, 1U);
}

TEST(LruCacheTest, EvictsLeastRecentlyUsed) {
  Cache cache(3, 1);
  for (int i = 0; i < 3; ++i) {
    cache.Insert(Key(i), i, 1).Reset();
  }
  cache.Lookup(Key(0)).Reset();  // 0 is now the most recently used
  cache.Insert(Key(3), 3, 1).Reset();
  EXPECT_TRUE(cache.Lookup(Key(0)));
  EXPECT_FALSE(cache.Lookup(Key(1)));  // the least recently used went
  EXPECT_TRUE(cache.Lookup(Key(2)));
  EXPECT_TRUE(cache.Lookup(Key(3)));
  EXPECT_EQ(cache.GetStats().evictions, 1U);
}

TEST(LruCacheTest, ChargeIsInBytesNotEntries) {
  Cache cache(100, 1);
  cache.Insert("big", 1, 90).Reset();
  cache.Insert("small", 2, 5).Reset();
  cache.Insert("medium", 3, 20).Reset();  // 115 > 100: "big" is the oldest
  EXPECT_FALSE(cache.Lookup("big"));
  EXPECT_TRUE(cache.Lookup("small"));
  EXPECT_TRUE(cache.Lookup("medium"));
  EXPECT_EQ(cache.GetStats().charge, 25U);
}

TEST(LruCacheTest, EntryLargerThanCapacityIsDroppedWhenReleased) {
  Cache cache(10, 1);
  {
    const Cache::Handle h = cache.Insert("huge", 7, 50);
    EXPECT_EQ(*h, 7);  // usable while held
  }
  EXPECT_FALSE(cache.Lookup("huge"));
  EXPECT_EQ(cache.GetStats().charge, 0U);
}

TEST(LruCacheTest, EntryInUseIsNeverFreed) {
  const auto alive = std::make_shared<std::atomic<int>>(0);
  ShardedLruCache<Tracked> cache(2, 1);
  ShardedLruCache<Tracked>::Handle held = cache.Insert("held", Tracked(42, alive), 1);
  for (int i = 0; i < 100; ++i) {
    cache.Insert(Key(i), Tracked(i, alive), 1).Reset();
  }
  // "held" was in use the whole time: not evicted, still readable. Of the
  // others only the newest is left (capacity 2 = "held" + 1).
  EXPECT_EQ(held->value(), 42);
  EXPECT_EQ(alive->load(), 2);
  EXPECT_EQ(cache.GetStats().evictions, 99U);
  held.Reset();
  EXPECT_TRUE(cache.Lookup("held"));  // released, it is cached like any other entry
}

TEST(LruCacheTest, InUseEntriesMayExceedCapacityUntilReleased) {
  const auto alive = std::make_shared<std::atomic<int>>(0);
  ShardedLruCache<Tracked> cache(2, 1);
  std::vector<ShardedLruCache<Tracked>::Handle> held;
  held.reserve(3);
  for (int i = 0; i < 3; ++i) {
    held.push_back(cache.Insert(Key(i), Tracked(i, alive), 1));
  }
  EXPECT_EQ(cache.GetStats().charge, 3U);  // over capacity: all in use
  EXPECT_EQ(alive->load(), 3);
  for (ShardedLruCache<Tracked>::Handle& h : held) {
    h.Reset();  // in order 0, 1, 2: releasing 0 leaves 3 > 2, so 0 is evicted
  }
  EXPECT_EQ(cache.GetStats().charge, 2U);
  EXPECT_EQ(alive->load(), 2);
  EXPECT_FALSE(cache.Lookup(Key(0)));
}

TEST(LruCacheTest, ErasedEntryInUseIsFreedByItsLastHandle) {
  const auto alive = std::make_shared<std::atomic<int>>(0);
  ShardedLruCache<Tracked> cache(10, 1);
  ShardedLruCache<Tracked>::Handle a = cache.Insert("k", Tracked(1, alive), 1);
  ShardedLruCache<Tracked>::Handle b = cache.Lookup("k");
  cache.Erase("k");
  EXPECT_FALSE(cache.Lookup("k"));
  EXPECT_EQ(cache.GetStats().charge, 0U);
  EXPECT_EQ(alive->load(), 1);
  a.Reset();
  EXPECT_EQ(alive->load(), 1);
  EXPECT_EQ(b->value(), 1);
  b.Reset();
  EXPECT_EQ(alive->load(), 0);
}

TEST(LruCacheTest, ReplacedEntryInUseStaysValid) {
  const auto alive = std::make_shared<std::atomic<int>>(0);
  ShardedLruCache<Tracked> cache(10, 1);
  ShardedLruCache<Tracked>::Handle old_value = cache.Insert("k", Tracked(1, alive), 1);
  cache.Insert("k", Tracked(2, alive), 1).Reset();
  EXPECT_EQ(old_value->value(), 1);
  EXPECT_EQ(cache.Lookup("k")->value(), 2);
  EXPECT_EQ(alive->load(), 2);
  old_value.Reset();
  EXPECT_EQ(alive->load(), 1);
}

TEST(LruCacheTest, EraseOfAnUnusedEntryFreesIt) {
  const auto alive = std::make_shared<std::atomic<int>>(0);
  ShardedLruCache<Tracked> cache(10, 1);
  cache.Insert("k", Tracked(1, alive), 1).Reset();
  cache.Erase("k");
  cache.Erase("never there");
  EXPECT_EQ(alive->load(), 0);
}

TEST(LruCacheTest, HandlesMove) {
  Cache cache(10, 1);
  Cache::Handle a = cache.Insert("k", 5, 1);
  Cache::Handle b = std::move(a);
  EXPECT_FALSE(a);  // NOLINT(bugprone-use-after-move,clang-analyzer-cplusplus.Move)
  ASSERT_TRUE(b);
  EXPECT_EQ(*b, 5);
  a = std::move(b);
  EXPECT_EQ(*a, 5);
}

TEST(LruCacheTest, DestroyingTheCacheFreesEverything) {
  const auto alive = std::make_shared<std::atomic<int>>(0);
  {
    ShardedLruCache<Tracked> cache(1000);  // room for all 50 in every shard
    for (int i = 0; i < 50; ++i) {
      cache.Insert(Key(i), Tracked(i, alive), 1).Reset();
    }
    EXPECT_EQ(alive->load(), 50);
  }
  EXPECT_EQ(alive->load(), 0);
}

TEST(LruCacheTest, ShardsSplitTheCapacity) {
  Cache cache(1600, 16);  // 100 per shard
  for (int i = 0; i < 10000; ++i) {
    cache.Insert(Key(i), i, 1).Reset();
  }
  const CacheStats stats = cache.GetStats();
  EXPECT_LE(stats.charge, 1600U);
  EXPECT_GT(stats.charge, 1500U);  // every shard got keys, and is full
  EXPECT_EQ(stats.entries + stats.evictions, 10000U);
}

TEST(LruCacheTest, NewIdIsUnique) {
  Cache cache(1);
  const std::uint64_t a = cache.NewId();
  const std::uint64_t b = cache.NewId();
  EXPECT_NE(a, b);
  EXPECT_NE(a, 0U);
}

// Many threads insert, look up, hold and release random keys in a cache much
// smaller than the key space: every value read matches its key, and no value
// is freed while a thread holds it (ASan/TSan builds check the memory side).
TEST(LruCacheTest, ConcurrentUse) {
  constexpr int kThreads = 16;
  constexpr int kOperations = 20000;
  constexpr int kKeys = 500;
  const auto alive = std::make_shared<std::atomic<int>>(0);
  ShardedLruCache<Tracked> cache(64);
  std::atomic<int> wrong{0};

  std::vector<std::thread> threads;
  threads.reserve(kThreads);
  for (int t = 0; t < kThreads; ++t) {
    threads.emplace_back([&cache, &wrong, &alive, t] {
      std::mt19937 random(static_cast<std::uint32_t>(t + 1));
      std::vector<ShardedLruCache<Tracked>::Handle> held;
      for (int i = 0; i < kOperations; ++i) {
        const int k = static_cast<int>(random() % kKeys);
        ShardedLruCache<Tracked>::Handle h = cache.Lookup(Key(k));
        if (!h) {
          h = cache.Insert(Key(k), Tracked(k, alive), 1 + (random() % 3));
        }
        if (h->value() != k) {
          wrong.fetch_add(1);
        }
        if (random() % 4 == 0) {
          held.push_back(std::move(h));
        }
        if (held.size() > 8 || (random() % 8 == 0 && !held.empty())) {
          if (held.front()->value() < 0) {
            wrong.fetch_add(1);
          }
          held.erase(held.begin());
        }
        if (random() % 100 == 0) {
          cache.Erase(Key(k));
        }
      }
    });
  }
  for (std::thread& thread : threads) {
    thread.join();
  }
  EXPECT_EQ(wrong.load(), 0);
  const CacheStats stats = cache.GetStats();
  EXPECT_EQ(stats.hits + stats.misses, std::uint64_t{kThreads} * kOperations);
  EXPECT_LE(stats.charge, 64U);  // every handle is released, so every shard is within capacity
  EXPECT_EQ(static_cast<std::size_t>(alive->load()), stats.entries);
}

}  // namespace
}  // namespace forgedb
