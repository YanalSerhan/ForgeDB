#include "memtable/memtable.h"

#include <atomic>
#include <cstddef>
#include <cstdint>

#include <gtest/gtest.h>

#include <latch>
#include <map>
#include <optional>
#include <ostream>
#include <random>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include "db/internal_key.h"
#include "memtable/arena.h"

namespace forgedb {
namespace {

using GetResult = MemTable::GetResult;

// Get() as a value: the result, plus the value when found.
struct Lookup {
  GetResult result = GetResult::kNotFound;
  std::string value;

  friend bool operator==(const Lookup&, const Lookup&) = default;
};

Lookup Get(const MemTable& table, std::string_view user_key, SequenceNumber snapshot) {
  const LookupKey key(user_key, snapshot);
  Lookup lookup;
  lookup.result = table.Get(key, &lookup.value);
  return lookup;
}

Lookup NotFound() { return {.result = GetResult::kNotFound, .value = ""}; }
Lookup Deleted() { return {.result = GetResult::kDeleted, .value = ""}; }
Lookup Found(std::string value) { return {.result = GetResult::kFound, .value = std::move(value)}; }

std::ostream& operator<<(std::ostream& os, const Lookup& lookup) {
  switch (lookup.result) {
    case GetResult::kNotFound:
      return os << "NotFound";
    case GetResult::kDeleted:
      return os << "Deleted";
    case GetResult::kFound:
      return os << "Found(\"" << lookup.value << "\")";
  }
  return os << "invalid";
}

// --- Get ---------------------------------------------------------------------

TEST(MemTableTest, EmptyTableFindsNothing) {
  const MemTableRef table = MemTable::Create();
  EXPECT_EQ(Get(*table, "a", kMaxSequenceNumber), NotFound());
  EXPECT_EQ(Get(*table, "", kMaxSequenceNumber), NotFound());
}

TEST(MemTableTest, ValueIsVisibleFromItsSequenceOn) {
  const MemTableRef table = MemTable::Create();
  table->Add(10, ValueType::kValue, "key", "v10");
  EXPECT_EQ(Get(*table, "key", 9), NotFound()) << "a snapshot before the write";
  EXPECT_EQ(Get(*table, "key", 10), Found("v10"));
  EXPECT_EQ(Get(*table, "key", kMaxSequenceNumber), Found("v10"));
}

TEST(MemTableTest, NewestVisibleVersionWins) {
  const MemTableRef table = MemTable::Create();
  table->Add(1, ValueType::kValue, "k", "v1");
  table->Add(5, ValueType::kValue, "k", "v5");
  table->Add(9, ValueType::kDeletion, "k", "");
  table->Add(12, ValueType::kValue, "k", "v12");

  EXPECT_EQ(Get(*table, "k", 0), NotFound());
  EXPECT_EQ(Get(*table, "k", 1), Found("v1"));
  EXPECT_EQ(Get(*table, "k", 4), Found("v1"));
  EXPECT_EQ(Get(*table, "k", 5), Found("v5"));
  EXPECT_EQ(Get(*table, "k", 8), Found("v5"));
  EXPECT_EQ(Get(*table, "k", 9), Deleted()) << "the tombstone hides v5";
  EXPECT_EQ(Get(*table, "k", 11), Deleted());
  EXPECT_EQ(Get(*table, "k", 12), Found("v12"));
  EXPECT_EQ(Get(*table, "k", kMaxSequenceNumber), Found("v12"));
}

TEST(MemTableTest, VersionsAddedOutOfOrderStillResolveBySequence) {
  // The DB adds in sequence order, but the memtable must not depend on it.
  const MemTableRef table = MemTable::Create();
  table->Add(7, ValueType::kValue, "k", "v7");
  table->Add(3, ValueType::kValue, "k", "v3");
  EXPECT_EQ(Get(*table, "k", 5), Found("v3"));
  EXPECT_EQ(Get(*table, "k", 7), Found("v7"));
}

TEST(MemTableTest, NeighbouringKeysAreNotConfused) {
  const MemTableRef table = MemTable::Create();
  table->Add(1, ValueType::kValue, "a", "va");
  table->Add(2, ValueType::kValue, "ab", "vab");
  table->Add(3, ValueType::kValue, "c", "vc");

  EXPECT_EQ(Get(*table, "a", 10), Found("va"));
  EXPECT_EQ(Get(*table, "ab", 10), Found("vab"));
  EXPECT_EQ(Get(*table, "c", 10), Found("vc"));
  EXPECT_EQ(Get(*table, "", 10), NotFound()) << "a prefix of every key";
  EXPECT_EQ(Get(*table, "aa", 10), NotFound()) << "between a and ab";
  EXPECT_EQ(Get(*table, "b", 10), NotFound()) << "between ab and c";
  EXPECT_EQ(Get(*table, "abc", 10), NotFound()) << "extends a stored key";
  EXPECT_EQ(Get(*table, "d", 10), NotFound()) << "after the last key";
  EXPECT_EQ(Get(*table, "ab", 1), NotFound()) << "ab exists only from sequence 2";
}

TEST(MemTableTest, DeletionWithoutAnOlderValue) {
  const MemTableRef table = MemTable::Create();
  table->Add(4, ValueType::kDeletion, "gone", "");
  EXPECT_EQ(Get(*table, "gone", 3), NotFound());
  EXPECT_EQ(Get(*table, "gone", 4), Deleted());
}

TEST(MemTableTest, EmptyKeysAndValuesAndBinaryBytes) {
  const MemTableRef table = MemTable::Create();
  const std::string binary_key("\x00\xff\x00", 3);
  const std::string binary_value("\xff\x00\x80\x7f", 4);
  table->Add(1, ValueType::kValue, "", "empty key");
  table->Add(2, ValueType::kValue, "empty value", "");
  table->Add(3, ValueType::kValue, binary_key, binary_value);

  EXPECT_EQ(Get(*table, "", 10), Found("empty key"));
  EXPECT_EQ(Get(*table, "empty value", 10), Found(""));
  EXPECT_EQ(Get(*table, binary_key, 10), Found(binary_value));
  EXPECT_EQ(Get(*table, std::string("\x00\xff", 2), 10), NotFound())
      << "a prefix of the binary key";
}

TEST(MemTableTest, LargeKeysAndValues) {
  const MemTableRef table = MemTable::Create();
  // Longer than LookupKey's inline buffer, and a value bigger than an arena block.
  const std::string key(1000, 'k');
  const std::string value(3 * Arena::kBlockSize, 'v');
  // Lengths >= 128 take multi-byte varints in the entry.
  const std::string medium_value(200, 'm');
  table->Add(1, ValueType::kValue, key, value);
  table->Add(2, ValueType::kValue, "medium", medium_value);
  EXPECT_EQ(Get(*table, key, 1), Found(value));
  EXPECT_EQ(Get(*table, "medium", 2), Found(medium_value));
}

TEST(MemTableTest, MaximumSequenceNumber) {
  const MemTableRef table = MemTable::Create();
  table->Add(kMaxSequenceNumber - 1, ValueType::kValue, "k", "older");
  table->Add(kMaxSequenceNumber, ValueType::kValue, "k", "newest");
  EXPECT_EQ(Get(*table, "k", kMaxSequenceNumber), Found("newest"));
  EXPECT_EQ(Get(*table, "k", kMaxSequenceNumber - 1), Found("older"));
}

TEST(MemTableTest, GetLeavesValueAloneUnlessFound) {
  const MemTableRef table = MemTable::Create();
  table->Add(1, ValueType::kDeletion, "d", "");
  std::string value = "untouched";
  EXPECT_EQ(table->Get(LookupKey("d", 5), &value), GetResult::kDeleted);
  EXPECT_EQ(value, "untouched");
  EXPECT_EQ(table->Get(LookupKey("missing", 5), &value), GetResult::kNotFound);
  EXPECT_EQ(value, "untouched");
}

TEST(MemTableTest, MemoryUsageGrowsWithData) {
  const MemTableRef table = MemTable::Create();
  const std::size_t empty = table->ApproximateMemoryUsage();
  EXPECT_GT(empty, 0U) << "the skip list's head node is already allocated";
  const std::string value(100, 'x');
  for (int i = 0; i < 1000; ++i) {
    table->Add(static_cast<SequenceNumber>(i) + 1, ValueType::kValue, "key" + std::to_string(i),
               value);
  }
  // At least the payload: 1000 * (~7-byte key + 8-byte tag + 100-byte value).
  EXPECT_GE(table->ApproximateMemoryUsage(), empty + (std::size_t{1000} * 110));
}

// Random puts and deletes over a small key space (so keys get many versions),
// then every key is looked up at many snapshots and compared with a model.
TEST(MemTableTest, AgreesWithModelAtEverySnapshot) {
  for (const std::uint64_t seed : {1U, 2U, 3U}) {
    SCOPED_TRACE(seed);
    std::mt19937_64 rng(seed);
    const MemTableRef table = MemTable::Create();
    // model[key][sequence] = value, or nullopt for a deletion
    std::map<std::string, std::map<SequenceNumber, std::optional<std::string>>> model;

    constexpr SequenceNumber kOperations = 20000;
    for (SequenceNumber sequence = 1; sequence <= kOperations; ++sequence) {
      const std::string key = "k" + std::to_string(rng() % 300);
      if (rng() % 4 == 0) {
        table->Add(sequence, ValueType::kDeletion, key, "");
        model[key][sequence] = std::nullopt;
      } else {
        std::string value = "v" + std::to_string(sequence) + std::string(rng() % 20, 'x');
        table->Add(sequence, ValueType::kValue, key, value);
        model[key][sequence] = std::move(value);
      }
    }

    for (int probe = 0; probe < 20000; ++probe) {
      const std::string key = "k" + std::to_string(rng() % 320);  // some never written
      const SequenceNumber snapshot = rng() % (kOperations + 10);
      Lookup expected = NotFound();
      if (const auto versions = model.find(key); versions != model.end()) {
        // The newest version with sequence <= snapshot.
        if (auto it = versions->second.upper_bound(snapshot); it != versions->second.begin()) {
          --it;
          expected = it->second ? Found(*it->second) : Deleted();
        }
      }
      ASSERT_EQ(Get(*table, key, snapshot), expected) << key << " @" << snapshot;
    }
  }
}

// --- Reference counting -------------------------------------------------------

TEST(MemTableRefTest, CreateHoldsOneReference) {
  const MemTableRef table = MemTable::Create();
  ASSERT_TRUE(table);
  EXPECT_EQ(table->RefCountForTesting(), 1);
}

TEST(MemTableRefTest, CopyAddsAndDestructionDropsAReference) {
  const MemTableRef table = MemTable::Create();
  {
    const MemTableRef copy = table;  // NOLINT(performance-unnecessary-copy-initialization)
    EXPECT_EQ(table->RefCountForTesting(), 2);
    EXPECT_EQ(&*copy, &*table);
  }
  EXPECT_EQ(table->RefCountForTesting(), 1);
}

TEST(MemTableRefTest, MoveTransfersTheReference) {
  MemTableRef first = MemTable::Create();
  const MemTable* const raw = &*first;
  const MemTableRef second = std::move(first);
  EXPECT_FALSE(first);  // NOLINT(bugprone-use-after-move,clang-analyzer-cplusplus.Move)
  ASSERT_TRUE(second);
  EXPECT_EQ(&*second, raw);
  EXPECT_EQ(second->RefCountForTesting(), 1) << "a move neither adds nor drops one";
}

TEST(MemTableRefTest, AssignmentAndReset) {
  MemTableRef a = MemTable::Create();
  const MemTableRef b = MemTable::Create();
  const MemTableRef keep_a = a;
  EXPECT_EQ(a->RefCountForTesting(), 2);

  a = b;  // drops a reference to the first table, adds one to the second
  EXPECT_EQ(keep_a->RefCountForTesting(), 1);
  EXPECT_EQ(b->RefCountForTesting(), 2);

  const MemTableRef& same = a;
  a = same;  // self-assignment changes nothing
  EXPECT_EQ(b->RefCountForTesting(), 2);

  MemTableRef c = MemTable::Create();
  const MemTable* const c_table = &*c;
  a = std::move(c);  // drops a's reference to b; takes over c's without adding one
  EXPECT_FALSE(c);   // NOLINT(bugprone-use-after-move,clang-analyzer-cplusplus.Move)
  EXPECT_EQ(&*a, c_table);
  EXPECT_EQ(a->RefCountForTesting(), 1);
  EXPECT_EQ(b->RefCountForTesting(), 1);

  a.Reset();
  EXPECT_FALSE(a);
  a.Reset();  // resetting an empty reference is a no-op
  EXPECT_FALSE(a);
}

TEST(MemTableRefTest, ManualRefAndUnref) {
  const MemTableRef table = MemTable::Create();
  table->Ref();
  EXPECT_EQ(table->RefCountForTesting(), 2);
  table->Unref();
  EXPECT_EQ(table->RefCountForTesting(), 1);
}

// The last reference can be dropped on any thread: several threads each take
// a reference, read, and drop it, racing with the creating thread dropping
// its own. ASan reports a use-after-free or double delete if the count is
// wrong; the memtable's arena makes a leak show up in LeakSanitizer.
TEST(MemTableRefTest, LastReferenceDroppedOnAnotherThread) {
  constexpr int kThreads = 4;
  for (int round = 0; round < 50; ++round) {
    MemTableRef table = MemTable::Create();
    table->Add(1, ValueType::kValue, "k", "v");
    std::latch start(kThreads + 1);
    std::atomic<int> found{0};
    std::vector<std::thread> threads;
    threads.reserve(kThreads);
    for (int t = 0; t < kThreads; ++t) {
      threads.emplace_back([&found, &start, mine = table] {  // a copy: one more reference
        start.arrive_and_wait();
        if (Get(*mine, "k", 1) == Found("v")) {
          found.fetch_add(1, std::memory_order_relaxed);
        }
      });
    }
    table.Reset();  // the threads now hold the only references
    start.arrive_and_wait();
    for (std::thread& thread : threads) {
      thread.join();
    }
    EXPECT_EQ(found.load(), kThreads);
  }
}

// --- Concurrency ----------------------------------------------------------------
//
// One writer adds versions while readers look up keys whose writes have
// completed: every such lookup must find exactly the value written (so the
// entry's bytes were visible when its node was), at the right snapshot.
// Halfway through, the writer waits until every reader has done a lookup.

std::string ValueFor(SequenceNumber sequence) { return "value-" + std::to_string(sequence * 7919); }

TEST(MemTableConcurrencyTest, ReadersSeeCompletedWrites) {
  constexpr int kReaders = 4;
  constexpr SequenceNumber kWrites = 20000;
  constexpr int kKeys = 500;

  const MemTableRef table = MemTable::Create();
  std::atomic<SequenceNumber> completed{0};  // writes 1..completed are in the table
  std::atomic<bool> done{false};
  std::atomic<int> readers_with_a_lookup{0};
  std::latch start(kReaders + 1);
  std::vector<std::string> failures(kReaders);
  std::vector<std::thread> readers;
  readers.reserve(kReaders);

  // Sequence s writes key s % kKeys, so the newest version of key k visible at
  // snapshot S is the largest s <= S with s % kKeys == k.
  for (int r = 0; r < kReaders; ++r) {
    readers.emplace_back([&, r] {
      std::string& failure = failures[static_cast<std::size_t>(r)];
      std::mt19937_64 rng(static_cast<std::uint64_t>(r) + 100);
      bool counted = false;
      start.arrive_and_wait();
      while (failure.empty() && !done.load(std::memory_order_acquire)) {
        const SequenceNumber snapshot = completed.load(std::memory_order_acquire);
        if (snapshot == 0) {
          continue;
        }
        const SequenceNumber written = (rng() % snapshot) + 1;  // a completed write
        const std::string key = "key" + std::to_string(written % kKeys);
        // Newest version of that key at the snapshot.
        const SequenceNumber newest = written + (((snapshot - written) / kKeys) * kKeys);
        const Lookup lookup = Get(*table, key, snapshot);
        if (lookup != Found(ValueFor(newest))) {
          failure = key + " @" + std::to_string(snapshot) + ": expected value of sequence " +
                    std::to_string(newest);
        }
        if (!counted) {
          counted = true;
          readers_with_a_lookup.fetch_add(1, std::memory_order_release);
        }
      }
    });
  }

  start.arrive_and_wait();
  for (SequenceNumber sequence = 1; sequence <= kWrites; ++sequence) {
    if (sequence == kWrites / 2) {
      // Make sure every reader looks something up while writes are still
      // happening, even on a machine with fewer cores than threads.
      while (readers_with_a_lookup.load(std::memory_order_acquire) < kReaders) {
        std::this_thread::yield();
      }
    }
    table->Add(sequence, ValueType::kValue, "key" + std::to_string(sequence % kKeys),
               ValueFor(sequence));
    completed.store(sequence, std::memory_order_release);
  }
  done.store(true, std::memory_order_release);
  for (std::thread& reader : readers) {
    reader.join();
  }
  for (int r = 0; r < kReaders; ++r) {
    EXPECT_EQ(failures[static_cast<std::size_t>(r)], "") << "reader " << r;
  }
}

}  // namespace
}  // namespace forgedb
