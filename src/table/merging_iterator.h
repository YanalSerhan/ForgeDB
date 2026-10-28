// A merging iterator: one ordered stream over several ordered iterators (the
// memtables and the table files), by a min-heap of the children's current
// keys. Positioning costs O(n log n) for n children, Next O(log n).
//
// Children must be ordered by `compare`, and no key may appear in two of them
// (true for internal keys: each write has its own sequence number); for equal
// keys the earlier child comes first.
//
// Forward-only. The first child error stops the merge for good: Valid()
// becomes false and status() returns that error, so a damaged table never
// silently drops out of the merged view.
#pragma once

#include <memory>
#include <vector>

#include "forgedb/iterator.h"
#include "forgedb/status.h"

#include "table/block_format.h"

namespace forgedb {

[[nodiscard]] std::unique_ptr<Iterator> NewMergingIterator(
    std::vector<std::unique_ptr<Iterator>> children, KeyCompare compare);

// An iterator with no entries whose status() is `status` (for a view that
// could not be built, e.g. a table that failed to open).
[[nodiscard]] std::unique_ptr<Iterator> NewErrorIterator(Status status);

}  // namespace forgedb
