// DBImpl: the DB implementation: a memtable and a write-ahead log, flushed by
// a background thread to table files recorded in a MANIFEST; snapshots.
//
// Write path (group commit)
//   Every Write queues a Writer under mutex_ and waits. The writer at the
//   front of the queue is the leader. It joins its batch with the batches
//   queued behind it (up to a size cap) into one group, numbers the group
//   with the next sequence numbers, and then, with mutex_ released, appends
//   the group to the log as one record, syncs the log once if any member
//   asked for sync, and applies the group to the memtable. Back under mutex_
//   it publishes the group (last_sequence_), hands every member the shared
//   status, and wakes the next writer in line to lead.
//
//   Only the leader touches the log, group_batch_, the memtable's write side
//   and the VersionSet, and leadership passes under mutex_, so there is
//   always one writer. Readers never wait for it: they read at
//   last_sequence_, and entries above it are invisible, so a group appears
//   all at once.
//
// Flush (Phase 3.5; in the background since 4.2)
//   Before writing, a leader that finds the memtable at memtable_bytes or
//   more switches: it syncs the current log (so a crash can only damage the
//   newest log, and recovery still yields a prefix of the writes), starts a
//   new log, turns the memtable into the immutable memtable (imm_), hands it
//   to the background thread and goes on writing. If imm_ is still there
//   from the last switch (its flush is still running), the leader waits for
//   it first: a stall, counted in stalls_.
//
//   The background thread writes imm_ to a new level-0 table, syncs it,
//   records it in the MANIFEST (synced), installs the new Version and drops
//   imm_. The old log is deleted one flush later, once another MANIFEST
//   record follows the one that retired it (ADR 0007: a damaged last record
//   is ignored, so the data it records must still be in a log). Readers keep
//   reading imm_ until the table is installed.
//
//   Only the background thread uses the VersionSet after the open, except
//   that the leader takes the new log's file number when it switches, which
//   it does only while imm_ is empty, i.e. while no flush is running.
//
// Shutdown
//   The destructor tells the background thread to stop and joins it. A flush
//   that is running is finished; one that has not started is not: its writes
//   are in the logs (the old one synced at the switch), which the next open
//   replays.
//
// Reads
//   A read takes, under mutex_, the memtable, imm_, the current Version and
//   last_sequence_, then works without the lock. Get looks in the memtable,
//   then imm_, then the level-0 tables newest first. An iterator merges all
//   of them (NewMergingIterator) under the DB iterator, and holds the Version.
//
// Errors
//   If appending, syncing or flushing fails, what is on disk is unknown.
//   Every member of the group gets the error, and it becomes the sticky
//   background_error_: every later write fails with it, including a writer
//   waiting for a flush. Reads go on. Nothing is lost: a log is deleted only
//   after the MANIFEST records its contents.
//
// Snapshots (Phase 4.1)
//   A snapshot is a sequence number registered in snapshots_; a read with
//   one uses it instead of last_sequence_.
//
// Open and recovery (Phase 3.6)
//   Open locks <db>/LOCK for the DB's lifetime and recovers the MANIFEST to
//   trust (VersionSet::Recover, ADR 0007). It replays the logs numbered at or
//   above the MANIFEST's log number, in order, checking that sequence numbers
//   continue exactly; every log but the newest is read strictly, the newest
//   stops at its first problem (ADR 0004, rule 6). Whatever was recovered is
//   written to a level-0 table, a new log is started, and a new MANIFEST
//   (with CURRENT switched to it) records it all. Then files the new
//   MANIFEST does not need are deleted: older logs and MANIFESTs, tables it
//   does not list, CURRENT.tmp. A database from Phase 2 (logs, no MANIFEST)
//   is recovered the same way, from all of its logs.
#pragma once

#include <condition_variable>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "forgedb/db.h"

#include "db/internal_key.h"
#include "db/snapshot.h"
#include "db/table_cache.h"
#include "db/version_edit.h"
#include "db/version_set.h"
#include "env/file_system.h"
#include "log/log_writer.h"
#include "memtable/memtable.h"
#include "table/table.h"

