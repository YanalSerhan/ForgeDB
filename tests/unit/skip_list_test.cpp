#include "memtable/skip_list.h"

#include <algorithm>
#include <atomic>
#include <compare>
#include <cstddef>
#include <cstdint>

#include <gtest/gtest.h>

#include <latch>
#include <random>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include "memtable/arena.h"

namespace forgedb {
namespace {

using Key = std::uint64_t;

struct Ascending {
  std::strong_ordering operator()(Key a, Key b) const noexcept { return a <=> b; }
};

using List = SkipList<Key, Ascending>;

std::vector<Key> Collect(const List& list) {
  std::vector<Key> keys;
  List::Iterator it(&list);
  for (it.SeekToFirst(); it.Valid(); it.Next()) {
    keys.push_back(it.key());
  }
  return keys;
}

// --- Basic behaviour ---------------------------------------------------------

TEST(SkipListTest, Empty) {
  Arena arena;
  const List list(Ascending{}, &arena);
  EXPECT_FALSE(list.Contains(10));

  List::Iterator it(&list);
  EXPECT_FALSE(it.Valid()) << "a new iterator is not positioned";
  it.SeekToFirst();
  EXPECT_FALSE(it.Valid());
  it.Seek(0);
  EXPECT_FALSE(it.Valid());
  it.Seek(100);
  EXPECT_FALSE(it.Valid());
}

TEST(SkipListTest, SingleKey) {
  Arena arena;
  List list(Ascending{}, &arena);
  list.Insert(5);
  EXPECT_TRUE(list.Contains(5));
  EXPECT_FALSE(list.Contains(4));
  EXPECT_FALSE(list.Contains(6));
  EXPECT_EQ(Collect(list), std::vector<Key>({5}));
}

TEST(SkipListTest, InsertAndContainsMatchStdSet) {
  constexpr Key kRange = 5000;
  Arena arena;
  List list(Ascending{}, &arena);
  std::set<Key> model;
  std::mt19937_64 rng(301);
  for (int i = 0; i < 2000; ++i) {
    const Key key = rng() % kRange;
    if (model.insert(key).second) {  // the list requires unique keys
      list.Insert(key);
    }
  }
  for (Key key = 0; key < kRange; ++key) {
    ASSERT_EQ(list.Contains(key), model.contains(key)) << "key " << key;
  }
  EXPECT_EQ(Collect(list), std::vector<Key>(model.begin(), model.end()))
      << "iteration visits every key once, in ascending order";
}

TEST(SkipListTest, SeekIsLowerBound) {
  constexpr Key kRange = 3000;
  Arena arena;
  List list(Ascending{}, &arena);
  std::set<Key> model;
  std::mt19937_64 rng(302);
  for (int i = 0; i < 1000; ++i) {
    const Key key = rng() % kRange;
    if (model.insert(key).second) {
      list.Insert(key);
    }
  }
  List::Iterator it(&list);
  // Every target in range plus a margin past the largest key.
  for (Key target = 0; target < kRange + 10; ++target) {
    it.Seek(target);
    const auto expected = model.lower_bound(target);
    if (expected == model.end()) {
      ASSERT_FALSE(it.Valid()) << "target " << target;
    } else {
      ASSERT_TRUE(it.Valid()) << "target " << target;
      ASSERT_EQ(it.key(), *expected) << "target " << target;
    }
  }
}

TEST(SkipListTest, SeekEdgeCases) {
  Arena arena;
  List list(Ascending{}, &arena);
  for (const Key key : {10U, 20U, 30U}) {
    list.Insert(key);
  }
  List::Iterator it(&list);

  it.Seek(0);  // before the first key
  ASSERT_TRUE(it.Valid());
  EXPECT_EQ(it.key(), 10U);

  it.Seek(10);  // exactly the first key
  ASSERT_TRUE(it.Valid());
  EXPECT_EQ(it.key(), 10U);

  it.Seek(11);  // between keys
  ASSERT_TRUE(it.Valid());
  EXPECT_EQ(it.key(), 20U);

  it.Seek(30);  // exactly the last key, then step off the end
  ASSERT_TRUE(it.Valid());
  EXPECT_EQ(it.key(), 30U);
  it.Next();
  EXPECT_FALSE(it.Valid());

  it.Seek(31);  // after the last key
  EXPECT_FALSE(it.Valid());

  it.Seek(UINT64_MAX);
  EXPECT_FALSE(it.Valid());

  it.SeekToFirst();  // an iterator can be repositioned after running off the end
  ASSERT_TRUE(it.Valid());
  EXPECT_EQ(it.key(), 10U);
}

TEST(SkipListTest, ExtremeKeys) {
  Arena arena;
  List list(Ascending{}, &arena);
  list.Insert(UINT64_MAX);
  list.Insert(0);  // also equals the head sentinel's key, which is never compared
  EXPECT_TRUE(list.Contains(0));
  EXPECT_TRUE(list.Contains(UINT64_MAX));
  EXPECT_EQ(Collect(list), std::vector<Key>({0, UINT64_MAX}));
}

// Sorted input is the worst case for a plain linked list and a common one for
// a memtable (sequential keys). Both directions must still work.
TEST(SkipListTest, AscendingAndDescendingInsertionOrder) {
  constexpr Key kCount = 10000;
  {
    Arena arena;
    List list(Ascending{}, &arena);
    for (Key key = 0; key < kCount; ++key) {
      list.Insert(key);
    }
    const std::vector<Key> keys = Collect(list);
    ASSERT_EQ(keys.size(), kCount);
    EXPECT_TRUE(std::ranges::is_sorted(keys));
  }
  {
    Arena arena;
    List list(Ascending{}, &arena);
    for (Key key = kCount; key > 0; --key) {
      list.Insert(key);
    }
    const std::vector<Key> keys = Collect(list);
    ASSERT_EQ(keys.size(), kCount);
    EXPECT_EQ(keys.front(), 1U);
    EXPECT_EQ(keys.back(), kCount);
    EXPECT_TRUE(std::ranges::is_sorted(keys));
  }
}

// The list must order by the comparator, not by operator<.
TEST(SkipListTest, UsesTheComparator) {
  struct Descending {
    std::strong_ordering operator()(Key a, Key b) const noexcept { return b <=> a; }
  };
  Arena arena;
  SkipList<Key, Descending> list(Descending{}, &arena);
  for (const Key key : {5U, 1U, 9U, 3U}) {
    list.Insert(key);
  }
  std::vector<Key> keys;
  SkipList<Key, Descending>::Iterator it(&list);
  for (it.SeekToFirst(); it.Valid(); it.Next()) {
    keys.push_back(it.key());
  }
  EXPECT_EQ(keys, std::vector<Key>({9, 5, 3, 1}));
  it.Seek(4);  // first key that sorts at or after 4 in descending order
  ASSERT_TRUE(it.Valid());
  EXPECT_EQ(it.key(), 3U);
}

// Node heights come from a seeded generator, so the same seed and inserts give
// the same shape, observable as identical memory use.
TEST(SkipListTest, SameSeedGivesSameShape) {
  auto memory_after_inserts = [](std::uint32_t seed) {
    Arena arena;
    List list(Ascending{}, &arena, seed);
    for (Key key = 0; key < 5000; ++key) {
      list.Insert(key * 7);
    }
    return arena.MemoryUsage();
  };
  EXPECT_EQ(memory_after_inserts(42), memory_after_inserts(42));
  EXPECT_EQ(memory_after_inserts(0), memory_after_inserts(0)) << "seed 0 is accepted";
}

// Each node costs sizeof(Key) plus 8 bytes per level. With P(extra level) =
// 1/4 the expected height is 4/3, so ~16 + 8/3 bytes per node here. A broken
// height generator (always tall, or never growing) would land far outside.
TEST(SkipListTest, MemoryPerNodeMatchesExpectedHeight) {
  constexpr std::size_t kCount = 100000;
  Arena arena;
  List list(Ascending{}, &arena);
  const std::size_t before = arena.MemoryUsage();
  for (Key key = 0; key < kCount; ++key) {
    list.Insert(key);
  }
  const double per_node =
      static_cast<double>(arena.MemoryUsage() - before) / static_cast<double>(kCount);
  // Expected ~18.7 bytes of node data, plus arena waste and block overhead.
  EXPECT_GT(per_node, 17.0);
  EXPECT_LT(per_node, 22.0);
}

// --- Concurrency -------------------------------------------------------------
//
// One writer inserts keys in random order while four readers repeatedly scan
// the list. Every key carries a checksum of itself in its low 16 bits, so a
// reader that saw a node before its key was written (a broken publish) would
// read a key that fails the check. Each scan must also be strictly ascending,
// and must contain every key whose insertion finished before the scan began.
// Halfway through, the writer waits until every reader has finished a scan,
// so each reader is guaranteed to scan while inserts are still happening, even
// on a machine with fewer cores than threads.

constexpr int kChecksumBits = 16;

Key MakeKey(std::uint64_t value) {
  // A cheap mix (from splitmix64) so neighbouring values get unrelated checksums.
  std::uint64_t h = value * 0x9E3779B97F4A7C15ULL;
  h ^= h >> 29;
  return (value << kChecksumBits) | (h & 0xFFFFU);
}

bool HasValidChecksum(Key key) { return MakeKey(key >> kChecksumBits) == key; }

TEST(SkipListConcurrencyTest, ReadersSeeSortedConsistentPrefix) {
  constexpr int kReaders = 4;
  constexpr std::size_t kKeys = 5000;

  // Insertion order: shuffled, so inserts land all over the list.
  std::vector<Key> order;
  order.reserve(kKeys);
  for (std::uint64_t value = 0; value < kKeys; ++value) {
    order.push_back(MakeKey(value));
  }
  std::shuffle(order.begin(), order.end(), std::mt19937_64(303));

  Arena arena;
  List list(Ascending{}, &arena);
  // How many keys of `order` are fully inserted. Release by the writer,
  // acquire by readers: a reader that reads n is guaranteed to find
  // order[0..n) in the list.
  std::atomic<std::size_t> inserted{0};
  std::atomic<bool> writer_done{false};
  std::atomic<int> readers_with_a_scan{0};
  std::latch start(kReaders + 1);

  // One slot per reader, so readers never write to shared state.
  std::vector<std::string> failures(kReaders);
  std::vector<int> scans_during_writes(kReaders, 0);
  std::vector<std::thread> readers;
  readers.reserve(kReaders);
  for (int r = 0; r < kReaders; ++r) {
    readers.emplace_back([&, r] {
      std::string& failure = failures[static_cast<std::size_t>(r)];
      start.arrive_and_wait();
      std::vector<Key> seen;
      bool last_scan = false;
      while (failure.empty() && !last_scan) {
        // Read the flag before the scan: a scan that starts after the writer
        // finished must see every key, and that is the last one.
        last_scan = writer_done.load(std::memory_order_acquire);
        const std::size_t must_see = inserted.load(std::memory_order_acquire);

        seen.clear();
        List::Iterator it(&list);
        for (it.SeekToFirst(); it.Valid(); it.Next()) {
          const Key key = it.key();
          if (!HasValidChecksum(key)) {
            failure = "key with a bad checksum: " + std::to_string(key);
            break;
          }
          if (!seen.empty() && key <= seen.back()) {
            failure = "scan not strictly ascending at " + std::to_string(key);
            break;
          }
          seen.push_back(key);
        }
        for (std::size_t i = 0; failure.empty() && i < must_see; ++i) {
          if (!std::ranges::binary_search(seen, order[i])) {
            failure = "missing key " + std::to_string(order[i]) + ", inserted before the scan";
          }
        }
        // Seek must agree with the scan: probe a few keys the scan saw.
        for (std::size_t i = 0; failure.empty() && i < seen.size(); i += 97) {
          it.Seek(seen[i]);
          if (!it.Valid() || it.key() != seen[i]) {
            failure = "Seek did not find " + std::to_string(seen[i]);
          }
        }
        if (!last_scan) {
          ++scans_during_writes[static_cast<std::size_t>(r)];
          if (scans_during_writes[static_cast<std::size_t>(r)] == 1) {
            readers_with_a_scan.fetch_add(1, std::memory_order_release);
          }
        }
      }
    });
  }

  start.arrive_and_wait();
  for (std::size_t i = 0; i < kKeys; ++i) {
    if (i == kKeys / 2) {
      // Every reader's first scan starts before writer_done is set, so this
      // wait always ends (readers count a scan even when it fails).
      while (readers_with_a_scan.load(std::memory_order_acquire) < kReaders) {
        std::this_thread::yield();
      }
    }
    list.Insert(order[i]);
    inserted.store(i + 1, std::memory_order_release);
  }
  writer_done.store(true, std::memory_order_release);
  for (std::thread& reader : readers) {
    reader.join();
  }

  for (int r = 0; r < kReaders; ++r) {
    EXPECT_EQ(failures[static_cast<std::size_t>(r)], "") << "reader " << r;
    EXPECT_GE(scans_during_writes[static_cast<std::size_t>(r)], 1) << "reader " << r;
  }
  EXPECT_EQ(Collect(list).size(), kKeys);
}

}  // namespace
}  // namespace forgedb
