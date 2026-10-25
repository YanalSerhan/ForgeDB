#include "db/db_impl.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "forgedb/check.h"

#include "db/db_iter.h"
#include "db/filename.h"
#include "db/write_batch_internal.h"
#include "env/win_file_system.h"
#include "log/log_reader.h"

namespace forgedb {
namespace {

Status CheckKeySize(std::string_view key) {
  if (key.size() > kMaxKeySize) {
    return Status::InvalidArgument("key too large", std::to_string(key.size()) + " bytes, limit " +
                                                        std::to_string(kMaxKeySize));
  }
  return Status::OK();
}

Status CheckValueSize(std::string_view value) {
  if (value.size() > kMaxValueSize) {
    return Status::InvalidArgument(
        "value too large",
        std::to_string(value.size()) + " bytes, limit " + std::to_string(kMaxValueSize));
  }
  return Status::OK();
}

// Checks every key and value in a batch against the size limits.
class SizeChecker final : public WriteBatch::Handler {
 public:
  void Put(std::string_view key, std::string_view value) override {
    if (status_.ok()) {
      status_ = CheckKeySize(key);
    }
    if (status_.ok()) {
      status_ = CheckValueSize(value);
    }
  }
  void Delete(std::string_view key) override {
    if (status_.ok()) {
      status_ = CheckKeySize(key);
    }
  }
  [[nodiscard]] const Status& status() const noexcept { return status_; }

