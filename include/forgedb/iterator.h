// Iterator: a forward cursor over a database's keys, in ascending bytewise
// order, as returned by DB::NewIterator.
//
//   std::unique_ptr<forgedb::Iterator> it = db->NewIterator({});
//   for (it->Seek("user:"); it->Valid() && it->key().starts_with("user:"); it->Next()) {
//     Use(it->key(), it->value());
//   }
//   FORGEDB_RETURN_IF_ERROR(it->status());
//
// An iterator reads a consistent snapshot: the database as it was when the
// iterator was created. Later writes are not visible through it.
//
// Forward only: no Prev or SeekToLast (not needed so far; they can be added
// later without changing the on-disk formats).
// An iterator may be used by one thread at a time. Destroy every iterator
// before the DB it came from.
#pragma once

#include <string_view>

#include "forgedb/status.h"

namespace forgedb {

class Iterator {
 public:
  Iterator() = default;
  virtual ~Iterator();  // defined in the library, so the vtable lives there once
  Iterator(const Iterator&) = delete;
  Iterator& operator=(const Iterator&) = delete;
  Iterator(Iterator&&) = delete;
  Iterator& operator=(Iterator&&) = delete;

  // True if the iterator is positioned at a key. Starts false.
  [[nodiscard]] virtual bool Valid() const = 0;

  // Positions at the first key; not Valid() if the database is empty.
  virtual void SeekToFirst() = 0;

  // Positions at the first key >= target; not Valid() if there is none.
  virtual void Seek(std::string_view target) = 0;

  // Moves to the next key. Precondition: Valid().
  virtual void Next() = 0;

  // The current key and value. Precondition: Valid(). The views are valid
  // until the iterator moves or is destroyed.
  [[nodiscard]] virtual std::string_view key() const = 0;
  [[nodiscard]] virtual std::string_view value() const = 0;

  // OK unless the iterator hit an error (e.g. a corrupt block, from Phase 3).
  // An iterator that stops being Valid() because of an error says so here.
  [[nodiscard]] virtual Status status() const = 0;
};

}  // namespace forgedb
