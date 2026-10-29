#include "support/fault_injection_file_system.h"

#include <algorithm>
#include <optional>
#include <span>
#include <string_view>
#include <utility>

namespace forgedb {
namespace {

using Key = std::filesystem::path::string_type;
using Op = FaultInjectionFileSystem::Op;

constexpr std::size_t kPageSize = 4096;
constexpr std::uint64_t kMaxTornBytes = 512;

// UTF-8, with '/' separators on every platform.
std::string ToUtf8(const std::filesystem::path& path) {
  const std::u8string utf8 = path.generic_u8string();
  return {utf8.begin(), utf8.end()};
}

Key KeyOf(const std::filesystem::path& path) { return path.lexically_normal().native(); }

Result<std::string> ReadWholeFile(SequentialFile& file) {
  std::string contents;
  std::string buffer(std::size_t{64} * 1024, '\0');
  while (true) {
    FORGEDB_ASSIGN_OR_RETURN(const std::size_t count, file.Read(buffer));
    if (count == 0) {
      return contents;
    }
    contents.append(buffer.data(), count);
  }
}

Status WriteWholeFile(FileSystem& fs, const std::filesystem::path& path,
                      std::string_view contents) {
  FORGEDB_ASSIGN_OR_RETURN(const std::unique_ptr<WritableFile> file, fs.NewWritableFile(path));
  FORGEDB_RETURN_IF_ERROR(file->Append(contents));
  FORGEDB_RETURN_IF_ERROR(file->Sync());
  return file->Close();
}

}  // namespace

const char* OpName(FaultInjectionFileSystem::Op op) noexcept {
  switch (op) {
    case Op::kNewWritableFile:
      return "NewWritableFile";
    case Op::kAppend:
      return "Append";
    case Op::kFlush:
      return "Flush";
    case Op::kSync:
      return "Sync";
    case Op::kClose:
      return "Close";
    case Op::kSyncFile:
      return "SyncFile";
    case Op::kRenameReplace:
      return "RenameReplace";
    case Op::kRemoveFile:
      return "RemoveFile";
    case Op::kCreateDir:
      return "CreateDir";
    case Op::kLockFile:
      return "LockFile";
  }
  return "unknown operation";
}

// --- Files and locks ------------------------------------------------------------

class FaultWritableFile final : public WritableFile {
 public:
  FaultWritableFile(FaultInjectionFileSystem* fs,
                    std::shared_ptr<FaultInjectionFileSystem::Node> node,
                    std::unique_ptr<WritableFile> base, std::uint64_t epoch)
      : fs_(fs), node_(std::move(node)), base_(std::move(base)), epoch_(epoch) {}

  ~FaultWritableFile() override {
    const std::scoped_lock lock(fs_->mutex_);
    fs_->open_files_.erase(this);
    if (Usable().ok() && !closed_ && !fs_->dead_) {
      // Destroying the file closes it, which hands buffered bytes to the OS.
      node_->flushed = node_->data.size();
    }
  }
  FaultWritableFile(const FaultWritableFile&) = delete;
  FaultWritableFile& operator=(const FaultWritableFile&) = delete;
  FaultWritableFile(FaultWritableFile&&) = delete;
  FaultWritableFile& operator=(FaultWritableFile&&) = delete;

  Status Append(std::string_view data) override {
    const std::scoped_lock lock(fs_->mutex_);
    FORGEDB_RETURN_IF_ERROR(Usable());
    bool injected = false;
    if (Status status = fs_->BeginOperation(Op::kAppend, &injected); !status.ok()) {
      if (injected && fs_->options_.partial_append_on_error && !data.empty()) {
        const std::string_view written = data.substr(0, fs_->Random(data.size()));
        if (!written.empty() && base_->Append(written).ok()) {
          node_->data.append(written);
        }
      }
      return Fail(std::move(status));
    }
    if (Status status = base_->Append(data); !status.ok()) {
      return Fail(std::move(status));
    }
    node_->data.append(data);
    return Status::OK();
  }

