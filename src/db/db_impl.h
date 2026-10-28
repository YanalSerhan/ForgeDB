// DBImpl: the DB implementation. Phase 3: a memtable and a write-ahead log,
// flushed to table files recorded in a MANIFEST.
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
// Flush (Phase 3.5)
//   Before writing, a leader that finds the memtable at memtable_bytes or
//   more switches: it syncs the current log (so a crash can only damage the
//   newest log, and recovery still yields a prefix of the writes), starts a
//   new log, and turns the memtable into the immutable memtable (imm_). Then,
//   with mutex_ released, it writes imm_ to a new level-0 table, syncs it,
//   records it in the MANIFEST (synced), installs the new Version and drops
//   imm_. The old log is deleted one flush later, once another MANIFEST
//   record follows the one that retired it (ADR 0007: a damaged last record
//   is ignored, so the data it records must still be in a log). Writers wait
//   meanwhile (until a background thread takes over, Phase 4.2); readers
//   keep reading imm_ until the table is installed.
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
//   background_error_: every later write fails with it. Reads go on. Nothing
//   is lost: a log is deleted only after the MANIFEST records its contents.
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
#include <vector>

#include "forgedb/db.h"

#include "db/internal_key.h"
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

  // For tools (forgedb_cli stats) and tests.
  [[nodiscard]] SequenceNumber LastSequence();
  [[nodiscard]] std::shared_ptr<const Version> CurrentVersion();
  [[nodiscard]] const RecoveryInfo& GetRecoveryInfo() const noexcept { return recovery_; }

  // For tests.
  [[nodiscard]] std::uint64_t GroupsWrittenForTesting();
  [[nodiscard]] std::uint64_t FlushesForTesting();

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
    SequenceNumber sequence = 0;
  };
  [[nodiscard]] ReadView CurrentReadView();

  // Leader only, mutex_ held. Joins the batches of the writers at the front
  // of the queue into group_batch_; returns the last writer included and
  // whether the group must be synced.
  struct Group {
    Writer* last = nullptr;
    bool sync = false;
  };
  [[nodiscard]] Group BuildGroup();

  // Leader only, mutex_ held via `lock` (released around I/O). Flushes the
  // memtable if it is full (see "Flush" at the top). On error sets
  // background_error_ and returns it.
  [[nodiscard]] Status MakeRoomForWrite(std::unique_lock<std::mutex>& lock);

  // Writes `memtable` to a new level-0 table, synced and closed, and adds it
  // to `edit`. Writes nothing for an empty memtable. Open or leader only.
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

  // Used only by Recover() and then by the current leader.
  std::unique_ptr<VersionSet> versions_;
  std::unique_ptr<WritableFile> log_file_;
  std::unique_ptr<log::Writer> log_;
  std::uint64_t log_number_ = 0;
  // The log the last flush retired, kept until the next MANIFEST record.
  std::optional<std::uint64_t> retired_log_;
  std::uint64_t mem_entries_ = 0;  // entries in mem_ (a group adds its count)
  WriteBatch group_batch_;
};

}  // namespace forgedb
