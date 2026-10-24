// SkipList: the ordered index inside a memtable.
//
// A sorted linked list with "express lanes": every node is on level 0, and
// each node is also on levels 1..h-1 with probability 1/4 per extra level, so
// a search skips most nodes and costs O(log n) expected comparisons.
//
// Contract
//   - One writer at a time. Insert() must be externally synchronized (the
//     memtable's writer holds the DB mutex or is the group-commit leader).
//   - Any number of readers, concurrently with that writer, without locks.
//   - No deletion. Nodes live in the Arena and are freed all at once with it,
//     so a reader can never hold a pointer to freed memory and there is no
//     ABA problem. The arena must outlive the list and all its iterators.
//   - Keys are unique: inserting a key equal to an existing one is a bug
//     (memtable keys contain a unique sequence number). Checked in debug.
//
// Memory ordering (why concurrent readers are safe)
//   Insert builds a node privately: the key, then each level's next pointer,
//   written with relaxed stores, because no other thread can reach the node
//   yet. It then publishes the node level by level, bottom-up, with a release
//   store into the predecessor's next pointer. Readers follow next pointers
//   with acquire loads. Release/acquire pairing means a reader that sees the
//   new pointer also sees everything the writer did before that store: the
//   key and the node's own next pointer on that level and every lower one.
//   Bottom-up order means a node is never reachable on a level without
//   already being linked on all levels below it.
//
//   max_height_ is relaxed: it is a search hint, not a publication point.
//   A reader that sees a stale (lower) height starts its search lower, which
//   is slower but correct. A reader that sees a new height before the head's
//   pointers on the new levels are set finds nullptr there and drops down.
//
// Node layout: the key, then a tower of `height` atomic next pointers in the
// same arena allocation (see Node), so a node costs sizeof(Key) + 8 * height
// bytes, and the key and the level-0 pointer usually share a cache line.
//
// Decision: no Iterator::Prev and no SeekToLast. Nothing in ForgeDB walks the
// memtable backwards (Get, flush and the DB iterator go forward). They could be
// added later without changing the layout: Prev as a search for the last node
// before the current key, O(log n) per step.
#pragma once

#include <array>
#include <atomic>
#include <compare>
#include <cstddef>
#include <cstdint>
#include <new>
#include <random>
#include <type_traits>

#include "forgedb/check.h"

#include "memtable/arena.h"

namespace forgedb {

// Comparator: a copyable callable, `std::strong_ordering(const Key&, const Key&)`.
// Key: copied into the node and never destroyed (the arena frees raw memory),
// so it must be trivially copyable and trivially destructible. The memtable
// uses `const char*` pointing at an encoded entry in the same arena.
template <typename Key, class Comparator>
class SkipList {
  struct Node;

 public:
  static constexpr int kMaxHeight = 12;
  // Each extra level is kept with probability 1/kBranching.
  static constexpr unsigned kBranching = 4;

  static_assert(std::is_trivially_copyable_v<Key> && std::is_trivially_destructible_v<Key>,
                "SkipList keys live in arena memory that is never destroyed");

  // `arena` must outlive the list. `seed` makes node heights, and therefore
  // the list's shape and memory use, deterministic.
  SkipList(Comparator compare, Arena* arena, std::uint32_t seed = 0x5EED);
  ~SkipList() = default;
  SkipList(const SkipList&) = delete;
  SkipList& operator=(const SkipList&) = delete;
  SkipList(SkipList&&) = delete;
  SkipList& operator=(SkipList&&) = delete;

  // Writer only. Precondition: no key equal to `key` is in the list.
  void Insert(const Key& key);

  // Safe to call concurrently with Insert.
  [[nodiscard]] bool Contains(const Key& key) const;

  // A forward cursor. Safe to use concurrently with Insert: it sees every key
  // inserted before it was positioned, and may or may not see keys inserted
  // while it moves. Copyable; holds no resources.
  class Iterator {
   public:
    // Starts not Valid(); position it with Seek or SeekToFirst.
    explicit Iterator(const SkipList* list) noexcept : list_(list) {}

    [[nodiscard]] bool Valid() const noexcept { return node_ != nullptr; }

    // Precondition: Valid().
    [[nodiscard]] const Key& key() const noexcept {
      FORGEDB_DCHECK(Valid());
      return node_->key;
    }

