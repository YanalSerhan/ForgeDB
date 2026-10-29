// An iterator over one level below level 0 (TODO 4.3): its tables are sorted
// and do not overlap, so it reads them one after another, opening each only
// when it gets there (a "concatenating" iterator), instead of opening every
// table of the level at once.
//
// It keeps the Version alive (and so its tables listed). The first error
// stops it for good (status()), so a damaged table is never skipped.
#pragma once

#include <memory>

#include "forgedb/iterator.h"

#include "db/table_cache.h"
#include "db/version_set.h"

namespace forgedb {

// Precondition: 1 <= level < kNumLevels. `table_cache` must outlive the iterator.
[[nodiscard]] std::unique_ptr<Iterator> NewLevelIterator(TableCache* table_cache,
                                                         std::shared_ptr<const Version> version,
                                                         int level);

}  // namespace forgedb
