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
                                                block_cache_.get())) {}

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

Status DBImpl::WriteLevel0Table(MemTable& memtable, VersionEdit& edit) {
  const std::unique_ptr<Iterator> entries = NewMemTableIterator(MemTableRef(&memtable));
  entries->SeekToFirst();
  if (!entries->Valid()) {
    return Status::OK();  // nothing to write
  }
  const std::uint64_t number = versions_->NewFileNumber();
  const std::filesystem::path path = TableFileName(name_, number);
  FORGEDB_ASSIGN_OR_RETURN(std::unique_ptr<WritableFile> file, file_system_->NewWritableFile(path));
  TableBuilder builder(
      {.block_size = options_.block_size, .filter_bits_per_key = options_.filter_bits_per_key},
      file.get());
  FileMetaData meta{
      .number = number, .file_size = 0, .smallest = std::string(entries->key()), .largest = {}};
  for (; entries->Valid(); entries->Next()) {
    meta.largest.assign(entries->key());
    builder.Add(entries->key(), entries->value());
  }
  Status status = builder.Finish();
  // Sync before the MANIFEST refers to it (ADR 0002, rule 3).
  if (status.ok()) {
    status = file->Sync();
  }
  if (status.ok()) {
    status = file->Close();
  }
  if (!status.ok()) {
    // Unreferenced, so the next open would delete it anyway.
    file.reset();
    static_cast<void>(file_system_->RemoveFile(path));
    return status;
  }
  meta.file_size = builder.FileSize();
  edit.added_files.push_back({.level = 0, .file = std::move(meta)});
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
  return Status::OK();
}

void DBImpl::DeleteObsoleteFiles() {
  Result<std::vector<std::string>> children = file_system_->GetChildren(name_);
  if (!children.ok()) {
    return;  // they will be deleted at the next open
  }
  std::vector<std::uint64_t> live;
  for (const std::vector<FileMetaData>& level : version_->files) {
    for (const FileMetaData& file : level) {
      live.push_back(file.number);
    }
  }
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
        obsolete = std::ranges::find(live, parsed->number) == live.end();
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
    if (status.ok()) {
      log_touched = true;
      status = log_->AddRecord(WriteBatchInternal::Contents(group_batch_));
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

std::uint64_t DBImpl::FlushesForTesting() {
  const std::scoped_lock lock(mutex_);
  return flushes_;
}

std::shared_ptr<const Version> DBImpl::CurrentVersionForTesting() {
  const std::scoped_lock lock(mutex_);
  return version_;
}

Status DBImpl::MakeRoomForWrite(std::unique_lock<std::mutex>& lock) {
  if (mem_entries_ == 0 || mem_->ApproximateMemoryUsage() < options_.memtable_bytes) {
    return Status::OK();
  }

  // 1. Switch logs. Sync the old one first: writes in the new log must not
  //    survive a crash that loses older, unsynced ones (recovery yields a
  //    prefix of the writes, never a hole).
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
  const SequenceNumber flushed_through = last_sequence_;
  const MemTableRef immutable = imm_;

  // 2. Write the immutable memtable to a table and record it, with the new
  //    log as the first one recovery needs.
  lock.unlock();
  VersionEdit edit;
  status = WriteLevel0Table(*immutable, edit);
  if (status.ok()) {
    edit.log_number = new_log;
    edit.last_sequence = flushed_through;
    status = versions_->LogAndApply(std::move(edit));
  }
  lock.lock();
  if (!status.ok()) {
    // imm_ stays readable; its data is still in the old log.
    background_error_ = status;
    return status;
  }
  version_ = versions_->Current();
  imm_.Reset();
  ++flushes_;

  // 3. The old log is now redundant. If this fails, the next open deletes it.
  lock.unlock();
  static_cast<void>(file_system_->RemoveFile(LogFileName(name_, old_log)));
  lock.lock();
  return Status::OK();
}

DBImpl::ReadView DBImpl::CurrentReadView() {
  const std::scoped_lock lock(mutex_);
  return {.memtable = mem_, .immutable = imm_, .version = version_, .sequence = last_sequence_};
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

Result<std::string> DBImpl::Get(const ReadOptions& /*options*/, std::string_view key) {
  FORGEDB_RETURN_IF_ERROR(CheckKeySize(key));
  const ReadView view = CurrentReadView();
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
  // Level 0 newest first (its files may overlap); a deeper level has at most
  // one file whose range holds the key (Phase 4).
  for (const std::vector<FileMetaData>& level : view.version->files) {
    for (const FileMetaData& file : level) {
      if (!InRange(file, key)) {
        continue;
      }
      FORGEDB_ASSIGN_OR_RETURN(const TableLookup found,
                               table_cache_->Get(file.number, file.file_size, lookup, &value));
      if (found == TableLookup::kFound) {
        return value;
      }
      if (found == TableLookup::kDeleted) {
        return Status::NotFound("key not found");
      }
    }
  }
  return Status::NotFound("key not found");
}

std::unique_ptr<Iterator> DBImpl::NewIterator(const ReadOptions& /*options*/) {
  ReadView view = CurrentReadView();
  std::vector<std::unique_ptr<Iterator>> children;
  children.push_back(NewMemTableIterator(std::move(view.memtable)));
  if (view.immutable) {
    children.push_back(NewMemTableIterator(std::move(view.immutable)));
  }
  for (const std::vector<FileMetaData>& level : view.version->files) {
    for (const FileMetaData& file : level) {
      Result<std::unique_ptr<Iterator>> table =
          table_cache_->NewIterator(file.number, file.file_size);
      if (!table.ok()) {
        return NewErrorIterator(table.status());
      }
      children.push_back(std::move(table).value());
    }
  }
  return NewDBIterator(NewMergingIterator(std::move(children), &CompareInternalKeys), view.sequence,
                       std::move(view.version));
}

}  // namespace forgedb