 private:
  Status status_;
};

// Group size cap: a leader adds followers' batches while the group stays
// under 1 MiB, but only 128 KiB beyond its own batch when that is small, so
// a small write isn't held up much by big ones queued behind it (LevelDB's
// rule).
constexpr std::size_t kMaxGroupBytes = std::size_t{1} << 20U;
constexpr std::size_t kSmallBatchBytes = std::size_t{128} << 10U;

}  // namespace

// --- Opening and recovery -----------------------------------------------------

namespace {

// Adds which log a corruption was found in; other errors already name their file.
Status InLog(const Status& status, const std::filesystem::path& log) {
  if (status.code() != Status::Code::kCorruption) {
    return status;
  }
  return Status::Corruption(log.filename().generic_string() + ": " + status.message());
}

// Replays one log into `memtable`. Every record must be a valid batch whose
// sequence number continues exactly where the previous one ended.
Status ReplayLog(FileSystem& fs, const std::filesystem::path& db, std::uint64_t number,
                 MemTable& memtable, SequenceNumber& last_sequence, RecoveryInfo& info) {
  const std::filesystem::path path = LogFileName(db, number);
  FORGEDB_ASSIGN_OR_RETURN(std::unique_ptr<SequentialFile> file, fs.NewSequentialFile(path));
  log::Reader reader(std::move(file));
  WriteBatch batch;
  while (true) {
    const Result<std::optional<std::string_view>> record = reader.ReadRecord();
    if (!record.ok()) {
      return InLog(record.status(), path);
    }
    if (!record->has_value()) {
      break;
    }
    FORGEDB_RETURN_IF_ERROR(InLog(WriteBatchInternal::SetContents(batch, **record), path));
    const SequenceNumber sequence = WriteBatchInternal::Sequence(batch);
    if (sequence != last_sequence + 1) {
      return Status::Corruption(
          path.filename().generic_string() + ": sequence numbers are not contiguous",
          "record at offset " + std::to_string(reader.LastRecordOffset()) + " starts at " +
              std::to_string(sequence) + ", expected " + std::to_string(last_sequence + 1));
    }
    // Validates the batch and applies all of it or none.
    FORGEDB_RETURN_IF_ERROR(InLog(WriteBatchInternal::InsertInto(batch, memtable), path));
    last_sequence += batch.Count();
    ++info.records_replayed;
  }
  if (const std::optional<log::Reader::TornTail>& tail = reader.IgnoredTail()) {
    info.torn_tails.push_back(
        {.log_number = number, .offset = tail->offset, .reason = tail->reason});
  }
  info.logs_replayed.push_back(number);
  return Status::OK();
}

}  // namespace

Result<std::unique_ptr<DB>> OpenDB(const Options& /*options*/, const std::filesystem::path& name,
                                   std::shared_ptr<FileSystem> file_system) {
  if (name.empty()) {
    return Status::InvalidArgument("database name is empty");
  }
  FileSystem& fs = *file_system;
  FORGEDB_RETURN_IF_ERROR(fs.CreateDir(name));

  // One open DB per directory, across processes and within this one. The lock
  // is released automatically if anything below fails.
  Result<std::unique_ptr<FileLock>> lock = fs.LockFile(LockFileName(name));
  if (!lock.ok()) {
    if (lock.status().code() == Status::Code::kBusy) {
      return Status::Busy("database is already open (in another process or another DB object)",
                          name.generic_string());
    }
    return lock.status();
  }

  // Every log, oldest first. Names come from the directory: ParseFileName
  // accepts only exact log names, and everything else is left alone.
  FORGEDB_ASSIGN_OR_RETURN(const std::vector<std::string> children, fs.GetChildren(name));
  std::vector<std::uint64_t> logs;
  for (const std::string& child : children) {
    if (const std::optional<ParsedFileName> parsed = ParseFileName(child);
        parsed && parsed->type == FileType::kLog) {
      logs.push_back(parsed->number);
    }
  }
  std::ranges::sort(logs);

  DBImpl::OpenedState state;
  state.memtable = MemTable::Create();
  for (const std::uint64_t number : logs) {
    FORGEDB_RETURN_IF_ERROR(
        ReplayLog(fs, name, number, *state.memtable, state.last_sequence, state.recovery));
  }

  // New writes go to a new log, so a torn tail in the old one stays where it
  // is (and is ignored again on the next recovery).
  if (!logs.empty() && logs.back() == UINT64_MAX) {
    return Status::NotSupported("log numbers exhausted");
  }
  state.recovery.new_log_number = logs.empty() ? 1 : logs.back() + 1;
  FORGEDB_ASSIGN_OR_RETURN(state.log_file,
                           fs.NewWritableFile(LogFileName(name, state.recovery.new_log_number)));
  state.lock = std::move(lock).value();
  state.file_system = std::move(file_system);
  return std::unique_ptr<DB>(std::make_unique<DBImpl>(std::move(state)));
}

// --- Public interfaces (defined here so their vtables live in the library) ---

Iterator::~Iterator() = default;
DB::~DB() = default;

Result<std::unique_ptr<DB>> DB::Open(const Options& options, const std::filesystem::path& name) {
  return OpenDB(options, name, std::make_shared<WinFileSystem>());
}

Status DB::Put(const WriteOptions& options, std::string_view key, std::string_view value) {
  // Check before building the batch: WriteBatch::Put aborts on sizes it
  // cannot encode, and the caller should get a Status instead.
  FORGEDB_RETURN_IF_ERROR(CheckKeySize(key));
  FORGEDB_RETURN_IF_ERROR(CheckValueSize(value));
  WriteBatch batch;
  batch.Put(key, value);
  return Write(options, batch);
}

Status DB::Delete(const WriteOptions& options, std::string_view key) {
  FORGEDB_RETURN_IF_ERROR(CheckKeySize(key));
  WriteBatch batch;
  batch.Delete(key);
  return Write(options, batch);
}

// --- DBImpl ------------------------------------------------------------------

DBImpl::DBImpl(OpenedState state)
    : file_system_(std::move(state.file_system)),
      lock_(std::move(state.lock)),
      recovery_(std::move(state.recovery)),
      mem_(std::move(state.memtable)),
      last_sequence_(state.last_sequence),
      log_file_(std::move(state.log_file)),
      log_(log_file_.get()) {}

DBImpl::~DBImpl() {
  // Destroying a DB while writes are in flight is a caller bug.
  FORGEDB_DCHECK(writers_.empty());
  // The destructor can't report errors; every acknowledged write has already
  // been flushed (and synced, if asked). Close the log, then release LOCK.
  static_cast<void>(log_file_->Close());
  static_cast<void>(file_system_->UnlockFile(std::move(lock_)));
}

Status DBImpl::Write(const WriteOptions& options, const WriteBatch& batch) {
  // Validation needs no lock: the batch is the caller's, the limits are fixed.
  SizeChecker sizes;
  FORGEDB_RETURN_IF_ERROR(batch.Iterate(sizes));
  FORGEDB_RETURN_IF_ERROR(sizes.status());

  Writer self(&batch, options.sync);
  std::unique_lock lock(mutex_);
  writers_.push_back(&self);
  self.cv.wait(lock, [&] { return self.done || &self == writers_.front(); });
  if (self.done) {
    return self.status;  // a leader wrote our batch as part of its group
  }

  // We lead: write a group of batches starting with ours.
  Status status = background_error_;
  const Writer* last = &self;
  if (status.ok()) {
    const Group group = BuildGroup();
    last = group.last;
    const std::uint32_t count = group_batch_.Count();
    const SequenceNumber first = last_sequence_ + 1;
    if (count > 0 && (first > kMaxSequenceNumber || count - 1 > kMaxSequenceNumber - first)) {
      // 2^56 sequence numbers used up. Checked before touching the log, so
      // nothing is half-written and this need not be sticky.
      status = Status::NotSupported("sequence numbers exhausted");
    }
    WriteBatchInternal::SetSequence(group_batch_, first);
    MemTable* const memtable = &*mem_;

    // The slow part runs without the mutex, so readers and newly arriving
    // writers aren't blocked; we are still the only writer (queue front).
    lock.unlock();
    bool log_touched = false;
    if (status.ok()) {
      log_touched = true;
      status = log_.AddRecord(WriteBatchInternal::Contents(group_batch_));
    }
    if (status.ok() && group.sync) {
      status = log_file_->Sync();
    }
    if (status.ok()) {
      // Checks the sequence range and applies all records or none.
      status = WriteBatchInternal::InsertInto(group_batch_, *memtable);
    }
    lock.lock();

    if (status.ok()) {
      last_sequence_ = first + count - 1;  // publishes the whole group at once
      ++groups_written_;
    } else if (log_touched) {
      // The log on disk may now hold part of this record or all of it; the
      // memtable doesn't. Refuse every later write.
      background_error_ = status;
    }
  }

  // Hand every member of the group (front of the queue up to `last`) its
  // status, then wake the next writer in line to lead.
  while (true) {
    Writer* const member = writers_.front();
    writers_.pop_front();
    if (member != &self) {
      member->status = status;
      member->done = true;
      member->cv.notify_one();
    }
    if (member == last) {
      break;
    }
  }
  if (!writers_.empty()) {
    writers_.front()->cv.notify_one();
  }
  return status;
}

DBImpl::Group DBImpl::BuildGroup() {
  Writer* const leader = writers_.front();
  group_batch_ = *leader->batch;
  Group group{.last = leader, .sync = leader->sync};

  std::size_t size = leader->batch->ApproximateSize();
  const std::size_t max_size = size <= kSmallBatchBytes ? size + kSmallBatchBytes : kMaxGroupBytes;
  for (auto it = writers_.begin() + 1; it != writers_.end(); ++it) {
    const Writer* const writer = *it;
    size += writer->batch->ApproximateSize() - WriteBatchInternal::kHeaderSize;
    const std::uint64_t count = std::uint64_t{group_batch_.Count()} + writer->batch->Count();
    if (size > max_size || count > UINT32_MAX) {
      break;
    }
    WriteBatchInternal::Append(group_batch_, *writer->batch);
    group.sync = group.sync || writer->sync;
    group.last = *it;
  }
  return group;
}

SequenceNumber DBImpl::LastSequenceForTesting() {
  const std::scoped_lock lock(mutex_);
  return last_sequence_;
}

std::uint64_t DBImpl::GroupsWrittenForTesting() {
  const std::scoped_lock lock(mutex_);
  return groups_written_;
}

DBImpl::ReadView DBImpl::CurrentReadView() {
  const std::scoped_lock lock(mutex_);
  return {.memtable = mem_, .sequence = last_sequence_};
}

Result<std::string> DBImpl::Get(const ReadOptions& /*options*/, std::string_view key) {
  FORGEDB_RETURN_IF_ERROR(CheckKeySize(key));
  const ReadView view = CurrentReadView();
  std::string value;
  switch (view.memtable->Get(LookupKey(key, view.sequence), &value)) {
    case MemTable::GetResult::kFound:
      return value;
    case MemTable::GetResult::kDeleted:
    case MemTable::GetResult::kNotFound:
      break;
  }
  return Status::NotFound("key not found");
}

std::unique_ptr<Iterator> DBImpl::NewIterator(const ReadOptions& /*options*/) {
  ReadView view = CurrentReadView();
  return NewDBIterator(std::move(view.memtable), view.sequence);
}

}  // namespace forgedb
