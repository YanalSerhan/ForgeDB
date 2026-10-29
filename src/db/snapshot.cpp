#include "db/snapshot.h"

#include <cstddef>
#include <memory>
#include <mutex>
#include <optional>
#include <utility>

namespace forgedb {

Snapshot::~Snapshot() = default;

SnapshotList::Entry SnapshotList::Add(SequenceNumber sequence) {
  const std::scoped_lock lock(mutex_);
  return sequences_.insert(sequence);
}

void SnapshotList::Remove(const Entry& entry) {
  const std::scoped_lock lock(mutex_);
  sequences_.erase(entry);
}

std::optional<SequenceNumber> SnapshotList::Oldest() const {
  const std::scoped_lock lock(mutex_);
  if (sequences_.empty()) {
    return std::nullopt;
  }
  return *sequences_.begin();
}

std::size_t SnapshotList::Count() const {
  const std::scoped_lock lock(mutex_);
  return sequences_.size();
}

SnapshotImpl::SnapshotImpl(std::shared_ptr<SnapshotList> list, SequenceNumber sequence)
    : list_(std::move(list)), sequence_(sequence), entry_(list_->Add(sequence)) {}

SnapshotImpl::~SnapshotImpl() { list_->Remove(entry_); }

}  // namespace forgedb
