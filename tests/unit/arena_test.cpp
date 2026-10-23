#include "memtable/arena.h"

#include <atomic>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstring>

#include <gtest/gtest.h>

#include <random>
#include <thread>
#include <utility>
#include <vector>

namespace forgedb {
namespace {

bool IsAligned(const void* pointer, std::size_t alignment) {
  return (std::bit_cast<std::uintptr_t>(pointer) & (alignment - 1)) == 0;
}

TEST(ArenaTest, StartsEmpty) {
  const Arena arena;
  EXPECT_EQ(arena.MemoryUsage(), 0U);
}

TEST(ArenaTest, SmallAllocationsShareABlock) {
  Arena arena;
  const char* first = arena.Allocate(10);
  const char* second = arena.Allocate(20);
  EXPECT_EQ(second, first + 10) << "consecutive small allocations are a pointer bump";
  const std::size_t usage = arena.MemoryUsage();
  EXPECT_GE(usage, Arena::kBlockSize);
  EXPECT_LT(usage, 2 * Arena::kBlockSize);
}

TEST(ArenaTest, LargeAllocationThatDoesNotFitGetsItsOwnBlock) {
  Arena arena;
  const char* first = arena.Allocate(100);                       // starts a block
  const char* filler = arena.Allocate(Arena::kBlockSize - 200);  // same block; 100 bytes left
  ASSERT_EQ(filler, first + 100);

  // Too big for the 100 bytes left and larger than a quarter block: own block.
  constexpr std::size_t kLarge = (Arena::kBlockSize / 4) + 1;
  char* large = arena.Allocate(kLarge);
  EXPECT_TRUE(large < first || large >= first + Arena::kBlockSize)
      << "the large allocation lives outside the current block";
  std::memset(large, 0x5A, kLarge);  // and is fully usable

  // The current block's last 100 bytes were not abandoned.
  const char* small_after = arena.Allocate(50);
  EXPECT_EQ(small_after, filler + (Arena::kBlockSize - 200));
}

TEST(ArenaTest, SmallAllocationThatDoesNotFitStartsANewBlock) {
  Arena arena;
  const char* first = arena.Allocate(100);
  static_cast<void>(arena.Allocate(Arena::kBlockSize - 110));  // 10 bytes left
  const std::size_t usage_one_block = arena.MemoryUsage();
  const char* next = arena.Allocate(20);  // doesn't fit, small: a new block, 10 bytes abandoned
  EXPECT_TRUE(next < first || next >= first + Arena::kBlockSize);
  EXPECT_GT(arena.MemoryUsage(), usage_one_block);
}

TEST(ArenaTest, AllocationLargerThanABlock) {
  Arena arena;
  constexpr std::size_t kHuge = 10 * Arena::kBlockSize;
  char* huge = arena.Allocate(kHuge);
  std::memset(huge, 0x11, kHuge);
  EXPECT_GE(arena.MemoryUsage(), kHuge);
}

TEST(ArenaTest, AlignedAllocations) {
  Arena arena;
  std::mt19937 rng(1);
  for (int i = 0; i < 2000; ++i) {
    static_cast<void>(arena.Allocate((rng() % 13) + 1));  // knock the pointer off alignment
    // Every power of two the arena supports. alignof(std::max_align_t) is 8
    // with MSVC's headers (MSVC and clang-cl) but 16 with gcc/clang on Linux.
    for (std::size_t alignment = 1; alignment <= alignof(std::max_align_t); alignment *= 2) {
      const char* p = arena.AllocateAligned((rng() % 100) + 1, alignment);
      ASSERT_TRUE(IsAligned(p, alignment)) << "alignment " << alignment << ", iteration " << i;
    }
  }
  EXPECT_TRUE(IsAligned(arena.AllocateAligned(8), alignof(std::max_align_t)))
      << "default alignment is max_align_t";
}

TEST(ArenaTest, AlignedLargeAllocation) {
  Arena arena;
  static_cast<void>(arena.Allocate(3));
  constexpr std::size_t kMaxAlign = alignof(std::max_align_t);
  const char* p = arena.AllocateAligned(Arena::kBlockSize, kMaxAlign);
  EXPECT_TRUE(IsAligned(p, kMaxAlign));
}

// Many random allocations; each is filled with its own byte pattern and all are
// verified at the end, so any overlap between allocations shows up. Also checks
// the accounting stays within the waste bound (a quarter block per block, plus
// per-block bookkeeping).
TEST(ArenaTest, RandomAllocationsDoNotOverlapAndAreAccounted) {
  Arena arena;
  std::mt19937 rng(2);
  std::vector<std::pair<char*, std::size_t>> allocations;
  std::size_t requested = 0;
  for (int i = 0; i < 20000; ++i) {
    std::size_t size = 0;
    switch (rng() % 10) {
      case 0:
        size = (rng() % 6000) + 1;  // sometimes larger than a block
        break;
      case 1:
        size = (rng() % 1500) + 1;  // around the quarter-block threshold
        break;
      default:
        size = (rng() % 40) + 1;  // typical small keys and nodes
        break;
    }
    char* p = (i % 3 == 0) ? arena.AllocateAligned(size) : arena.Allocate(size);
    std::memset(p, i % 256, size);
    allocations.emplace_back(p, size);
    requested += size;

    const std::size_t usage = arena.MemoryUsage();
    ASSERT_GE(usage, requested);
    // Worst case: every block wastes just under a quarter, plus alignment
    // padding and one pointer of bookkeeping per block.
    ASSERT_LE(usage, requested + (requested / 3) + (2 * Arena::kBlockSize) +
                         (std::size_t{64} * static_cast<std::size_t>(i + 1)));
  }
  for (std::size_t i = 0; i < allocations.size(); ++i) {
    const auto [p, size] = allocations[i];
    for (std::size_t b = 0; b < size; ++b) {
      ASSERT_EQ(static_cast<unsigned char>(p[b]), i % 256) << "allocation " << i << " byte " << b;
    }
  }
}

TEST(ArenaTest, MemoryUsageReadableFromAnotherThread) {
  Arena arena;
  std::atomic<bool> done{false};
  std::atomic<bool> went_backwards{false};
  std::thread reader([&] {
    std::size_t last = 0;
    while (!done.load(std::memory_order_acquire)) {
      const std::size_t now = arena.MemoryUsage();
      if (now < last) {
        went_backwards.store(true);
      }
      last = now;
    }
  });
  for (int i = 0; i < 50000; ++i) {
    static_cast<void>(arena.Allocate(64));
  }
  done.store(true, std::memory_order_release);
  reader.join();
  EXPECT_FALSE(went_backwards.load()) << "usage only grows";
  EXPECT_GE(arena.MemoryUsage(), 50000U * 64U);
}

}  // namespace
}  // namespace forgedb