    // Precondition: Valid().
    void Next() noexcept {
      FORGEDB_DCHECK(Valid());
      node_ = node_->Next(0);
    }

    // Positions at the first key >= target (not Valid() if there is none).
    void Seek(const Key& target) { node_ = list_->FindGreaterOrEqual(target, nullptr); }

    void SeekToFirst() noexcept { node_ = list_->head_->Next(0); }

   private:
    const SkipList* list_;
    Node* node_ = nullptr;
  };

 private:
  using Prev = std::array<Node*, kMaxHeight>;

  [[nodiscard]] int MaxHeight() const noexcept {
    return max_height_.load(std::memory_order_relaxed);
  }
  [[nodiscard]] int RandomHeight();
  [[nodiscard]] Node* NewNode(const Key& key, int height);
  [[nodiscard]] bool Equal(const Key& a, const Key& b) const { return std::is_eq(compare_(a, b)); }
  // True if `key` sorts after `node`'s key; nullptr is treated as +infinity.
  [[nodiscard]] bool KeyIsAfterNode(const Key& key, Node* node) const {
    return node != nullptr && std::is_lt(compare_(node->key, key));
  }
  // The first node with a key >= `key`, or nullptr. If `prev` is non-null,
  // fills (*prev)[level] with the last node before `key` on every level
  // below MaxHeight().
  [[nodiscard]] Node* FindGreaterOrEqual(const Key& key, Prev* prev) const;

  Comparator compare_;
  Arena* arena_;
  Node* head_;  // a sentinel of height kMaxHeight; its key is never compared
  std::atomic<int> max_height_{1};
  std::minstd_rand rng_;  // writer only
};

// ---------------------------------------------------------------------------
// Node
//
// One arena allocation: the Node object (just the key), immediately followed
// by an array of `height` std::atomic<Node*>, created by NewNode with array
// placement-new. Tower() finds that array from the node's address and uses
// std::launder to obtain a pointer to it, since the compiler cannot know an
// object of that type lives there. This is the "trailing objects" idiom
// (LLVM's TrailingObjects uses it throughout Clang). The standard's wording
// on arithmetic over a char* to an object's bytes has a known gap (paper
// P1839), but every compiler gives it the obvious meaning. The fully
// by-the-book alternative is a stored tower pointer, at +8 bytes per node.
// ---------------------------------------------------------------------------
template <typename Key, class Comparator>
struct alignas(std::atomic<void*>) SkipList<Key, Comparator>::Node {
  explicit Node(const Key& k) noexcept : key(k) {}

  Key key;  // set once, before the node is published; never modified after

  // Acquire: see the node behind the pointer as it was when published.
  [[nodiscard]] Node* Next(int level) noexcept {
    return Tower()[level].load(std::memory_order_acquire);
  }
  // Release: publish everything written to `node` before this call.
  void SetNext(int level, Node* node) noexcept {
    Tower()[level].store(node, std::memory_order_release);
  }
  // Relaxed: only while this node is not yet reachable by readers (Insert),
  // or for reads by the writer itself.
  [[nodiscard]] Node* NoBarrierNext(int level) noexcept {
    return Tower()[level].load(std::memory_order_relaxed);
  }
  void NoBarrierSetNext(int level, Node* node) noexcept {
    Tower()[level].store(node, std::memory_order_relaxed);
  }

