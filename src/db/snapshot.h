// The DB's side of snapshots (forgedb/snapshot.h).
//
// SnapshotList holds the sequence number of every live snapshot of one DB;
// compaction (Phase 4.3) asks it for the oldest, below which old versions of
// a key are no longer visible to anyone. The DB and each of its snapshots
// share ownership of the list, so a snapshot can outlive the DB.
#pragma once

#include <cstddef>
#include <memory>
#include <mutex>
#include <optional>
#include <set>

#include "forgedb/snapshot.h"

#include "db/internal_key.h"

namespace forgedb {

class SnapshotList {
 public:
  using Entry = std::multiset<SequenceNumber>::iterator;

  [[nodiscard]] Entry Add(SequenceNumber sequence);
  void Remove(Entry entry);

  // The oldest live snapshot's sequence number; nullopt if there is none.
  [[nodiscard]] std::optional<SequenceNumber> Oldest() const;
  [[nodiscard]] std::size_t Count() const;

 private:
  mutable std::mutex mutex_;
  std::multiset<SequenceNumber> sequences_;  // guarded by mutex_
};

class SnapshotImpl final : public Snapshot {
 public:
  // Registers `sequence` in `list` until destroyed.
  SnapshotImpl(std::shared_ptr<SnapshotList> list, SequenceNumber sequence);
  ~SnapshotImpl() override;
  SnapshotImpl(const SnapshotImpl&) = delete;
  SnapshotImpl& operator=(const SnapshotImpl&) = delete;
  SnapshotImpl(SnapshotImpl&&) = delete;
  SnapshotImpl& operator=(SnapshotImpl&&) = delete;

  [[nodiscard]] SequenceNumber Sequence() const noexcept { return sequence_; }
  // Which DB made it (its list).
  [[nodiscard]] const SnapshotList* List() const noexcept { return list_.get(); }

 private:
  std::shared_ptr<SnapshotList> list_;
  SequenceNumber sequence_;
  SnapshotList::Entry entry_;
};

}  // namespace forgedb
