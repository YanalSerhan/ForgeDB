#include "memtable/arena.h"

#include <bit>
#include <cstdint>
#include <memory>
#include <utility>

#include "forgedb/check.h"

namespace forgedb {
namespace {

// Used only by debug checks, hence [[maybe_unused]] for release builds.
[[maybe_unused]] bool IsAligned(const void* pointer, std::size_t alignment) noexcept {
  return (std::bit_cast<std::uintptr_t>(pointer) & (alignment - 1)) == 0;
}

}  // namespace

char* Arena::Allocate(std::size_t bytes) {
  // Zero-byte allocations would hand out the same address twice; nobody needs them.
  FORGEDB_DCHECK(bytes > 0);
  if (bytes <= alloc_bytes_remaining_) {
    char* result = alloc_ptr_;
    alloc_ptr_ += bytes;
    alloc_bytes_remaining_ -= bytes;
    return result;
  }
  return AllocateFallback(bytes);
}

char* Arena::AllocateAligned(std::size_t bytes, std::size_t alignment) {
  FORGEDB_DCHECK(bytes > 0);
  FORGEDB_DCHECK(std::has_single_bit(alignment) && alignment <= alignof(std::max_align_t));

  // std::align moves `ptr` forward to the next multiple of `alignment` and
  // shrinks `space` accordingly, or returns nullptr if `bytes` no longer fits.
  void* ptr = alloc_ptr_;
  std::size_t space = alloc_bytes_remaining_;
  if (alloc_ptr_ != nullptr && std::align(alignment, bytes, ptr, space) != nullptr) {
    char* result = static_cast<char*>(ptr);
    alloc_ptr_ = result + bytes;
    alloc_bytes_remaining_ = space - bytes;
    return result;
  }
  // A fresh block starts at an address aligned for any fundamental type
  // (new[] guarantees alignof(std::max_align_t)), so its first bytes are aligned.
  char* result = AllocateFallback(bytes);
  FORGEDB_DCHECK(IsAligned(result, alignment));
  return result;
}

char* Arena::AllocateFallback(std::size_t bytes) {
  if (bytes > kBlockSize / 4) {
    // Big request: its own block, so the current block's free space stays usable.
    return AllocateNewBlock(bytes);
  }
  // Small request that doesn't fit: abandon the rest of the current block
  // (less than `bytes`, so less than a quarter block) and start a new one.
  alloc_ptr_ = AllocateNewBlock(kBlockSize);
  alloc_bytes_remaining_ = kBlockSize;

  char* result = alloc_ptr_;
  alloc_ptr_ += bytes;
  alloc_bytes_remaining_ -= bytes;
  return result;
}

char* Arena::AllocateNewBlock(std::size_t block_bytes) {
  // make_unique_for_overwrite: no zero-filling; callers overwrite the memory anyway.
  auto block = std::make_unique_for_overwrite<char[]>(block_bytes);  // NOLINT(*-avoid-c-arrays)
  char* start = block.get();
  blocks_.push_back(std::move(block));
  // Why relaxed is enough: MemoryUsage() is a statistic. A reader needs a value
  // that isn't torn and that eventually reflects the writer's allocations; it
  // never uses that value to reach memory the writer prepared, so no
  // happens-before relationship (acquire/release) is needed. The atomic is
  // still required: a plain size_t read while another thread writes it is a
  // data race, which is undefined behavior in C++.
  memory_usage_.fetch_add(block_bytes + sizeof(char*), std::memory_order_relaxed);
  return start;
}

}  // namespace forgedb
