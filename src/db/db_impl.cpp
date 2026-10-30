#include "db/db_impl.h"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "forgedb/check.h"

#include "db/compaction.h"
#include "db/db_iter.h"
#include "db/filename.h"
#include "db/level_iterator.h"
#include "db/snapshot.h"
#include "db/version_edit.h"
#include "db/version_set.h"
#include "db/write_batch_internal.h"
#include "env/win_file_system.h"
#include "log/log_reader.h"
#include "table/merging_iterator.h"
#include "table/table_builder.h"

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
    bytes_ += key.size() + value.size();
  }
  void Delete(std::string_view key) override {
    if (status_.ok()) {
      status_ = CheckKeySize(key);
    }
    bytes_ += key.size();
  }
  [[nodiscard]] const Status& status() const noexcept { return status_; }
  // Keys plus values (DBStats::user_bytes).
  [[nodiscard]] std::uint64_t Bytes() const noexcept { return bytes_; }

 private:
  Status status_;
  std::uint64_t bytes_ = 0;
};

// Microseconds since `start`.
std::uint64_t MicrosecondsSince(std::chrono::steady_clock::time_point start) {
  return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(
                                        std::chrono::steady_clock::now() - start)
                                        .count());
}

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

}  // namespace

Status ValidateOptions(const Options& options) {
  if (options.memtable_bytes == 0) {
    return Status::InvalidArgument("Options::memtable_bytes must be at least 1");
  }
  if (options.max_open_tables == 0) {
    return Status::InvalidArgument("Options::max_open_tables must be at least 1");
  }
  if (options.filter_bits_per_key < 0 || options.filter_bits_per_key > 30) {
    return Status::InvalidArgument("Options::filter_bits_per_key must be 0 to 30",
                                   std::to_string(options.filter_bits_per_key));
  }
  if (options.block_size < 256 || options.block_size > (std::size_t{1} << 20)) {
    return Status::InvalidArgument("Options::block_size must be 256 B to 1 MiB",
                                   std::to_string(options.block_size));
  }
  if (options.level0_compaction_trigger < 1 ||
      options.level0_slowdown_writes_trigger < options.level0_compaction_trigger ||
      options.level0_stop_writes_trigger < options.level0_slowdown_writes_trigger) {
    return Status::InvalidArgument(
        "Options: need 1 <= level0_compaction_trigger <= level0_slowdown_writes_trigger <= "
        "level0_stop_writes_trigger",
        std::to_string(options.level0_compaction_trigger) + ", " +
            std::to_string(options.level0_slowdown_writes_trigger) + ", " +
            std::to_string(options.level0_stop_writes_trigger));
  }
  if (options.max_bytes_for_level_base == 0) {
    return Status::InvalidArgument("Options::max_bytes_for_level_base must be at least 1");
  }
  if (options.target_file_size == 0) {
    return Status::InvalidArgument("Options::target_file_size must be at least 1");
  }
  return Status::OK();
}

Result<std::unique_ptr<DB>> OpenDB(const Options& options, const std::filesystem::path& name,
                                   std::shared_ptr<FileSystem> file_system) {
  if (name.empty()) {
    return Status::InvalidArgument("database name is empty");
  }
  FORGEDB_RETURN_IF_ERROR(ValidateOptions(options));
  auto db = std::make_unique<DBImpl>(options, name, std::move(file_system));
  FORGEDB_RETURN_IF_ERROR(db->Recover());  // on error, the destructor releases what was taken
  return std::unique_ptr<DB>(std::move(db));
}

DBImpl::DBImpl(const Options& options, std::filesystem::path name,
               std::shared_ptr<FileSystem> file_system)
    : options_(options),
      name_(std::move(name)),
      file_system_(std::move(file_system)),
      block_cache_(options.block_cache_bytes > 0
                       ? std::make_unique<BlockCache>(options.block_cache_bytes)
                       : nullptr),
      table_cache_(std::make_unique<TableCache>(file_system_.get(), name_, options.max_open_tables,
                                                block_cache_.get())) {
  stats_.levels.resize(static_cast<std::size_t>(kNumLevels));
}

Status DBImpl::ReplayLog(std::uint64_t number, bool newest, MemTable& memtable,
                         SequenceNumber& last_sequence) {
  const std::filesystem::path path = LogFileName(name_, number);
  FORGEDB_ASSIGN_OR_RETURN(std::unique_ptr<SequentialFile> file,
                           file_system_->NewSequentialFile(path));
  // Only the newest log can hold writes that were never synced, and its
  // recovered prefix goes into a table below, so it is never replayed again:
  // it may end at its first problem (ADR 0004, rule 6). Older logs were
  // synced before a newer one started: damage there is corruption.
  log::Reader reader(std::move(file),
                     newest ? log::Reader::Mode::kPointInTime : log::Reader::Mode::kStrict);
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
    ++recovery_.records_replayed;
  }
  if (const std::optional<log::Reader::TornTail>& tail = reader.IgnoredTail()) {
    recovery_.torn_tails.push_back({.log_number = number,
                                    .offset = tail->offset,
                                    .reason = tail->reason,
                                    .valid_data_dropped = tail->valid_data_dropped});
  }
  recovery_.logs_replayed.push_back(number);
  return Status::OK();
}

