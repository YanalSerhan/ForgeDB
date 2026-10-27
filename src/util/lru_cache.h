// ShardedLruCache<T>: a thread-safe, size-bounded cache with least-recently-
// used eviction, used for the block cache (T = Block, charged in bytes) and
// the table cache (T = std::unique_ptr<Table>, charged 1 per table).
//
//   ShardedLruCache<Block> cache(8 << 20);
//   ShardedLruCache<Block>::Handle h = cache.Lookup(key);
//   if (!h) h = cache.Insert(key, std::move(block), block_size);
//   Use(*h);  // the entry stays alive while `h` exists
//
// Design (LevelDB's LRUCache, sharded):
//   - Keys are byte strings. MurmurHash3 of the key picks one of the shards;
//     each shard has its own mutex, hash map and lists, and capacity / shards
//     of the total capacity, so threads working on different keys rarely
//     contend.
//   - Every entry has a charge. A shard keeps the charges of its entries at or
//     below its capacity by evicting the least recently used entries.
//   - A Handle is a counted reference to one entry. An entry with handles is
//     "in use": it sits on a separate list, is never evicted and never freed.
//     If every entry is in use, a shard may stay over capacity until handles
//     are released. An entry erased or replaced while in use leaves the map at
//     once (later lookups miss) and is freed when its last handle goes away.
//   - Values are freed outside the shard's mutex.
//
// Every Handle must be destroyed before the cache. Values live in list nodes,
// so a value's address never changes while it is cached.
//
// Handles hold a plain pointer to their entry; list iterators are used only
// under the shard's mutex. (MSVC's debug-mode iterators register themselves
// with their list, and copying one in another thread while the list moves its
// node races with that bookkeeping.)
#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <list>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#include "forgedb/check.h"

#include "util/hash.h"

namespace forgedb {

inline constexpr std::size_t kDefaultCacheShards = 16;

struct CacheStats {
  std::uint64_t hits = 0;       // Lookups that found the key
  std::uint64_t misses = 0;     // Lookups that did not
  std::uint64_t evictions = 0;  // entries removed to make room (not Erase or replacement)
  std::size_t charge = 0;       // total charge of the cached entries
  std::size_t entries = 0;      // cached entries (not counting erased ones still in use)
};

template <typename T>
class ShardedLruCache {
  struct Entry;
  struct Shard;

 public:
  // A counted reference to a cached value; empty when default-constructed,
  // moved from, or returned by a Lookup that missed. Move-only.
  class Handle {
   public:
    Handle() noexcept = default;
    ~Handle() { Reset(); }
    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;
    Handle(Handle&& other) noexcept
        : shard_(std::exchange(other.shard_, nullptr)),
          entry_(std::exchange(other.entry_, nullptr)) {}
    Handle& operator=(Handle&& other) noexcept {
      if (this != &other) {
        Reset();
        shard_ = std::exchange(other.shard_, nullptr);
        entry_ = std::exchange(other.entry_, nullptr);
      }
      return *this;
    }

    explicit operator bool() const noexcept { return entry_ != nullptr; }

    // Precondition: not empty.
    [[nodiscard]] const T& operator*() const noexcept {
      FORGEDB_DCHECK(entry_ != nullptr);
      return entry_->value;
    }
    [[nodiscard]] const T* operator->() const noexcept { return &**this; }

    // Drops the reference (if any); the handle becomes empty.
    void Reset() noexcept {
      if (entry_ != nullptr) {
        shard_->Release(std::exchange(entry_, nullptr));
        shard_ = nullptr;
      }
    }

   private:
    friend class ShardedLruCache;
    Handle(Shard* shard, Entry* entry) noexcept : shard_(shard), entry_(entry) {}

    Shard* shard_ = nullptr;
    Entry* entry_ = nullptr;
  };

  // `capacity` is the total charge the cache holds when nothing is in use,
  // split evenly over `shards` shards (rounded up). Precondition: shards >= 1.
  explicit ShardedLruCache(std::size_t capacity, std::size_t shards = kDefaultCacheShards)
      : capacity_(capacity) {
    FORGEDB_CHECK(shards >= 1);
    shards_.reserve(shards);
    for (std::size_t i = 0; i < shards; ++i) {
      shards_.push_back(std::make_unique<Shard>());
      shards_.back()->capacity = (capacity + shards - 1) / shards;
    }
  }

  // Precondition: no Handle is alive (checked in debug builds).
  ~ShardedLruCache() {
    for ([[maybe_unused]] const std::unique_ptr<Shard>& shard : shards_) {
      FORGEDB_DCHECK(shard->in_use.empty() && shard->erased.empty());
    }
  }
  ShardedLruCache(const ShardedLruCache&) = delete;
  ShardedLruCache& operator=(const ShardedLruCache&) = delete;
  ShardedLruCache(ShardedLruCache&&) = delete;
  ShardedLruCache& operator=(ShardedLruCache&&) = delete;

  // Caches `value` under `key`, replacing any entry with that key, and returns
  // a handle to it. May evict least recently used entries.
  [[nodiscard]] Handle Insert(std::string_view key, T value, std::size_t charge) {
    Shard& shard = ShardFor(key);
    return Handle(&shard, shard.Insert(key, std::move(value), charge));
  }

