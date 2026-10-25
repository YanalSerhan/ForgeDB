// DBImpl: the DB implementation. Phase 2.2: one memtable and a write-ahead
// log, written with group commit.
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
//   Only the leader touches the log, group_batch_ and the memtable's write
//   side, and leadership passes under mutex_, so there is always one writer.
//   Readers never wait for it: they read at last_sequence_, and entries above
//   it are invisible, so a group appears all at once.
//
// Errors
//   If appending or syncing fails, the log's contents on disk are unknown.
//   The group is not applied, every member gets the error, and it becomes the
//   sticky background_error_: every later write fails with it. Reads go on.
//   (Such a write may still be in the log and reappear after recovery: like
//   any write that returned an error, its outcome is unknown.)
//
// Open and recovery (Phase 2.3)
//   OpenDB locks <db>/LOCK for the DB's lifetime, replays every log in the
//   directory in number order into a fresh memtable, checks that sequence
//   numbers continue exactly from record to record and from log to log,
//   restores the last sequence number, syncs every replayed log (what was
//   recovered must not be lost once newer writes depend on it; Phase 2.5),
//   and starts a new log numbered one above the highest. A torn tail ends a
//   log quietly; mid-log corruption, a malformed batch or a sequence gap fails
//   the open (ADR 0004). Phase 2 never deletes logs: their contents exist
//   nowhere else until Phase 3 flushes memtables to table files.
#pragma once

#include <condition_variable>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

#include "forgedb/db.h"

#include "db/internal_key.h"
#include "env/file_system.h"
#include "log/log_writer.h"
#include "memtable/memtable.h"

namespace forgedb {

// What recovery found when the database was opened. For tests and diagnostics.
struct RecoveryInfo {
  struct TornTail {
    std::uint64_t log_number = 0;
    std::uint64_t offset = 0;  // where the ignored tail began in that log
    std::string reason;
  };
  std::vector<std::uint64_t> logs_replayed;  // in replay order
  std::uint64_t records_replayed = 0;
  std::vector<TornTail> torn_tails;
  std::uint64_t new_log_number = 0;  // the log this session writes
};

// Opens a database on any FileSystem: DB::Open uses the Windows one; tests
// use MemFileSystem or, from Phase 2.4, the fault-injection file system. The
// DB keeps the file system alive.
[[nodiscard]] Result<std::unique_ptr<DB>> OpenDB(const Options& options,
                                                 const std::filesystem::path& name,
                                                 std::shared_ptr<FileSystem> file_system);

class DBImpl final : public DB {
 public:
  // Everything OpenDB prepares for a DBImpl.
  struct OpenedState {
    std::shared_ptr<FileSystem> file_system;
    std::unique_ptr<FileLock> lock;          // <db>/LOCK, held until destruction
    MemTableRef memtable;                    // holds the replayed logs
    SequenceNumber last_sequence = 0;        // of the last replayed write
    std::unique_ptr<WritableFile> log_file;  // the new, empty log
    RecoveryInfo recovery;
  };

  // Use OpenDB.
  explicit DBImpl(OpenedState state);
  ~DBImpl() override;
  DBImpl(const DBImpl&) = delete;
  DBImpl& operator=(const DBImpl&) = delete;
  DBImpl(DBImpl&&) = delete;
  DBImpl& operator=(DBImpl&&) = delete;

  [[nodiscard]] Status Write(const WriteOptions& options, const WriteBatch& batch) override;
  [[nodiscard]] Result<std::string> Get(const ReadOptions& options, std::string_view key) override;
  [[nodiscard]] std::unique_ptr<Iterator> NewIterator(const ReadOptions& options) override;

  // For tests.
  [[nodiscard]] SequenceNumber LastSequenceForTesting();
  [[nodiscard]] std::uint64_t GroupsWrittenForTesting();
  [[nodiscard]] const RecoveryInfo& RecoveryInfoForTesting() const noexcept { return recovery_; }

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

  // What a read needs: the memtable and the sequence number to read at.
  struct ReadView {
    MemTableRef memtable;
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

  std::shared_ptr<FileSystem> file_system_;  // kept alive for the files below
  std::unique_ptr<FileLock> lock_;
  RecoveryInfo recovery_;  // set once by the constructor

  std::mutex mutex_;
  std::deque<Writer*> writers_;       // guarded by mutex_
  Status background_error_;           // guarded by mutex_; sticky
  MemTableRef mem_;                   // guarded by mutex_
  SequenceNumber last_sequence_ = 0;  // guarded by mutex_; recovered, then advanced
  std::uint64_t groups_written_ = 0;  // guarded by mutex_

  // Used only by the current leader (see the comment at the top).
  std::unique_ptr<WritableFile> log_file_;
  log::Writer log_;
  WriteBatch group_batch_;
};

}  // namespace forgedb