  Status Flush() override {
    const std::scoped_lock lock(fs_->mutex_);
    FORGEDB_RETURN_IF_ERROR(Usable());
    if (Status status = fs_->BeginOperation(Op::kFlush); !status.ok()) {
      return Fail(std::move(status));
    }
    if (Status status = base_->Flush(); !status.ok()) {
      return Fail(std::move(status));
    }
    node_->flushed = node_->data.size();
    return Status::OK();
  }

  Status Sync() override {
    const std::scoped_lock lock(fs_->mutex_);
    FORGEDB_RETURN_IF_ERROR(Usable());
    if (Status status = fs_->BeginOperation(Op::kSync); !status.ok()) {
      return Fail(std::move(status));
    }
    if (Status status = base_->Sync(); !status.ok()) {
      return Fail(std::move(status));
    }
    node_->flushed = node_->data.size();
    node_->synced = node_->data.size();
    // Flushing a file also stores its metadata, read as including its own
    // directory entry (ADR 0002, rule 3): its current name is now durable.
    if (node_->has_live_name) {
      fs_->durable_[node_->live_name] = node_;
    }
    return Status::OK();
  }

  Status Close() override {
    const std::scoped_lock lock(fs_->mutex_);
    FORGEDB_RETURN_IF_ERROR(Usable());
    if (Status status = fs_->BeginOperation(Op::kClose); !status.ok()) {
      return Fail(std::move(status));
    }
    if (Status status = base_->Close(); !status.ok()) {
      return Fail(std::move(status));
    }
    node_->flushed = node_->data.size();
    closed_ = true;
    return Status::OK();
  }

  // Called by SimulateCrash with the mutex held: the process that had this
  // file open is gone. Its contents are decided by the crash, not by this
  // handle, so the wrapped file is closed and every later call fails.
  void Detach() {
    if (base_) {
      static_cast<void>(base_->Close());
      base_.reset();
    }
  }

 private:
  // Called with the mutex held.
  [[nodiscard]] Status Usable() const {
    if (epoch_ != fs_->epoch_.load()) {
      return FaultInjectionFileSystem::StaleStatus();
    }
    return error_;
  }
  // Errors are sticky (WritableFile contract): keep the first one.
  Status Fail(Status status) {
    if (error_.ok()) {
      error_ = std::move(status);
    }
    return error_;
  }

  FaultInjectionFileSystem* fs_;
  std::shared_ptr<FaultInjectionFileSystem::Node> node_;
  std::unique_ptr<WritableFile> base_;
  std::uint64_t epoch_;
  Status error_;
  bool closed_ = false;
};

class FaultSequentialFile final : public SequentialFile {
 public:
  FaultSequentialFile(const FaultInjectionFileSystem* fs, std::unique_ptr<SequentialFile> base,
                      std::uint64_t epoch)
      : fs_(fs), base_(std::move(base)), epoch_(epoch) {}

  Result<std::size_t> Read(std::span<char> buffer) override {
    FORGEDB_RETURN_IF_ERROR(Usable());
    return base_->Read(buffer);
  }
  Status Skip(std::uint64_t n) override {
    FORGEDB_RETURN_IF_ERROR(Usable());
    return base_->Skip(n);
  }

 private:
  [[nodiscard]] Status Usable() const {
    if (fs_->dead_.load()) {
      return FaultInjectionFileSystem::DeadStatus();
    }
    return epoch_ == fs_->epoch_.load() ? Status::OK() : FaultInjectionFileSystem::StaleStatus();
  }

  const FaultInjectionFileSystem* fs_;
  std::unique_ptr<SequentialFile> base_;
  std::uint64_t epoch_;
};

class FaultRandomAccessFile final : public RandomAccessFile {
 public:
  FaultRandomAccessFile(const FaultInjectionFileSystem* fs, std::unique_ptr<RandomAccessFile> base,
                        std::uint64_t epoch)
      : fs_(fs), base_(std::move(base)), epoch_(epoch) {}