namespace {

// A table being written by a flush or a compaction.
class TableOutput {
 public:
  TableOutput(std::unique_ptr<WritableFile> file, const Options& options, std::uint64_t number)
      : file_(std::move(file)),
        builder_(
            {.block_size = options.block_size, .filter_bits_per_key = options.filter_bits_per_key},
            file_.get()),
        meta_{.number = number, .file_size = 0, .smallest = {}, .largest = {}} {}

  void Add(std::string_view key, std::string_view value) {
    if (builder_.NumEntries() == 0) {
      meta_.smallest.assign(key);
    }
    meta_.largest.assign(key);
    builder_.Add(key, value);
  }
  [[nodiscard]] std::uint64_t FileSize() const noexcept { return builder_.FileSize(); }
  [[nodiscard]] const std::string& LargestKey() const noexcept { return meta_.largest; }

  // Writes the rest, syncs (a table is synced before the MANIFEST refers to
  // it: ADR 0002, rule 3) and closes.
  [[nodiscard]] Result<FileMetaData> Finish() {
    Status status = builder_.Finish();
    if (status.ok()) {
      status = file_->Sync();
    }
    if (status.ok()) {
      status = file_->Close();
    }
    FORGEDB_RETURN_IF_ERROR(status);
    meta_.file_size = builder_.FileSize();
    return meta_;
  }

 private:
  std::unique_ptr<WritableFile> file_;
  TableBuilder builder_;
  FileMetaData meta_;
};

}  // namespace

Result<std::optional<FileMetaData>> DBImpl::WriteTable(Iterator& entries) {
  if (!entries.Valid()) {
    FORGEDB_RETURN_IF_ERROR(entries.status());
    return std::optional<FileMetaData>();  // nothing to write
  }
  const std::uint64_t number = versions_->NewFileNumber();
  const std::filesystem::path path = TableFileName(name_, number);
  FORGEDB_ASSIGN_OR_RETURN(std::unique_ptr<WritableFile> file, file_system_->NewWritableFile(path));
  Result<FileMetaData> meta = [&]() -> Result<FileMetaData> {
    TableOutput output(std::move(file), options_, number);
    for (; entries.Valid(); entries.Next()) {
      output.Add(entries.key(), entries.value());
    }
    FORGEDB_RETURN_IF_ERROR(entries.status());
    return output.Finish();
  }();
  if (!meta.ok()) {
    // Unreferenced, so the next open would delete it anyway.
    static_cast<void>(file_system_->RemoveFile(path));
    return meta.status();
  }
  return std::optional<FileMetaData>(std::move(meta).value());
}

Status DBImpl::WriteLevel0Table(MemTable& memtable, VersionEdit& edit) {
  const std::unique_ptr<Iterator> entries = NewMemTableIterator(MemTableRef(&memtable));
  entries->SeekToFirst();
  FORGEDB_ASSIGN_OR_RETURN(std::optional<FileMetaData> meta, WriteTable(*entries));
  if (meta) {
    edit.added_files.push_back({.level = 0, .file = std::move(*meta)});
  }
  return Status::OK();
}

Status DBImpl::Recover() {
  FileSystem& fs = *file_system_;
  FORGEDB_RETURN_IF_ERROR(fs.CreateDir(name_));

  // One open DB per directory, across processes and within this one.
  Result<std::unique_ptr<FileLock>> lock = fs.LockFile(LockFileName(name_));
  if (!lock.ok()) {
    if (lock.status().code() == Status::Code::kBusy) {
      return Status::Busy("database is already open (in another process or another DB object)",
                          name_.generic_string());
    }
    return lock.status();
  }
  lock_ = std::move(lock).value();

  FORGEDB_ASSIGN_OR_RETURN(const std::vector<std::string> children, fs.GetChildren(name_));
  versions_ = std::make_unique<VersionSet>(file_system_.get(), name_);
  FORGEDB_RETURN_IF_ERROR(versions_->Recover(children, recovery_.manifest));

  // The logs written since the MANIFEST's last flush, oldest first. Older
  // logs hold nothing that is not in a table; they are deleted below.
  std::vector<std::uint64_t> logs;
  for (const std::string& child : children) {
    if (const std::optional<ParsedFileName> parsed = ParseFileName(child);
        parsed && parsed->type == FileType::kLog && parsed->number >= versions_->LogNumber()) {
      logs.push_back(parsed->number);
    }
  }
  std::ranges::sort(logs);

  const MemTableRef recovered = MemTable::Create();
  SequenceNumber last_sequence = versions_->LastSequence();
  for (std::size_t i = 0; i < logs.size(); ++i) {
    FORGEDB_RETURN_IF_ERROR(
        ReplayLog(logs[i], /*newest=*/i + 1 == logs.size(), *recovered, last_sequence));
  }

  // Everything recovered goes into a synced table, so no log is replayed
  // twice; then a new log and a new MANIFEST that records them.
  VersionEdit edit;
  FORGEDB_RETURN_IF_ERROR(WriteLevel0Table(*recovered, edit));
  if (!edit.added_files.empty()) {
    recovery_.table_written = edit.added_files.front().file.number;
  }
  log_number_ = versions_->NewFileNumber();
  FORGEDB_ASSIGN_OR_RETURN(log_file_, fs.NewWritableFile(LogFileName(name_, log_number_)));
  log_ = std::make_unique<log::Writer>(log_file_.get());
  edit.log_number = log_number_;
  edit.last_sequence = last_sequence;
  FORGEDB_RETURN_IF_ERROR(versions_->StartNewManifest(edit));
  recovery_.new_log_number = log_number_;
  recovery_.new_manifest_number = versions_->ManifestNumber();

  last_sequence_ = last_sequence;
  version_ = versions_->Current();
  mem_ = MemTable::Create();
  DeleteObsoleteFiles();
  background_thread_ = std::thread([this] { BackgroundThread(); });
  return Status::OK();
}

