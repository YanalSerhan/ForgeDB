// Arena: a bump allocator for the memtable.
//
// The memtable allocates many small objects (skip-list nodes, encoded keys and
// values) and frees them all at once when it is discarded after a flush. An
// arena makes each allocation a pointer bump inside a 4 KiB block, instead of a
// trip through the general-purpose heap, and frees everything in one go when it
// is destroyed. Memory is never returned piecemeal.
//
// Policy:
//   - Requests are served from the current 4 KiB block.
//   - A request that doesn't fit and is larger than a quarter block gets a
//     block of its own, and the current block keeps serving small requests.
//   - Otherwise a new 4 KiB block is started and the rest of the old one is
//     abandoned. That remainder was too small for this request, which is at
//     most a quarter block, so at most a quarter of each block is wasted.
//
// Thread safety: Allocate/AllocateAligned need external synchronization (the
// memtable has a single writer). MemoryUsage() may be called from any thread.
#pragma once

#include <atomic>
#include <cstddef>
#include <memory>
#include <vector>

namespace forgedb {

class Arena {
 public:
  static constexpr std::size_t kBlockSize = 4096;

  Arena() = default;
  ~Arena() = default;
  Arena(const Arena&) = delete;
  Arena& operator=(const Arena&) = delete;
  Arena(Arena&&) = delete;
  Arena& operator=(Arena&&) = delete;

  // Returns `bytes` (> 0) bytes of uninitialized memory with no particular
  // alignment, valid until the arena is destroyed. Use for raw byte data.
  [[nodiscard]] char* Allocate(std::size_t bytes);

  // Like Allocate, but aligned to `alignment`, a power of two no larger than
  // alignof(std::max_align_t), the most a fresh heap block is guaranteed to
  // have. Note that this is 8 with MSVC's headers (MSVC and clang-cl), 16 with
  // gcc/clang on Linux. Use for objects, e.g. skip-list nodes (which need 8).
  [[nodiscard]] char* AllocateAligned(std::size_t bytes,
                                      std::size_t alignment = alignof(std::max_align_t));

  // Total bytes obtained from the heap, including block bookkeeping. Read by
  // other threads to decide when the memtable is full, so it is an atomic;
  // see MemoryUsage's definition for why relaxed ordering is enough.
  [[nodiscard]] std::size_t MemoryUsage() const noexcept {
    return memory_usage_.load(std::memory_order_relaxed);
  }

 private:
  char* AllocateFallback(std::size_t bytes);
  char* AllocateNewBlock(std::size_t block_bytes);

  char* alloc_ptr_ = nullptr;              // next free byte in the current block
  std::size_t alloc_bytes_remaining_ = 0;  // free bytes left in the current block
  // Raw byte storage, freed with delete[] when the arena is destroyed.
  std::vector<std::unique_ptr<char[]>> blocks_;  // NOLINT(*-avoid-c-arrays)
  std::atomic<std::size_t> memory_usage_{0};
};

}  // namespace forgedb
