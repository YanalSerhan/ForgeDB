// Snapshot: a point in time to read the database at.
//
//   std::shared_ptr<const forgedb::Snapshot> snapshot = db->GetSnapshot();
//   FORGEDB_RETURN_IF_ERROR(db->Put({}, "key", "new"));
//   forgedb::Result<std::string> old = db->Get({.snapshot = snapshot}, "key");  // the old value
//   snapshot.reset();  // released
//
// A read with ReadOptions::snapshot sees exactly the writes that were
// acknowledged before GetSnapshot() returned, whatever happens afterwards:
// later writes, flushes, and (from Phase 4.3) compactions, which keep every
// version of a key that a live snapshot can see.
//
// A snapshot is released when its last shared_ptr is dropped; it may outlive
// the DB that made it (it then holds nothing). It belongs to that DB: reading
// another DB with it is InvalidArgument. Snapshots are not saved: they end
// when the DB is closed.
#pragma once

namespace forgedb {

class Snapshot {
 public:
  virtual ~Snapshot();  // defined in the library, so the vtable lives there once
  Snapshot(const Snapshot&) = delete;
  Snapshot& operator=(const Snapshot&) = delete;
  Snapshot(Snapshot&&) = delete;
  Snapshot& operator=(Snapshot&&) = delete;

 protected:
  Snapshot() = default;  // made only by DB::GetSnapshot
};

}  // namespace forgedb