  Result<std::size_t> Read(std::uint64_t offset, std::span<char> buffer) const override {
    if (fs_->dead_.load()) {
      return FaultInjectionFileSystem::DeadStatus();
    }
    if (epoch_ != fs_->epoch_.load()) {
      return FaultInjectionFileSystem::StaleStatus();
    }
    return base_->Read(offset, buffer);
  }

 private:
  const FaultInjectionFileSystem* fs_;
  std::unique_ptr<RandomAccessFile> base_;
  std::uint64_t epoch_;
};

class FaultFileLock final : public FileLock {
 public:
  FaultFileLock(FaultInjectionFileSystem* fs, std::uint64_t id) : fs_(fs), id_(id) {}
  ~FaultFileLock() override {
    if (fs_ != nullptr) {
      const std::scoped_lock lock(fs_->mutex_);
      static_cast<void>(fs_->ReleaseLockLocked(id_));
    }
  }
  FaultFileLock(const FaultFileLock&) = delete;
  FaultFileLock& operator=(const FaultFileLock&) = delete;
  FaultFileLock(FaultFileLock&&) = delete;
  FaultFileLock& operator=(FaultFileLock&&) = delete;

  [[nodiscard]] const FaultInjectionFileSystem* Owner() const { return fs_; }
  // Called by UnlockFile with the mutex held.
  [[nodiscard]] Status ReleaseLocked() {
    Status status = fs_->ReleaseLockLocked(id_);
    fs_ = nullptr;
    return status;
  }