void DBImpl::DeleteObsoleteFiles() {
  Result<std::vector<std::string>> children = file_system_->GetChildren(name_);
  if (!children.ok()) {
    return;  // they will be deleted at the next open
  }
  const std::vector<std::uint64_t> live = versions_->LiveFiles();
  for (const std::string& child : *children) {
    const std::optional<ParsedFileName> parsed = ParseFileName(child);
    if (!parsed) {
      continue;  // not ours: left alone
    }
    bool obsolete = false;
    switch (parsed->type) {
      case FileType::kLog:
        obsolete = parsed->number < log_number_;
        break;
      case FileType::kTable:
        obsolete = !std::ranges::binary_search(live, parsed->number);
        break;
      case FileType::kDescriptor:
        obsolete = parsed->number != versions_->ManifestNumber();
        break;
      case FileType::kCurrentTemp:
        obsolete = true;
        break;
      case FileType::kCurrent:
      case FileType::kLock:
        break;
    }
    if (obsolete && file_system_->RemoveFile(name_ / child).ok()) {
      recovery_.files_deleted.push_back(child);
    }
  }
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

DBImpl::~DBImpl() {
  // Destroying a DB while writes are in flight is a caller bug.
  FORGEDB_DCHECK(writers_.empty());
  // Stop the background thread: it finishes a flush that is running and
  // starts nothing new (see "Shutdown" in db_impl.h).
  {
    const std::scoped_lock lock(mutex_);
    shutting_down_ = true;
  }
  stopping_.store(true);  // a running compaction stops at its next check
  work_cv_.notify_all();
  if (background_thread_.joinable()) {
    background_thread_.join();
  }
  // The destructor can't report errors; every acknowledged write has already
  // been flushed (and synced, if asked). Close the files, then release LOCK,
  // so the next owner finds nothing of ours open.
  if (log_file_ != nullptr) {
    static_cast<void>(log_file_->Close());
  }
  versions_.reset();
  table_cache_.reset();
  if (lock_ != nullptr) {
    static_cast<void>(file_system_->UnlockFile(std::move(lock_)));
  }
}

Status DBImpl::Write(const WriteOptions& options, const WriteBatch& batch) {
  // Validation needs no lock: the batch is the caller's, the limits are fixed.
  SizeChecker sizes;
  FORGEDB_RETURN_IF_ERROR(batch.Iterate(sizes));
  FORGEDB_RETURN_IF_ERROR(sizes.status());

  Writer self(&batch, options.sync);
  self.user_bytes = sizes.Bytes();
  std::unique_lock lock(mutex_);
  writers_.push_back(&self);
  self.cv.wait(lock, [&] { return self.done || &self == writers_.front(); });
  if (self.done) {
    return self.status;  // a leader wrote our batch as part of its group
  }

  // We lead: make room if the memtable is full, then write a group of
  // batches starting with ours.
  Status status = background_error_;
  const Writer* last = &self;
  if (status.ok()) {
    status = MakeRoomForWrite(lock);
  }
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
    const std::uint64_t log_bytes_before = log_->BytesWritten();
    if (status.ok()) {
      log_touched = true;
      status = log_->AddRecord(WriteBatchInternal::Contents(group_batch_));
    }
    const std::uint64_t log_bytes = log_->BytesWritten() - log_bytes_before;
    if (status.ok() && group.sync) {
      status = log_file_->Sync();
    }
    if (status.ok()) {
      // Checks the sequence range and applies all records or none.
      status = WriteBatchInternal::InsertInto(group_batch_, *memtable);
    }
    lock.lock();

    stats_.log_bytes += log_bytes;  // appended, whatever happened next
    if (status.ok()) {
      last_sequence_ = first + count - 1;  // publishes the whole group at once
      ++groups_written_;
      mem_entries_ += count;
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
    if (status.ok()) {
      stats_.user_bytes += member->user_bytes;
    }
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
    if (writer->force_flush) {
      break;  // FlushForTesting leads on its own
    }
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

SequenceNumber DBImpl::LastSequence() {
  const std::scoped_lock lock(mutex_);
  return last_sequence_;
}

std::uint64_t DBImpl::GroupsWrittenForTesting() {
  const std::scoped_lock lock(mutex_);
  return groups_written_;
}

std::uint64_t DBImpl::FlushesForTesting() {
  const std::scoped_lock lock(mutex_);
  return stats_.flushes;
}

std::shared_ptr<const Version> DBImpl::CurrentVersion() {
  const std::scoped_lock lock(mutex_);
  return version_;
}

namespace {

void AddStall(StallStats& stall, std::chrono::steady_clock::time_point start) {
  ++stall.count;
  stall.microseconds += MicrosecondsSince(start);
}

}  // namespace

Status DBImpl::MakeRoomForWrite(std::unique_lock<std::mutex>& lock, bool force) {
  bool delayed = false;
  while (true) {
    if (!background_error_.ok()) {
      return background_error_;
    }
    const auto level0_files = static_cast<int>(version_->files[0].size());
    if (!delayed && level0_files >= options_.level0_slowdown_writes_trigger) {
      // Level 0 is filling up: give compaction a little time, once per write
      // (see "Write stalls" in db_impl.h).
      const auto start = std::chrono::steady_clock::now();
      lock.unlock();
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
      lock.lock();
      AddStall(stats_.level0_slowdown, start);
      delayed = true;
      continue;
    }
    const bool room =
        mem_entries_ == 0 || (!force && mem_->ApproximateMemoryUsage() < options_.memtable_bytes);
    if (room) {
      return Status::OK();
    }
    if (imm_ || level0_files >= options_.level0_stop_writes_trigger) {
      // The last switch's flush is still running, or level 0 is full: wait
      // for the background thread.
      const auto start = std::chrono::steady_clock::now();
      StallStats& stall = imm_ ? stats_.memtable_full : stats_.level0_stop;
      done_cv_.wait(lock);
      AddStall(stall, start);
      continue;
    }
    FORGEDB_RETURN_IF_ERROR(SwitchMemtable(lock));
    if (synchronous_flush_) {
      done_cv_.wait(lock, [this] { return BackgroundIdle(); });
      return background_error_;
    }
    return Status::OK();  // the new memtable is empty
  }
}

Status DBImpl::SwitchMemtable(std::unique_lock<std::mutex>& lock) {
  // Sync the old log first: writes in the new log must not survive a crash
  // that loses older, unsynced ones (recovery yields a prefix of the writes,
  // never a hole).
  const std::uint64_t old_log = log_number_;
  const std::uint64_t new_log = versions_->NewFileNumber();
  lock.unlock();
  Status status = log_file_->Sync();
  if (status.ok()) {
    status = log_file_->Close();
  }
  Result<std::unique_ptr<WritableFile>> new_file = Status::Internal("not created");
  if (status.ok()) {
    new_file = file_system_->NewWritableFile(LogFileName(name_, new_log));
    status = new_file.status();
  }
  lock.lock();
  if (!status.ok()) {
    background_error_ = status;
    return status;
  }
  log_file_ = std::move(new_file).value();
  log_ = std::make_unique<log::Writer>(log_file_.get());
  log_number_ = new_log;
  imm_ = std::move(mem_);
  mem_ = MemTable::Create();
  mem_entries_ = 0;
  pending_flush_ = FlushJob{
      .memtable = imm_, .flushed_through = last_sequence_, .new_log = new_log, .old_log = old_log};
  flush_queued_.store(true);
  work_cv_.notify_one();
  return Status::OK();
}

// --- The background thread ------------------------------------------------------

bool DBImpl::HasBackgroundWork() const {
  if (!background_error_.ok()) {
    return manual_level_.has_value();  // only to answer it
  }
  return pending_flush_.has_value() || manual_level_.has_value() ||
         (!compactions_paused_ && NeedsCompaction(*version_, options_));
}

bool DBImpl::BackgroundIdle() const {
  return !background_error_.ok() || shutting_down_ || (!imm_ && !busy_ && !HasBackgroundWork());
}

void DBImpl::BackgroundThread() {
  std::unique_lock lock(mutex_);
  while (true) {
    work_cv_.wait(lock, [this] { return shutting_down_ || HasBackgroundWork(); });
    if (shutting_down_) {
      return;  // a queued flush stays in the logs (see "Shutdown")
    }
    // Busy until the job's files are deleted too, so that a waiter (a
    // synchronous flush, a manual compaction) sees every file operation of
    // the job done, in the same order every time.
    busy_ = true;
    bool manual_answered = false;
    if (!background_error_.ok()) {
      manual_level_.reset();  // nothing more will run: answer the request
      manual_answered = true;
    } else if (pending_flush_) {
      DoFlush(lock);  // flushes come first
    } else if (manual_level_) {
      const int level = manual_level_.value_or(0);
      std::optional<Compaction> compaction =
          CompactionForLevel(version_, level, compaction_pointers_);
      if (compaction) {
        DoCompaction(lock, *compaction, /*manual=*/true);
      }
      manual_level_.reset();
      manual_answered = true;
    } else if (std::optional<Compaction> compaction =
                   compactions_paused_ ? std::nullopt
                                       : PickCompaction(version_, options_, compaction_pointers_)) {
      DoCompaction(lock, *compaction, /*manual=*/false);
    }
    DeleteObsoleteBackgroundFiles(lock);
    busy_ = false;
    manual_done_ += manual_answered ? 1U : 0U;
    done_cv_.notify_all();
  }
}

void DBImpl::DoFlush(std::unique_lock<std::mutex>& lock) {
  if (!pending_flush_) {
    return;
  }
  const FlushJob job = std::move(*pending_flush_);
  pending_flush_.reset();
  flush_queued_.store(false);

  const auto start = std::chrono::steady_clock::now();
  lock.unlock();
  std::uint64_t bytes_written = 0;
  const Status status = RunFlush(job, &bytes_written);
  lock.lock();

  if (status.ok()) {
    version_ = versions_->Current();
    imm_.Reset();
    ++stats_.flushes;
    stats_.flush_microseconds += MicrosecondsSince(start);
    stats_.flush_bytes_written += bytes_written;
    // The old log is kept until the next MANIFEST record (ADR 0007); the
    // one the previous flush retired can go.
    if (const std::optional<std::uint64_t> obsolete = std::exchange(retired_log_, job.old_log)) {
      logs_to_delete_.push_back(*obsolete);
    }
  } else {
    // imm_ stays readable; its writes are still in the old log.
    background_error_ = status;
  }
  done_cv_.notify_all();
}

Status DBImpl::RunFlush(const FlushJob& job, std::uint64_t* bytes_written) {
  VersionEdit edit;
  FORGEDB_RETURN_IF_ERROR(WriteLevel0Table(*job.memtable, edit));
  for (const VersionEdit::AddedFile& added : edit.added_files) {
    *bytes_written += added.file.file_size;
  }
  // The new log is the first one recovery needs from now on.
  edit.log_number = job.new_log;
  edit.last_sequence = job.flushed_through;
  FORGEDB_RETURN_IF_ERROR(versions_->LogAndApply(edit));
  RecordEdit(edit);
  return Status::OK();
}

void DBImpl::FlushDuringCompaction() {
  std::unique_lock lock(mutex_);
  if (pending_flush_ && background_error_.ok() && !shutting_down_) {
    DoFlush(lock);
  }
}

void DBImpl::DoCompaction(std::unique_lock<std::mutex>& lock, const Compaction& compaction,
                          bool manual) {
  // Flushes run in the middle (FlushDuringCompaction) count as flush time,
  // not compaction time.
  const auto start = std::chrono::steady_clock::now();
  const std::uint64_t flush_microseconds = stats_.flush_microseconds;
  lock.unlock();
  bool abandoned = false;
  CompactionStats done;
  const Status status =
      RunCompaction(compaction, /*allow_trivial_move=*/!manual, &abandoned, &done);
  lock.lock();
  if (status.ok()) {
    version_ = versions_->Current();
    const std::uint64_t elapsed = MicrosecondsSince(start);
    const std::uint64_t flushing = stats_.flush_microseconds - flush_microseconds;
    CompactionStats& level =
        stats_.levels.at(static_cast<std::size_t>(compaction.level)).compactions;
    level.count += done.count;
    level.trivial_moves += done.trivial_moves;
    level.microseconds += elapsed > flushing ? elapsed - flushing : 0;
    level.bytes_read += done.bytes_read;
    level.bytes_written += done.bytes_written;
  } else if (!abandoned) {
    background_error_ = status;  // writes stop; reads go on (see "Errors")
  }
}

namespace {

// Decides, entry by entry, what a compaction drops (ADR 0008). Entries come
// in internal-key order: by user key, newest first.
class DropRules {
 public:
  DropRules(const Compaction& compaction, SequenceNumber smallest_snapshot)
      : compaction_(&compaction), smallest_snapshot_(smallest_snapshot) {}

  [[nodiscard]] bool Drop(const ParsedInternalKey& key) {
    const bool first_of_key = !has_user_key_ || key.user_key != user_key_;
    if (first_of_key) {
      user_key_.assign(key.user_key);
      has_user_key_ = true;
    }
    //  (a) a newer entry of the key at or below the oldest snapshot hides
    //      this one from every reader;
    //  (b) a deletion at or below it, with no older version of the key
    //      below the output level, deletes nothing any reader can see.
    const bool drop = (!first_of_key && newer_sequence_ <= smallest_snapshot_) ||
                      (key.type == ValueType::kDeletion && key.sequence <= smallest_snapshot_ &&
                       compaction_->IsBaseLevelForKey(key.user_key));
    newer_sequence_ = key.sequence;
    return drop;
  }

 private:
  const Compaction* compaction_;
  SequenceNumber smallest_snapshot_;
  std::string user_key_;
  bool has_user_key_ = false;
  SequenceNumber newer_sequence_ = 0;  // of the entry before, same user key
};

// The tables a compaction writes, recorded in `edit` as they are finished.
class CompactionOutputs {
 public:
  CompactionOutputs(FileSystem* fs, std::filesystem::path db, const Options& options,
                    VersionSet* versions, int level, VersionEdit* edit)
      : fs_(fs),
        db_(std::move(db)),
        options_(&options),
        versions_(versions),
        level_(level),
        edit_(edit) {}

  // Tables are cut only between user keys, so every version of a key stays
  // in one table (ADR 0008).
  [[nodiscard]] Status Add(std::string_view key, std::string_view user_key,
                           std::string_view value) {
    if (output_ && output_->FileSize() >= options_->target_file_size &&
        ExtractUserKey(output_->LargestKey()) != user_key) {
      FORGEDB_RETURN_IF_ERROR(Finish());
    }
    if (!output_) {
      const std::uint64_t number = versions_->NewFileNumber();
      FORGEDB_ASSIGN_OR_RETURN(std::unique_ptr<WritableFile> file,
                               fs_->NewWritableFile(TableFileName(db_, number)));
      numbers_.push_back(number);
      output_.emplace(std::move(file), *options_, number);
    }
    output_->Add(key, value);
    return Status::OK();
  }

  // Finishes the table being written, if any.
  [[nodiscard]] Status Finish() {
    if (!output_) {
      return Status::OK();
    }
    Result<FileMetaData> meta = output_->Finish();
    output_.reset();
    FORGEDB_RETURN_IF_ERROR(meta.status());
    edit_->added_files.push_back({.level = level_, .file = std::move(meta).value()});
    return Status::OK();
  }

  // After a failure: removes every table written.
  void RemoveAll() {
    output_.reset();
    for (const std::uint64_t number : numbers_) {
      static_cast<void>(fs_->RemoveFile(TableFileName(db_, number)));
    }
  }

 private:
  FileSystem* fs_;
  std::filesystem::path db_;
  const Options* options_;
  VersionSet* versions_;
  int level_;
  VersionEdit* edit_;
  std::optional<TableOutput> output_;
  std::vector<std::uint64_t> numbers_;
};

// Copies what the drop rules keep from `input` into `outputs`. Every 256
// entries it calls `keep_going`, which may run a queued flush; false
// abandons the compaction.
Status MergeInputs(Iterator& input, DropRules& rules, CompactionOutputs& outputs,
                   const std::function<bool()>& keep_going, bool* abandoned) {
  std::uint64_t entries = 0;
  for (input.SeekToFirst(); input.Valid(); input.Next()) {
    if (++entries % 256 == 0 && !keep_going()) {
      *abandoned = true;
      return Status::Internal("compaction abandoned at shutdown");
    }
    const std::optional<ParsedInternalKey> key = ParseInternalKey(input.key());
    if (!key) {
      return Status::Corruption("malformed internal key in a table being compacted");
    }
    if (!rules.Drop(*key)) {
      FORGEDB_RETURN_IF_ERROR(outputs.Add(input.key(), key->user_key, input.value()));
    }
  }
  return input.status();  // a damaged input stops the compaction
}

}  // namespace

Status DBImpl::RunCompaction(const Compaction& compaction, bool allow_trivial_move, bool* abandoned,
                             CompactionStats* stats) {
  const int output_level = compaction.level + 1;
  VersionEdit edit;
  for (int which = 0; which < 2; ++which) {
    for (const FileMetaData& file : compaction.inputs.at(static_cast<std::size_t>(which))) {
      edit.removed_files.push_back({.level = compaction.level + which, .number = file.number});
    }
  }
  if (allow_trivial_move && compaction.IsTrivialMove()) {
    // Nothing to merge: the table moves down a level as it is.
    edit.added_files.push_back({.level = output_level, .file = compaction.inputs[0].front()});
    FORGEDB_RETURN_IF_ERROR(versions_->LogAndApply(edit));
    edit.removed_files.clear();  // the table is still in use
    RecordEdit(edit);
    ++stats->trivial_moves;
    return Status::OK();
  }

  // Versions at or below the oldest snapshot (or, with none, the latest
  // write) that something newer of the same key hides can never be read
  // again (ADR 0008).
  SequenceNumber smallest_snapshot = 0;
  {
    const std::scoped_lock lock(mutex_);
    smallest_snapshot = snapshots_->Oldest().value_or(last_sequence_);
  }

  std::vector<std::unique_ptr<Iterator>> children;
  for (const std::vector<FileMetaData>& files : compaction.inputs) {
    for (const FileMetaData& file : files) {
      FORGEDB_ASSIGN_OR_RETURN(std::unique_ptr<Iterator> table,
                               table_cache_->NewIterator(file.number, file.file_size));
      children.push_back(std::move(table));
    }
  }
  const std::unique_ptr<Iterator> input =
      NewMergingIterator(std::move(children), &CompareInternalKeys);

  DropRules rules(compaction, smallest_snapshot);
  CompactionOutputs outputs(file_system_.get(), name_, options_, versions_.get(), output_level,
                            &edit);
  const auto keep_going = [this] {
    if (stopping_.load()) {
      return false;
    }
    if (flush_queued_.load()) {
      FlushDuringCompaction();  // flushes come first
    }
    return true;
  };
  Status status = MergeInputs(*input, rules, outputs, keep_going, abandoned);
  if (status.ok()) {
    status = outputs.Finish();
  }
  if (!status.ok()) {
    outputs.RemoveAll();  // no MANIFEST record names them
    return status;
  }
  // If this fails, the edit may still be in the MANIFEST (appended, only the
  // sync failed), and a crash may keep it: its outputs must stay. The next
  // open deletes them if the edit is not there.
  FORGEDB_RETURN_IF_ERROR(versions_->LogAndApply(edit));
  RecordEdit(edit);
  ++stats->count;
  for (const std::vector<FileMetaData>& files : compaction.inputs) {
    for (const FileMetaData& file : files) {
      stats->bytes_read += file.file_size;
    }
  }
  for (const VersionEdit::AddedFile& added : edit.added_files) {
    stats->bytes_written += added.file.file_size;
  }
  return Status::OK();
}

void DBImpl::RecordEdit(const VersionEdit& edit) {
  // The tables the previous record removed may go once a newer record is in
  // the MANIFEST: if this record is damaged later, recovery ignores it, and
  // the tables it removes must still be there (ADR 0007 rule 5, ADR 0008).
  deletable_tables_.insert(deletable_tables_.end(), removed_by_last_edit_.begin(),
                           removed_by_last_edit_.end());
  removed_by_last_edit_.clear();
  for (const VersionEdit::RemovedFile& removed : edit.removed_files) {
    removed_by_last_edit_.push_back(removed.number);
  }
}

void DBImpl::DeleteObsoleteBackgroundFiles(std::unique_lock<std::mutex>& lock) {
  if (deletable_tables_.empty() && logs_to_delete_.empty()) {
    return;
  }
  lock.unlock();
  // A table still listed by a version some read holds stays until a later
  // pass. If a removal fails, the next open deletes the file.
  const std::vector<std::uint64_t> live = versions_->LiveFiles();
  std::vector<std::uint64_t> still_used;
  for (const std::uint64_t number : deletable_tables_) {
    if (std::ranges::binary_search(live, number)) {
      still_used.push_back(number);
      continue;
    }
    table_cache_->Evict(number);
    static_cast<void>(file_system_->RemoveFile(TableFileName(name_, number)));
  }
  deletable_tables_ = std::move(still_used);
  for (const std::uint64_t number : logs_to_delete_) {
    static_cast<void>(file_system_->RemoveFile(LogFileName(name_, number)));
  }
  logs_to_delete_.clear();
  lock.lock();
}

// --- Statistics and test hooks ----------------------------------------------------

DBStats DBImpl::GetStats() {
  const std::scoped_lock lock(mutex_);
  DBStats stats = stats_;
  for (std::size_t level = 0; level < stats.levels.size(); ++level) {
    const std::vector<FileMetaData>& files = version_->files.at(level);
    stats.levels[level].tables = files.size();
    for (const FileMetaData& file : files) {
      stats.levels[level].bytes += file.file_size;
    }
  }
  return stats;
}

std::optional<SequenceNumber> DBImpl::OldestSnapshotForTesting() const {
  return snapshots_->Oldest();
}

void DBImpl::SetSynchronousFlushForTesting(bool on) {
  const std::scoped_lock lock(mutex_);
  synchronous_flush_ = on;
}

void DBImpl::WaitForFlushForTesting() {
  std::unique_lock lock(mutex_);
  done_cv_.wait(lock, [this] { return (!imm_ && !busy_) || !background_error_.ok(); });
}

bool DBImpl::FlushPendingForTesting() {
  const std::scoped_lock lock(mutex_);
  return static_cast<bool>(imm_);
}

void DBImpl::WaitForBackgroundWorkForTesting() {
  std::unique_lock lock(mutex_);
  done_cv_.wait(lock, [this] { return BackgroundIdle(); });
}

void DBImpl::SetCompactionsPausedForTesting(bool on) {
  {
    const std::scoped_lock lock(mutex_);
    compactions_paused_ = on;
  }
  work_cv_.notify_one();
}

std::uint64_t DBImpl::CompactionsForTesting() {
  const std::scoped_lock lock(mutex_);
  std::uint64_t compactions = 0;
  for (const LevelStats& level : stats_.levels) {
    compactions += level.compactions.count + level.compactions.trivial_moves;
  }
  return compactions;
}

Status DBImpl::FlushForTesting() {
  // Takes a place in the writers' queue: only the leader may switch logs.
  const WriteBatch nothing;
  Writer self(&nothing, false);
  self.force_flush = true;
  std::unique_lock lock(mutex_);
  writers_.push_back(&self);
  self.cv.wait(lock, [&] { return &self == writers_.front(); });
  Status status = MakeRoomForWrite(lock, /*force=*/true);
  writers_.pop_front();
  if (!writers_.empty()) {
    writers_.front()->cv.notify_one();
  }
  if (status.ok()) {
    done_cv_.wait(lock, [this] { return BackgroundIdle(); });
    status = background_error_;
  }
  return status;
}

Status DBImpl::CompactLevelForTesting(int level) {
  std::unique_lock lock(mutex_);
  done_cv_.wait(lock, [this] { return !manual_level_.has_value(); });
  manual_level_ = level;
  const std::uint64_t ticket = manual_done_ + 1;
  work_cv_.notify_one();
  done_cv_.wait(lock, [&] { return manual_done_ >= ticket || shutting_down_; });
  return background_error_;
}

Status DBImpl::CompactAllForTesting() {
  FORGEDB_RETURN_IF_ERROR(FlushForTesting());
  for (int level = 0; level + 1 < kNumLevels; ++level) {
    while (!CurrentVersion()->files.at(static_cast<std::size_t>(level)).empty()) {
      FORGEDB_RETURN_IF_ERROR(CompactLevelForTesting(level));
    }
  }
  WaitForBackgroundWorkForTesting();
  const std::scoped_lock lock(mutex_);
  return background_error_;
}

std::shared_ptr<const Snapshot> DBImpl::GetSnapshot() {
  const std::scoped_lock lock(mutex_);
  return std::make_shared<const SnapshotImpl>(snapshots_, last_sequence_);
}

Result<DBImpl::ReadView> DBImpl::CurrentReadView(const ReadOptions& options) {
  std::optional<SequenceNumber> snapshot_sequence;
  if (options.snapshot != nullptr) {
    const auto* snapshot = dynamic_cast<const SnapshotImpl*>(options.snapshot.get());
    if (snapshot == nullptr || snapshot->List() != snapshots_.get()) {
      return Status::InvalidArgument("ReadOptions::snapshot was not made by this DB");
    }
    snapshot_sequence = snapshot->Sequence();
  }
  const std::scoped_lock lock(mutex_);
  return ReadView{.memtable = mem_,
                  .immutable = imm_,
                  .version = version_,
                  .sequence = snapshot_sequence.value_or(last_sequence_)};
}

namespace {

// Whether `user_key` is within the file's key range.
bool InRange(const FileMetaData& file, std::string_view user_key) noexcept {
  return user_key >= ExtractUserKey(file.smallest) && user_key <= ExtractUserKey(file.largest);
}

Result<std::string> FoundOrNotFound(MemTable::GetResult result, std::string&& value) {
  if (result == MemTable::GetResult::kFound) {
    return std::move(value);
  }
  return Status::NotFound("key not found");
}

}  // namespace

Result<std::string> DBImpl::Get(const ReadOptions& options, std::string_view key) {
  FORGEDB_RETURN_IF_ERROR(CheckKeySize(key));
  FORGEDB_ASSIGN_OR_RETURN(const ReadView view, CurrentReadView(options));
  const LookupKey lookup(key, view.sequence);
  std::string value;

  // Newest data first: the memtable, the one being flushed, then the tables.
  for (const MemTableRef* memtable : {&view.memtable, &view.immutable}) {
    if (*memtable) {
      const MemTable::GetResult result = (*memtable)->Get(lookup, &value);
      if (result != MemTable::GetResult::kNotFound) {
        return FoundOrNotFound(result, std::move(value));
      }
    }
  }
  // Level 0 newest first (its files may overlap); then each deeper level,
  // where at most one table can hold the key: the first whose largest key is
  // not before it (tables there are sorted and never share a user key).
  const auto search = [&](const FileMetaData& file) -> Result<std::optional<std::string>> {
    if (!InRange(file, key)) {
      return std::optional<std::string>();
    }
    FORGEDB_ASSIGN_OR_RETURN(const TableLookup found,
                             table_cache_->Get(file.number, file.file_size, lookup, &value));
    if (found == TableLookup::kFound) {
      return std::optional<std::string>(std::move(value));
    }
    if (found == TableLookup::kDeleted) {
      return Status::NotFound("key not found");
    }
    return std::optional<std::string>();
  };
  for (const FileMetaData& file : view.version->files[0]) {
    FORGEDB_ASSIGN_OR_RETURN(std::optional<std::string> found, search(file));
    if (found) {
      return std::move(*found);
    }
  }
  for (std::size_t level = 1; level < view.version->files.size(); ++level) {
    const std::vector<FileMetaData>& files = view.version->files[level];
    const auto it = std::ranges::partition_point(
        files, [key](const FileMetaData& file) { return ExtractUserKey(file.largest) < key; });
    if (it == files.end()) {
      continue;
    }
    FORGEDB_ASSIGN_OR_RETURN(std::optional<std::string> found, search(*it));
    if (found) {
      return std::move(*found);
    }
  }
  return Status::NotFound("key not found");
}

std::unique_ptr<Iterator> DBImpl::NewIterator(const ReadOptions& options) {
  Result<ReadView> read_view = CurrentReadView(options);
  if (!read_view.ok()) {
    return NewErrorIterator(read_view.status());
  }
  ReadView view = std::move(read_view).value();
  std::vector<std::unique_ptr<Iterator>> children;
  children.push_back(NewMemTableIterator(std::move(view.memtable)));
  if (view.immutable) {
    children.push_back(NewMemTableIterator(std::move(view.immutable)));
  }
  // Every level-0 table, and one level iterator per deeper level, which
  // opens its tables one at a time.
  for (const FileMetaData& file : view.version->files[0]) {
    Result<std::unique_ptr<Iterator>> table =
        table_cache_->NewIterator(file.number, file.file_size);
    if (!table.ok()) {
      return NewErrorIterator(table.status());
    }
    children.push_back(std::move(table).value());
  }
  for (int level = 1; level < kNumLevels; ++level) {
    if (!view.version->files.at(static_cast<std::size_t>(level)).empty()) {
      children.push_back(NewLevelIterator(table_cache_.get(), view.version, level));
    }
  }
  return NewDBIterator(NewMergingIterator(std::move(children), &CompareInternalKeys), view.sequence,
                       std::move(view.version));
}

}  // namespace forgedb