namespace forgedb {

// What recovery found when the database was opened. For tests and tools.
struct RecoveryInfo {
  struct TornTail {
    std::uint64_t log_number = 0;
    std::uint64_t offset = 0;  // where the ignored tail began in that log
    std::string reason;
    // The newest log only: valid records followed the problem and were
    // dropped (point-in-time recovery, ADR 0004 rule 6).
    bool valid_data_dropped = false;
  };
  ManifestRecoveryInfo manifest;
  std::vector<std::uint64_t> logs_replayed;  // in replay order
  std::uint64_t records_replayed = 0;
  std::vector<TornTail> torn_tails;
  std::optional<std::uint64_t> table_written;  // the recovered data, as a level-0 table
  std::uint64_t new_log_number = 0;            // the log this session writes
  std::uint64_t new_manifest_number = 0;
  std::vector<std::string> files_deleted;  // obsolete files removed after the open
};

// Opens a database on any FileSystem: DB::Open uses the Windows one; tests
// use MemFileSystem or the fault-injection file system. The DB keeps the file
// system alive.
[[nodiscard]] Result<std::unique_ptr<DB>> OpenDB(const Options& options,
                                                 const std::filesystem::path& name,
                                                 std::shared_ptr<FileSystem> file_system);

// InvalidArgument naming the first field of `options` out of range.
[[nodiscard]] Status ValidateOptions(const Options& options);

class DBImpl final : public DB {
 public:
  // Use OpenDB.
  DBImpl(const Options& options, std::filesystem::path name,
         std::shared_ptr<FileSystem> file_system);
  ~DBImpl() override;
  DBImpl(const DBImpl&) = delete;
  DBImpl& operator=(const DBImpl&) = delete;
  DBImpl(DBImpl&&) = delete;
  DBImpl& operator=(DBImpl&&) = delete;

  // Everything Open does after construction (see the comment at the top).
  [[nodiscard]] Status Recover();

  [[nodiscard]] Status Write(const WriteOptions& options, const WriteBatch& batch) override;
  [[nodiscard]] Result<std::string> Get(const ReadOptions& options, std::string_view key) override;
  [[nodiscard]] std::unique_ptr<Iterator> NewIterator(const ReadOptions& options) override;
  [[nodiscard]] std::shared_ptr<const Snapshot> GetSnapshot() override;

  // For tools (forgedb_cli stats) and tests.
  [[nodiscard]] SequenceNumber LastSequence();
  [[nodiscard]] std::shared_ptr<const Version> CurrentVersion();
  [[nodiscard]] const RecoveryInfo& GetRecoveryInfo() const noexcept { return recovery_; }

  // Times a writer had to wait for a flush to finish, and for how long.
  struct Stalls {
    std::uint64_t count = 0;
    std::uint64_t microseconds = 0;
  };
  [[nodiscard]] Stalls GetStalls();

  // For tests.
  [[nodiscard]] std::uint64_t GroupsWrittenForTesting();
  [[nodiscard]] std::uint64_t FlushesForTesting();
  [[nodiscard]] std::optional<SequenceNumber> OldestSnapshotForTesting() const;
  // While on, a write that switches memtables waits for that flush to finish,
  // so the file operations of a run come in the same order every time (the
  // step-by-step crash tests need that).
  void SetSynchronousFlushForTesting(bool on);
  // Waits until no flush is queued or running (or the background error is set).
  void WaitForFlushForTesting();
  // Whether a flush is queued or running (imm_ is set).
  [[nodiscard]] bool FlushPendingForTesting();

 private:
  // A Write waiting in the queue.
  struct Writer {
    Writer(const WriteBatch* writer_batch, bool writer_sync) noexcept
        : batch(writer_batch), sync(writer_sync) {}
    const WriteBatch* batch;
    bool sync;
    bool done = false;  // set by the leader that wrote this writer's batch
    Status status;      // valid once done
    std::condition_variable cv;
  };

  // What a read needs, taken under mutex_.
  struct ReadView {
    MemTableRef memtable;
    MemTableRef immutable;  // may be empty
    std::shared_ptr<const Version> version;
    SequenceNumber sequence = 0;  // the snapshot's, or last_sequence_
  };
  // InvalidArgument if options.snapshot is not one of this DB's.
  [[nodiscard]] Result<ReadView> CurrentReadView(const ReadOptions& options);

