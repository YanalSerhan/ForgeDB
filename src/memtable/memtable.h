// MemTable: the in-memory, sorted write buffer.
//
// Every write goes to the active memtable (after the write-ahead log, from
// Phase 2). When ApproximateMemoryUsage() passes the configured buffer size,
// the DB swaps in a fresh memtable and flushes the full one to a table file
// (Phase 3). Reads check the memtables before any table file.
//
// Each Add() stores one entry: the internal key (user key + sequence + type)
// and the value, encoded into one arena allocation, and inserts a pointer to
// it into a skip list ordered by internal key. Byte layout:
// docs/file-formats.md, "Memtable entry".
//
// Concurrency: the same contract as the skip list. Add() needs external
// synchronization (one writer at a time); Get() and ApproximateMemoryUsage()
// may run concurrently with it and with each other, without locks.
//
// Lifetime: reference counted. A memtable is created through Create(), which
// returns a MemTableRef; it is destroyed when the last reference goes away.
// That lets a reader keep a memtable alive (copy the DB's MemTableRef under
// the DB mutex, then read without the mutex) while the DB swaps it out.
#pragma once

#include <atomic>
#include <compare>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <utility>

#include "forgedb/iterator.h"

#include "db/internal_key.h"
#include "memtable/arena.h"
#include "memtable/skip_list.h"

namespace forgedb {

class MemTableRef;

class MemTable {
 public:
  enum class GetResult : std::uint8_t {
    kNotFound,  // no version of the key is visible at the lookup's sequence
    kFound,     // the newest visible version is a value; it was copied out
    kDeleted,   // the newest visible version is a deletion (tombstone)
  };

  // A new, empty memtable with one reference: the returned MemTableRef.
  [[nodiscard]] static MemTableRef Create();

  MemTable(const MemTable&) = delete;
  MemTable& operator=(const MemTable&) = delete;
  MemTable(MemTable&&) = delete;
  MemTable& operator=(MemTable&&) = delete;

  // Writer only. Adds a version of `user_key` written at `sequence`. Callers
  // never reuse a sequence number, so an entry is never added twice.
  // For a deletion, `value` is ignored by readers and should be empty.
  void Add(SequenceNumber sequence, ValueType type, std::string_view user_key,
           std::string_view value);

  // Looks up the newest version of key.UserKey() with a sequence number <=
  // the lookup's sequence. On kFound, assigns the value to *value; otherwise
  // leaves *value untouched.
  [[nodiscard]] GetResult Get(const LookupKey& key, std::string* value) const;

  // Bytes of memory the memtable holds (its arena). The flush trigger.
  [[nodiscard]] std::size_t ApproximateMemoryUsage() const noexcept { return arena_.MemoryUsage(); }

  // Manual reference counting. Prefer MemTableRef, which does this for you.
  // Unref() destroys the memtable when the count drops to zero.
  void Ref() noexcept;
  void Unref() noexcept;

  [[nodiscard]] int RefCountForTesting() const noexcept {
    return refs_.load(std::memory_order_relaxed);
  }

  // Walks every entry, every version of every key, in internal-key order
  // (user key ascending, newest version first). Defined below.
  class Iterator;

 private:
  // Orders skip-list entries (pointers to encoded entries) by internal key.
  struct KeyComparator {
    std::strong_ordering operator()(const char* a, const char* b) const noexcept;
  };
  using Table = SkipList<const char*, KeyComparator>;

  // Only Create() constructs and only Unref() destroys, so a memtable can't
  // be put on the stack or deleted while references to it remain.
  MemTable();
  ~MemTable() = default;

  std::atomic<int> refs_{0};
  Arena arena_;
  Table table_;  // declared after arena_: its nodes live in the arena
};

// An owning reference to a MemTable, like std::shared_ptr but intrusive (the
// count lives in the MemTable). Copying adds a reference, destruction or
// reassignment drops one. Empty when default-constructed or moved from.
class MemTableRef {
 public:
  MemTableRef() noexcept = default;
  // Adds a reference to `table` (which may be null).
  explicit MemTableRef(MemTable* table) noexcept : table_(table) {
    if (table_ != nullptr) {
      table_->Ref();
    }
  }
  ~MemTableRef() { Reset(); }

  MemTableRef(const MemTableRef& other) noexcept : MemTableRef(other.table_) {}
  MemTableRef(MemTableRef&& other) noexcept : table_(other.table_) { other.table_ = nullptr; }
  // Both assignments build the new reference first and swap it in, so they
  // are safe for self-assignment and the old reference is dropped last.
  MemTableRef& operator=(const MemTableRef& other) noexcept {
    MemTableRef copy(other);
    std::swap(table_, copy.table_);
    return *this;
  }
  MemTableRef& operator=(MemTableRef&& other) noexcept {
    MemTableRef moved(std::move(other));
    std::swap(table_, moved.table_);
    return *this;
  }

  // Drops this reference (if any) and becomes empty.
  void Reset() noexcept {
    if (table_ != nullptr) {
      std::exchange(table_, nullptr)->Unref();
    }
  }

  MemTable* operator->() const noexcept { return table_; }
  MemTable& operator*() const noexcept { return *table_; }
  explicit operator bool() const noexcept { return table_ != nullptr; }

 private:
  MemTable* table_ = nullptr;
};

// A forward cursor over a memtable's entries. Like the skip list's iterator
// it may run concurrently with Add(), and sees every entry added before it
// was positioned. It holds no reference: the caller keeps the memtable alive
// (the DB iterator holds a MemTableRef). Views returned by key() and value()
// stay valid as long as the memtable does.
class MemTable::Iterator {
 public:
  explicit Iterator(const MemTable* table) noexcept : it_(&table->table_) {}

  [[nodiscard]] bool Valid() const noexcept { return it_.Valid(); }
  void SeekToFirst() noexcept { it_.SeekToFirst(); }
  // Positions at the first entry whose internal key is >= `internal_key`.
  void Seek(std::string_view internal_key);
  // Precondition: Valid().
  void Next() noexcept { it_.Next(); }

  // The current entry's internal key and value. Precondition: Valid().
  [[nodiscard]] std::string_view key() const noexcept;
  [[nodiscard]] std::string_view value() const noexcept;

 private:
  Table::Iterator it_;
  std::string seek_buffer_;  // Seek's target, in memtable-key form
};

// Every entry of `memtable` (internal key -> value) as a forgedb::Iterator,
// which the merging iterator can combine with table iterators. Holds a
// reference, so the memtable lives as long as the iterator. Never fails.
[[nodiscard]] std::unique_ptr<Iterator> NewMemTableIterator(MemTableRef memtable);

}  // namespace forgedb
