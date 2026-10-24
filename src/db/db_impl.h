// DBImpl: the DB implementation. Phase 1: one memtable, memory only.
//
// Concurrency
//   mutex_ guards mem_ and last_sequence_ and serializes writers. A write
//   inserts its batch into the memtable with sequence numbers above
//   last_sequence_ and only then advances last_sequence_, all under mutex_.
//   A read copies mem_ (a reference) and last_sequence_ under mutex_, then
//   reads the memtable without the lock, at that sequence number. Entries
//   above it are invisible, so a reader sees each batch entirely or not at
//   all, and never waits for the memtable itself.
//   (Phase 2 replaces "writers hold the mutex while inserting" with group
//   commit and the write-ahead log.)
#pragma once

#include <memory>
#include <mutex>
#include <string>
#include <string_view>

#include "forgedb/db.h"

#include "db/internal_key.h"
#include "memtable/memtable.h"

namespace forgedb {

class DBImpl final : public DB {
 public:
  explicit DBImpl(const Options& options);

  [[nodiscard]] Status Write(const WriteOptions& options, const WriteBatch& batch) override;
  [[nodiscard]] Result<std::string> Get(const ReadOptions& options, std::string_view key) override;
  [[nodiscard]] std::unique_ptr<Iterator> NewIterator(const ReadOptions& options) override;

 private:
  // What a read needs: the memtable and the sequence number to read at.
  struct ReadView {
    MemTableRef memtable;
    SequenceNumber sequence = 0;
  };
  [[nodiscard]] ReadView CurrentReadView();

  std::mutex mutex_;
  MemTableRef mem_;                   // guarded by mutex_
  SequenceNumber last_sequence_ = 0;  // guarded by mutex_; writes start at 1
};

}  // namespace forgedb