  // A flush handed to the background thread.
  struct FlushJob {
    MemTableRef memtable;                // imm_
    SequenceNumber flushed_through = 0;  // its last sequence number
    std::uint64_t new_log = 0;           // the first log recovery will need
    std::uint64_t old_log = 0;           // the log it replaces
  };

  // Leader only, mutex_ held. Joins the batches of the writers at the front
  // of the queue into group_batch_; returns the last writer included and
  // whether the group must be synced.
  struct Group {
    Writer* last = nullptr;
    bool sync = false;
  };
  [[nodiscard]] Group BuildGroup();

  // Leader only, mutex_ held via `lock` (released around I/O and while
  // waiting). Switches memtables if the memtable is full, waiting for the
  // previous flush first (see "Flush" at the top). Returns
  // background_error_ if it is set, or sets it if switching fails.
  [[nodiscard]] Status MakeRoomForWrite(std::unique_lock<std::mutex>& lock);
  [[nodiscard]] Status SwitchMemtable(std::unique_lock<std::mutex>& lock);

  // The background thread's loop, and one flush (mutex_ not held).
  void BackgroundThread();
  [[nodiscard]] Status RunFlush(const FlushJob& job);

  // Writes `memtable` to a new level-0 table, synced and closed, and adds it
  // to `edit`. Writes nothing for an empty memtable. Open or background
  // thread only.
  [[nodiscard]] Status WriteLevel0Table(MemTable& memtable, VersionEdit& edit);

  // Replays log `number` into `memtable`, continuing from `last_sequence`.
  [[nodiscard]] Status ReplayLog(std::uint64_t number, bool newest, MemTable& memtable,
                                 SequenceNumber& last_sequence);

  // Removes files the current MANIFEST does not need (open only).
  void DeleteObsoleteFiles();

  // Fixed at construction.
  const Options options_;
  const std::filesystem::path name_;
  const std::shared_ptr<FileSystem> file_system_;
  // Declared before the table cache, which must be destroyed first.
  std::unique_ptr<BlockCache> block_cache_;  // null if options.block_cache_bytes == 0
  std::unique_ptr<TableCache> table_cache_;

  std::unique_ptr<FileLock> lock_;
  RecoveryInfo recovery_;  // set once by Recover()

  std::mutex mutex_;
  std::deque<Writer*> writers_;             // guarded by mutex_
  Status background_error_;                 // guarded by mutex_; sticky
  MemTableRef mem_;                         // guarded by mutex_
  MemTableRef imm_;                         // guarded by mutex_; set while flushing
  std::shared_ptr<const Version> version_;  // guarded by mutex_
  SequenceNumber last_sequence_ = 0;        // guarded by mutex_
  std::uint64_t groups_written_ = 0;        // guarded by mutex_
  std::uint64_t flushes_ = 0;               // guarded by mutex_
  Stalls stalls_;                           // guarded by mutex_
  std::optional<FlushJob> pending_flush_;   // guarded by mutex_; for the background thread
  bool shutting_down_ = false;              // guarded by mutex_
  bool synchronous_flush_ = false;          // guarded by mutex_
  // The log the last flush retired, kept until the next MANIFEST record.
  std::optional<std::uint64_t> retired_log_;  // guarded by mutex_
  std::condition_variable work_cv_;           // wakes the background thread
  std::condition_variable done_cv_;           // a flush finished or failed: wakes waiting writers
  const std::shared_ptr<SnapshotList> snapshots_ = std::make_shared<SnapshotList>();

  // Used by Recover(), then by the background thread (see "Flush").
  std::unique_ptr<VersionSet> versions_;
  // Used only by Recover() and then by the current leader.
  std::unique_ptr<WritableFile> log_file_;
  std::unique_ptr<log::Writer> log_;
  std::uint64_t log_number_ = 0;
  std::uint64_t mem_entries_ = 0;  // entries in mem_ (a group adds its count)
  WriteBatch group_batch_;

  // Started by Recover(), joined by the destructor.
  std::thread background_thread_;
};

}  // namespace forgedb