 private:
  FaultInjectionFileSystem* fs_;
  std::uint64_t id_;
};

// --- FaultInjectionFileSystem ------------------------------------------------------

FaultInjectionFileSystem::FaultInjectionFileSystem(std::shared_ptr<FileSystem> base)
    : FaultInjectionFileSystem(std::move(base), Options{}) {}

FaultInjectionFileSystem::FaultInjectionFileSystem(std::shared_ptr<FileSystem> base,
                                                   const Options& options)
    : base_(std::move(base)), options_(options), rng_(options.seed) {}

FaultInjectionFileSystem::~FaultInjectionFileSystem() = default;

void FaultInjectionFileSystem::FailNth(Op op, std::uint64_t n) {
  const std::scoped_lock lock(mutex_);
  fail_countdown_.at(static_cast<std::size_t>(op)) = n;
}

void FaultInjectionFileSystem::FailWithProbability(Op op, double p) {
  const std::scoped_lock lock(mutex_);
  fail_probability_.at(static_cast<std::size_t>(op)) = p;
}

void FaultInjectionFileSystem::ClearFaults() {
  const std::scoped_lock lock(mutex_);
  fail_countdown_.fill(0);
  fail_probability_.fill(0.0);
}

void FaultInjectionFileSystem::CrashAtOperation(std::uint64_t n) {
  const std::scoped_lock lock(mutex_);
  crash_at_ = n == 0 ? 0 : operations_ + n;
}

std::uint64_t FaultInjectionFileSystem::OperationCount() const {
  const std::scoped_lock lock(mutex_);
  return operations_;
}

void FaultInjectionFileSystem::KillProcess() {
  const std::scoped_lock lock(mutex_);
  crash_at_ = 0;
  dead_.store(true);
}

bool FaultInjectionFileSystem::ProcessIsDead() const { return dead_.load(); }

std::vector<FaultInjectionFileSystem::CrashedFile> FaultInjectionFileSystem::LastCrash() const {
  const std::scoped_lock lock(mutex_);
  return last_crash_;
}

Status FaultInjectionFileSystem::DeadStatus() {
  return Status::IOError("simulated crash",
                         "the process is dead (FaultInjectionFileSystem::CrashAtOperation or "
                         "KillProcess)");
}

Status FaultInjectionFileSystem::StaleStatus() {
  return Status::IOError("simulated crash", "the handle was open when the crash happened");
}

std::uint64_t FaultInjectionFileSystem::Random(std::uint64_t bound) {
  // Modulo, not std::uniform_int_distribution, whose output differs between
  // standard libraries: a seed must give the same crash with MSVC and others.
  return bound == 0 ? 0 : rng_() % bound;
}

bool FaultInjectionFileSystem::Chance(double p) {
  constexpr double kTwoToMinus53 = 1.0 / 9007199254740992.0;
  return static_cast<double>(rng_() >> 11U) * kTwoToMinus53 < p;
}

Status FaultInjectionFileSystem::BeginOperation(Op op, bool* injected) {
  if (dead_.load()) {
    return DeadStatus();
  }
  ++operations_;
  if (crash_at_ != 0 && operations_ >= crash_at_) {
    crash_at_ = 0;
    dead_.store(true);
    return DeadStatus();
  }
  const auto index = static_cast<std::size_t>(op);
  std::uint64_t& countdown = fail_countdown_.at(index);
  bool fail = false;
  if (countdown != 0) {
    --countdown;
    fail = countdown == 0;
  }
  if (!fail && fail_probability_.at(index) > 0) {
    fail = Chance(fail_probability_.at(index));
  }
  if (!fail) {
    return Status::OK();
  }
  if (injected != nullptr) {
    *injected = true;
  }
  return Status::IOError(std::string("injected fault in ") + OpName(op));
}

Status FaultInjectionFileSystem::Track(const Key& key, bool read_contents) {
  if (durable_.contains(key)) {
    return Status::OK();
  }
  const std::filesystem::path path(key);
  if (!read_contents) {
    FORGEDB_ASSIGN_OR_RETURN(const bool exists, base_->FileExists(path));
    NodePtr node;
    if (exists) {
      node = std::make_shared<Node>();  // contents unknown and not needed (a LOCK file)
      SetLiveName(key, node);
    }
    durable_[key] = node;
    return Status::OK();
  }
  Result<std::unique_ptr<SequentialFile>> file = base_->NewSequentialFile(path);
  if (!file.ok()) {
    if (file.status().code() != Status::Code::kNotFound) {
      return file.status();
    }
    FORGEDB_ASSIGN_OR_RETURN(const bool exists, base_->FileExists(path));
    if (exists) {
      return Status::IOError("FaultInjectionFileSystem: not a regular file", ToUtf8(path));
    }
    durable_[key] = nullptr;
    return Status::OK();
  }
  // A file from before tracking began: fully durable by definition.
  FORGEDB_ASSIGN_OR_RETURN(std::string contents, ReadWholeFile(**file));
  auto node = std::make_shared<Node>();
  node->data = std::move(contents);
  node->flushed = node->data.size();
  node->synced = node->data.size();
  SetLiveName(key, node);
  durable_[key] = node;
  return Status::OK();
}

void FaultInjectionFileSystem::SetLiveName(const Key& key, const NodePtr& node) {
  ClearLiveName(key);
  node->has_live_name = true;
  node->live_name = key;
  live_[key] = node;
}

void FaultInjectionFileSystem::ClearLiveName(const Key& key) {
  const auto it = live_.find(key);
  if (it != live_.end()) {
    it->second->has_live_name = false;
    live_.erase(it);
  }
}

Status FaultInjectionFileSystem::ReleaseLockLocked(std::uint64_t id) {
  const auto it = held_locks_.find(id);
  if (it == held_locks_.end()) {
    return Status::OK();  // released by a crash already
  }
  std::unique_ptr<FileLock> lock = std::move(it->second);
  held_locks_.erase(it);
  return base_->UnlockFile(std::move(lock));
}

// --- FileSystem ---------------------------------------------------------------

Result<std::unique_ptr<WritableFile>> FaultInjectionFileSystem::NewWritableFile(
    const std::filesystem::path& path) {
  const Key key = KeyOf(path);
  const std::scoped_lock lock(mutex_);
  FORGEDB_RETURN_IF_ERROR(BeginOperation(Op::kNewWritableFile));
  FORGEDB_RETURN_IF_ERROR(Track(key));
  FORGEDB_ASSIGN_OR_RETURN(std::unique_ptr<WritableFile> base_file, base_->NewWritableFile(path));
  // A new, empty file takes the name. The old file (if any) stays the
  // durable one until the new one is synced.
  const auto node = std::make_shared<Node>();
  SetLiveName(key, node);
  auto file = std::make_unique<FaultWritableFile>(this, node, std::move(base_file), epoch_.load());
  open_files_.insert(file.get());
  return std::unique_ptr<WritableFile>(std::move(file));
}

Result<std::unique_ptr<SequentialFile>> FaultInjectionFileSystem::NewSequentialFile(
    const std::filesystem::path& path) {
  if (dead_.load()) {
    return DeadStatus();
  }
  FORGEDB_ASSIGN_OR_RETURN(std::unique_ptr<SequentialFile> base_file,
                           base_->NewSequentialFile(path));
  return std::unique_ptr<SequentialFile>(
      std::make_unique<FaultSequentialFile>(this, std::move(base_file), epoch_.load()));
}

Result<std::unique_ptr<RandomAccessFile>> FaultInjectionFileSystem::NewRandomAccessFile(
    const std::filesystem::path& path) {
  if (dead_.load()) {
    return DeadStatus();
  }
  FORGEDB_ASSIGN_OR_RETURN(std::unique_ptr<RandomAccessFile> base_file,
                           base_->NewRandomAccessFile(path));
  return std::unique_ptr<RandomAccessFile>(
      std::make_unique<FaultRandomAccessFile>(this, std::move(base_file), epoch_.load()));
}

Status FaultInjectionFileSystem::RenameReplace(const std::filesystem::path& from,
                                               const std::filesystem::path& to) {
  const Key from_key = KeyOf(from);
  const Key to_key = KeyOf(to);
  const std::scoped_lock lock(mutex_);
  FORGEDB_RETURN_IF_ERROR(BeginOperation(Op::kRenameReplace));
  FORGEDB_RETURN_IF_ERROR(Track(from_key));
  FORGEDB_RETURN_IF_ERROR(Track(to_key));
  FORGEDB_RETURN_IF_ERROR(base_->RenameReplace(from, to));
  const auto it = live_.find(from_key);
  if (from_key == to_key || it == live_.end()) {
    return Status::OK();
  }
  const NodePtr node = it->second;
  ClearLiveName(from_key);
  SetLiveName(to_key, node);
  if (options_.durable_renames) {
    durable_[to_key] = node;
    durable_[from_key] = nullptr;
  }
  return Status::OK();
}

Status FaultInjectionFileSystem::RemoveFile(const std::filesystem::path& path) {
  const Key key = KeyOf(path);
  const std::scoped_lock lock(mutex_);
  FORGEDB_RETURN_IF_ERROR(BeginOperation(Op::kRemoveFile));
  FORGEDB_RETURN_IF_ERROR(Track(key));
  FORGEDB_RETURN_IF_ERROR(base_->RemoveFile(path));
  ClearLiveName(key);
  if (options_.durable_removals) {
    durable_[key] = nullptr;
  }
  return Status::OK();
}

Result<bool> FaultInjectionFileSystem::FileExists(const std::filesystem::path& path) {
  if (dead_.load()) {
    return DeadStatus();
  }
  return base_->FileExists(path);
}

Result<std::vector<std::string>> FaultInjectionFileSystem::GetChildren(
    const std::filesystem::path& dir) {
  if (dead_.load()) {
    return DeadStatus();
  }
  return base_->GetChildren(dir);
}

Result<std::uint64_t> FaultInjectionFileSystem::GetFileSize(const std::filesystem::path& path) {
  if (dead_.load()) {
    return DeadStatus();
  }
  return base_->GetFileSize(path);
}

Status FaultInjectionFileSystem::SyncFile(const std::filesystem::path& path) {
  const Key key = KeyOf(path);
  const std::scoped_lock lock(mutex_);
  FORGEDB_RETURN_IF_ERROR(BeginOperation(Op::kSyncFile));
  FORGEDB_RETURN_IF_ERROR(Track(key));
  FORGEDB_RETURN_IF_ERROR(base_->SyncFile(path));
  // Like WritableFile::Sync: all of the file's data, and its name.
  if (const auto it = live_.find(key); it != live_.end()) {
    Node& node = *it->second;
    node.flushed = node.data.size();
    node.synced = node.data.size();
    durable_[key] = it->second;
  }
  return Status::OK();
}

Status FaultInjectionFileSystem::CreateDir(const std::filesystem::path& path) {
  const std::scoped_lock lock(mutex_);
  FORGEDB_RETURN_IF_ERROR(BeginOperation(Op::kCreateDir));
  return base_->CreateDir(path);  // directories are durable once created (assumed)
}

Result<std::unique_ptr<FileLock>> FaultInjectionFileSystem::LockFile(
    const std::filesystem::path& path) {
  const Key key = KeyOf(path);
  const std::scoped_lock lock(mutex_);
  FORGEDB_RETURN_IF_ERROR(BeginOperation(Op::kLockFile));
  // Not read: a locked file's contents may be unreadable, and nobody reads them.
  FORGEDB_RETURN_IF_ERROR(Track(key, /*read_contents=*/false));
  FORGEDB_ASSIGN_OR_RETURN(std::unique_ptr<FileLock> base_lock, base_->LockFile(path));
  if (!live_.contains(key)) {
    SetLiveName(key, std::make_shared<Node>());  // LockFile created it, empty and unsynced
  }
  const std::uint64_t id = next_lock_id_++;
  held_locks_[id] = std::move(base_lock);
  return std::unique_ptr<FileLock>(std::make_unique<FaultFileLock>(this, id));
}

Status FaultInjectionFileSystem::UnlockFile(std::unique_ptr<FileLock> lock) {
  auto* fault_lock = dynamic_cast<FaultFileLock*>(lock.get());
  if (fault_lock == nullptr || fault_lock->Owner() != this) {
    return Status::InvalidArgument(
        "UnlockFile: the lock does not come from this FaultInjectionFileSystem");
  }
  const std::scoped_lock guard(mutex_);
  return fault_lock->ReleaseLocked();
}

// --- Crashing -------------------------------------------------------------------

void FaultInjectionFileSystem::TearAndZero(std::string& contents, std::size_t tail_start) {
  const std::size_t tail = contents.size() - tail_start;
  if (tail == 0) {
    return;
  }
  if (options_.unsynced_tail == UnsyncedTail::kKeepPrefixTorn) {
    // The last write was cut off mid-way: its end is garbage or zeros.
    const std::size_t torn = 1 + Random(std::min<std::uint64_t>(tail, kMaxTornBytes));
    const bool zeros = Chance(0.5);
    for (std::size_t i = contents.size() - torn; i < contents.size(); ++i) {
      contents[i] = zeros ? '\0' : static_cast<char>(Random(256));
    }
  }
  if (options_.zero_pages_in_unsynced_tail) {
    // Pages written out of order: some never made it, and read back as zeros.
    for (std::size_t page = tail_start / kPageSize * kPageSize; page < contents.size();
         page += kPageSize) {
      if (Chance(1.0 / 3.0)) {
        const std::size_t begin = std::max(page, tail_start);
        const std::size_t end = std::min(page + kPageSize, contents.size());
        std::fill(contents.begin() + static_cast<std::ptrdiff_t>(begin),
                  contents.begin() + static_cast<std::ptrdiff_t>(end), '\0');
      }
    }
  }
}

std::string FaultInjectionFileSystem::PowerLossContents(const Node& node) {
  const auto synced = static_cast<std::size_t>(node.synced);
  std::string contents = node.data.substr(0, synced);
  if (options_.unsynced_tail == UnsyncedTail::kDrop) {
    return contents;
  }
  // Only flushed bytes ever reached the operating system.
  const std::size_t tail = static_cast<std::size_t>(node.flushed) - synced;
  const auto kept = static_cast<std::size_t>(Random(tail + 1));
  contents.append(node.data, synced, kept);
  TearAndZero(contents, synced);
  return contents;
}

Status FaultInjectionFileSystem::SimulateCrash(CrashKind kind) {
  const std::scoped_lock lock(mutex_);
  // The process is gone, and its open files and locks with it.
  for (FaultWritableFile* file : open_files_) {
    file->Detach();
  }
  open_files_.clear();
  for (auto& [id, held] : held_locks_) {
    static_cast<void>(base_->UnlockFile(std::move(held)));
  }
  held_locks_.clear();
  epoch_.fetch_add(1);

  std::set<Key> keys;
  for (const auto& [key, node] : live_) {
    keys.insert(key);
  }
  for (const auto& [key, node] : durable_) {
    keys.insert(key);
  }

  last_crash_.clear();
  Status first_error;
  for (const Key& key : keys) {
    const auto live_it = live_.find(key);
    const auto durable_it = durable_.find(key);
    const NodePtr live = live_it == live_.end() ? nullptr : live_it->second;
    const NodePtr durable = durable_it == durable_.end() ? nullptr : durable_it->second;

    std::optional<std::string> contents;  // nullopt: no file under this name
    if (kind == CrashKind::kProcessCrash) {
      if (live) {
        // Only bytes the process never handed to the OS are lost. The rest
        // stays in the OS's cache, exactly as unsynced as before.
        contents = live->data.substr(0, static_cast<std::size_t>(live->flushed));
      }
    } else if (live == durable) {
      if (live) {
        contents = PowerLossContents(*live);
      }
    } else {
      // The name's latest change was never made durable: the disk may hold
      // the old state or the new one.
      const NodePtr& chosen = Chance(0.5) ? durable : live;
      if (chosen) {
        contents = PowerLossContents(*chosen);
      }
    }

    const std::filesystem::path path(key);
    CrashedFile record{
        .path = ToUtf8(path), .outcome = CrashedFile::Outcome::kUnchanged, .size = 0};
    Status status;
    if (!contents) {
      if (!live) {
        continue;  // already absent
      }
      status = base_->RemoveFile(path);
      record.outcome = CrashedFile::Outcome::kRemoved;
    } else if (live && *contents == live->data) {
      record.size = contents->size();
    } else {
      status = WriteWholeFile(*base_, path, *contents);
      record.outcome = CrashedFile::Outcome::kRewritten;
      record.size = contents->size();
    }
    if (kind == CrashKind::kProcessCrash && live) {
      live->data.resize(static_cast<std::size_t>(live->flushed));  // tracking goes on
    }
    if (!status.ok() && first_error.ok()) {
      first_error = status;
    }
    last_crash_.push_back(std::move(record));
  }

  if (kind == CrashKind::kPowerLoss) {
    // Everything on disk now is, by definition, durable.
    live_.clear();
    durable_.clear();
  }
  // After a process crash, nothing became more durable: what was not synced
  // (data, new names, renames, removals) can still be lost by a power loss.
  crash_at_ = 0;
  dead_.store(false);
  return first_error;
}

}  // namespace forgedb