  [[nodiscard]] std::atomic<Node*>* Tower() noexcept {
    // reinterpret_cast to char* is C++'s way to address an object's bytes; the
    // second cast is made safe by std::launder (see the comment above).
    // NOLINTBEGIN(cppcoreguidelines-pro-type-reinterpret-cast)
    char* const bytes = reinterpret_cast<char*>(this);
    return std::launder(reinterpret_cast<std::atomic<Node*>*>(bytes + sizeof(Node)));
    // NOLINTEND(cppcoreguidelines-pro-type-reinterpret-cast)
  }
};

// ---------------------------------------------------------------------------
// SkipList
// ---------------------------------------------------------------------------
template <typename Key, class Comparator>
SkipList<Key, Comparator>::SkipList(Comparator compare, Arena* arena, std::uint32_t seed)
    : compare_(compare),
      arena_(arena),
      head_(NewNode(Key{}, kMaxHeight)),
      // minstd_rand rejects a zero seed; any other value is fine.
      rng_(seed == 0 ? 1U : seed) {}

template <typename Key, class Comparator>
int SkipList<Key, Comparator>::RandomHeight() {
  // minstd_rand is a multiplicative generator modulo a prime (2^31 - 1), so
  // its low bits are as random as its high bits and `% kBranching` is fine.
  int height = 1;
  while (height < kMaxHeight && rng_() % kBranching == 0) {
    ++height;
  }
  return height;
}

// Trailing return types: `Node` is looked up in the class scope there, so no
// `typename SkipList<Key, Comparator>::` prefix is needed.
template <typename Key, class Comparator>
auto SkipList<Key, Comparator>::NewNode(const Key& key, int height) -> Node* {
  static_assert(sizeof(Node) % alignof(std::atomic<Node*>) == 0,
                "the tower must start right after the node, correctly aligned");
  static_assert(std::is_trivially_destructible_v<std::atomic<Node*>>,
                "tower elements are never destroyed");
  const auto levels = static_cast<std::size_t>(height);
  // False positive: placement-new below needs a pointer to non-const memory.
  // NOLINTNEXTLINE(misc-const-correctness)
  char* const memory =
      arena_->AllocateAligned(sizeof(Node) + (levels * sizeof(std::atomic<Node*>)), alignof(Node));
  // owning-memory treats every `new` as creating an owner that must be
  // deleted. These are placement-news into arena memory: the arena owns the
  // bytes and frees them wholesale, and the objects are never destroyed
  // (both types are trivially destructible), so there is no owner to track.
  // NOLINTBEGIN(cppcoreguidelines-owning-memory)
  Node* const node = ::new (memory) Node(key);
  // Non-allocating array placement-new has no size overhead (C++20, CWG 2382),
  // so the array starts exactly at `memory + sizeof(Node)`; checked below.
  // `{}` value-initializes every pointer to nullptr.
  const auto* const tower = ::new (memory + sizeof(Node)) std::atomic<Node*>[levels] {};
  // NOLINTEND(cppcoreguidelines-owning-memory)
  FORGEDB_DCHECK(tower == node->Tower());
  static_cast<void>(tower);
  return node;
}

template <typename Key, class Comparator>
auto SkipList<Key, Comparator>::FindGreaterOrEqual(const Key& key, Prev* prev) const -> Node* {
  Node* node = head_;
  int level = MaxHeight() - 1;
  while (true) {
    Node* const next = node->Next(level);
    if (KeyIsAfterNode(key, next)) {
      node = next;  // keep going right on this level
    } else {
      if (prev != nullptr) {
        (*prev)[static_cast<std::size_t>(level)] = node;
      }
      if (level == 0) {
        return next;
      }
      --level;  // drop down a level
    }
  }
}

template <typename Key, class Comparator>
void SkipList<Key, Comparator>::Insert(const Key& key) {
  Prev prev{};
  const Node* const successor = FindGreaterOrEqual(key, &prev);
  FORGEDB_DCHECK(successor == nullptr || !Equal(key, successor->key));
  static_cast<void>(successor);

  const int height = RandomHeight();
  if (const int max_height = MaxHeight(); height > max_height) {
    for (int level = max_height; level < height; ++level) {
      prev[static_cast<std::size_t>(level)] = head_;
    }
    // Relaxed: see "Memory ordering" at the top of this file. Readers that see
    // the new height before the links below exist find nullptr in head_ on
    // the new levels and simply drop down.
    max_height_.store(height, std::memory_order_relaxed);
  }

  Node* const node = NewNode(key, height);
  for (int level = 0; level < height; ++level) {
    Node* const before = prev[static_cast<std::size_t>(level)];
    // Relaxed is enough for both: `node` is unreachable on this level until
    // the release store below, which also publishes this write.
    node->NoBarrierSetNext(level, before->NoBarrierNext(level));
    before->SetNext(level, node);
  }
}

template <typename Key, class Comparator>
bool SkipList<Key, Comparator>::Contains(const Key& key) const {
  const Node* const node = FindGreaterOrEqual(key, nullptr);
  return node != nullptr && Equal(key, node->key);
}

}  // namespace forgedb