  // A handle to the entry for `key`, or an empty handle.
  [[nodiscard]] Handle Lookup(std::string_view key) {
    Shard& shard = ShardFor(key);
    Entry* const entry = shard.Lookup(key);
    return entry != nullptr ? Handle(&shard, entry) : Handle();
  }

  // Removes the entry for `key`, if any. Handles to it stay valid.
  void Erase(std::string_view key) { ShardFor(key).Erase(key); }

  // A number never returned before by this cache: lets each client (e.g. each
  // open table) build keys that cannot collide with another client's.
  [[nodiscard]] std::uint64_t NewId() noexcept {
    return next_id_.fetch_add(1, std::memory_order_relaxed) + 1;
  }

  [[nodiscard]] std::size_t Capacity() const noexcept { return capacity_; }

  // Totals over all shards (each shard read under its own lock).
  [[nodiscard]] CacheStats GetStats() const {
    CacheStats total;
    for (const std::unique_ptr<Shard>& shard : shards_) {
      const std::scoped_lock lock(shard->mutex);
      total.hits += shard->stats.hits;
      total.misses += shard->stats.misses;
      total.evictions += shard->stats.evictions;
      total.charge += shard->usage;
      total.entries += shard->map.size();
    }
    return total;
  }

 private:
  enum class Where : std::uint8_t { kLru, kInUse, kErased };

  using EntryList = std::list<Entry>;
  using EntryIterator = EntryList::iterator;

  struct Entry {
    Entry(std::string_view key_in, T value_in, std::size_t charge_in)
        : key(key_in), value(std::move(value_in)), charge(charge_in) {}

    std::string key;
    T value;
    std::size_t charge;
    std::size_t refs = 0;  // live Handles
    Where where = Where::kInUse;
    EntryIterator self;  // this entry's node; used only under the shard's mutex
  };

  struct Shard {
    // Inserts and returns the new entry with one reference.
    Entry* Insert(std::string_view key, T value, std::size_t charge) {
      EntryList garbage;  // destroyed after the lock is released
      const std::scoped_lock lock(mutex);
      if (const auto it = map.find(key); it != map.end()) {
        Entry* const old = it->second;
        map.erase(it);
        Unlink(*old, garbage);
      }
      Entry& entry = in_use.emplace_back(key, std::move(value), charge);
      entry.self = std::prev(in_use.end());
      entry.refs = 1;
      map.emplace(entry.key, &entry);
      usage += charge;
      EvictWhileOverCapacity(garbage);
      return &entry;
    }

    Entry* Lookup(std::string_view key) {
      const std::scoped_lock lock(mutex);
      const auto it = map.find(key);
      if (it == map.end()) {
        ++stats.misses;
        return nullptr;
      }
      ++stats.hits;
      Entry& entry = *it->second;
      if (entry.refs++ == 0) {
        in_use.splice(in_use.end(), lru, entry.self);
        entry.where = Where::kInUse;
      }
      return &entry;
    }

    void Erase(std::string_view key) {
      EntryList garbage;
      const std::scoped_lock lock(mutex);
      if (const auto it = map.find(key); it != map.end()) {
        Entry* const entry = it->second;
        map.erase(it);
        Unlink(*entry, garbage);
      }
    }

    void Release(Entry* entry) noexcept {
      EntryList garbage;
      const std::scoped_lock lock(mutex);
      FORGEDB_DCHECK(entry->refs > 0);
      if (--entry->refs > 0) {
        return;
      }
      if (entry->where == Where::kErased) {
        garbage.splice(garbage.end(), erased, entry->self);
        return;
      }
      lru.splice(lru.end(), in_use, entry->self);  // most recently used at the back
      entry->where = Where::kLru;
      EvictWhileOverCapacity(garbage);
    }

    // Removes a cached entry (already out of the map) from the accounting: an
    // unused entry moves to `garbage`, an entry in use to `erased` until its
    // last handle goes.
    void Unlink(Entry& entry, EntryList& garbage) noexcept {
      usage -= entry.charge;
      if (entry.where == Where::kLru) {
        garbage.splice(garbage.end(), lru, entry.self);
      } else {
        erased.splice(erased.end(), in_use, entry.self);
        entry.where = Where::kErased;
      }
    }

    void EvictWhileOverCapacity(EntryList& garbage) noexcept {
      while (usage > capacity && !lru.empty()) {
        const Entry& oldest = lru.front();
        map.erase(oldest.key);
        usage -= oldest.charge;
        ++stats.evictions;
        garbage.splice(garbage.end(), lru, oldest.self);
      }
    }

    mutable std::mutex mutex;
    std::size_t capacity = 0;
    std::size_t usage = 0;
    EntryList lru;     // cached, no handles; front = least recently used
    EntryList in_use;  // cached, with handles
    EntryList erased;  // no longer cached, still with handles
    // Keys are views of the entries' own keys, which never move: list nodes
    // stay put when spliced between lists.
    std::unordered_map<std::string_view, Entry*> map;
    CacheStats stats;
  };

  Shard& ShardFor(std::string_view key) noexcept {
    return *shards_[MurmurHash3x64(key).low % shards_.size()];
  }

  std::size_t capacity_;
  std::vector<std::unique_ptr<Shard>> shards_;
  std::atomic<std::uint64_t> next_id_{0};
};

}  // namespace forgedb
