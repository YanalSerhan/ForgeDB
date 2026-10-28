// The DB iterator: turns a stream of internal entries (every version of every
// key, newest first per key: the merge of the memtables and tables) into the
// user's view at one sequence number: each live key once, with its newest
// visible value.
//
// Rules, for the entries of one user key in order (newest first):
//   - entries newer than the iterator's sequence number are invisible;
//   - the first visible entry decides: a value is the key's value, a deletion
//     hides the key;
//   - every older entry of that key is skipped.
//
// Skipping is normally a step at a time. But a frequently written key can
// have thousands of versions (in the memtable, and in tables until
// compaction drops old ones), and stepping over all of them would make Next()
// and Seek() cost O(versions).
// After kMaxSequentialSkip consecutive skipped entries, the iterator re-seeks
// straight past the rest (O(log n)). RocksDB does the same
// (max_sequential_skip_in_iterations, default 8).
#pragma once

#include <cstddef>
#include <memory>

#include "forgedb/iterator.h"

#include "db/internal_key.h"

namespace forgedb {

inline constexpr std::size_t kMaxSequentialSkip = 8;

// The user's view of `entries` (internal keys, in order) as of `sequence`.
// `pin` (may be null) is held until the iterator is destroyed: whatever the
// entries read from that must outlive them (the DB passes its Version).
// A malformed internal key ends the iteration with Corruption.
[[nodiscard]] std::unique_ptr<Iterator> NewDBIterator(std::unique_ptr<Iterator> entries,
                                                      SequenceNumber sequence,
                                                      std::shared_ptr<const void> pin = nullptr);

}  // namespace forgedb
