// The DB iterator: turns the memtable's stream of internal entries (every
// version of every key, newest first per key) into the user's view at one
// sequence number: each live key once, with its newest visible value.
//
// Rules, for the entries of one user key in order (newest first):
//   - entries newer than the iterator's sequence number are invisible;
//   - the first visible entry decides: a value is the key's value, a deletion
//     hides the key;
//   - every older entry of that key is skipped.
//
// Skipping is normally a step at a time. But Phase 1 never flushes the
// memtable, so a frequently written key can have thousands of versions, and
// stepping over all of them would make Next() and Seek() cost O(versions).
// After kMaxSequentialSkip consecutive skipped entries, the iterator re-seeks
// straight past the rest (O(log n)). RocksDB does the same
// (max_sequential_skip_in_iterations, default 8).
#pragma once

#include <cstddef>
#include <memory>

#include "forgedb/iterator.h"

#include "db/internal_key.h"
#include "memtable/memtable.h"

namespace forgedb {

inline constexpr std::size_t kMaxSequentialSkip = 8;

// An iterator over `memtable` as of `sequence`. Holds a reference, so the
// memtable stays alive as long as the iterator.
[[nodiscard]] std::unique_ptr<Iterator> NewDBIterator(MemTableRef memtable,
                                                      SequenceNumber sequence);

}  // namespace forgedb
