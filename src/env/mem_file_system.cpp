#include "env/mem_file_system.h"

#include <algorithm>
#include <cstring>
#include <utility>

namespace forgedb {
namespace {

using FileData = MemFileSystem::FileData;

std::string ToUtf8(const std::filesystem::path& path) {
  const std::u8string utf8 = path.u8string();
  return {utf8.begin(), utf8.end()};
}

Status NotFound(std::string_view operation, const std::filesystem::path& path) {
  return Status::NotFound(std::string(operation) + " '" + ToUtf8(path) + "'", "no such file");
}

class MemWritableFile final : public WritableFile {
 public:
  MemWritableFile(std::shared_ptr<FileData> data, std::filesystem::path path)
      : data_(std::move(data)), path_(std::move(path)) {}

  Status Append(std::string_view data) override {
    FORGEDB_RETURN_IF_ERROR(CheckOpen());
    const std::scoped_lock lock(data_->mutex);
    data_->contents.append(data);
    return Status::OK();
  }
  Status Flush() override { return CheckOpen(); }
  Status Sync() override { return CheckOpen(); }
  Status Close() override {
    FORGEDB_RETURN_IF_ERROR(CheckOpen());
    closed_ = true;
    return Status::OK();
  }

 private:
  [[nodiscard]] Status CheckOpen() const {
    return closed_ ? Status::InvalidArgument("file is closed", ToUtf8(path_)) : Status::OK();
  }

  std::shared_ptr<FileData> data_;
  std::filesystem::path path_;
  bool closed_ = false;
};

// Copies up to buffer.size() bytes starting at `offset`; returns the count.
std::size_t ReadAt(FileData& data, std::uint64_t offset, std::span<char> buffer) {
  const std::scoped_lock lock(data.mutex);
  if (offset >= data.contents.size()) {
    return 0;
  }
  const auto start = static_cast<std::size_t>(offset);
  const std::size_t count = std::min(buffer.size(), data.contents.size() - start);
  std::memcpy(buffer.data(), data.contents.data() + start, count);
  return count;
}

class MemSequentialFile final : public SequentialFile {
 public:
  explicit MemSequentialFile(std::shared_ptr<FileData> data) : data_(std::move(data)) {}

  Result<std::size_t> Read(std::span<char> buffer) override {
    const std::size_t count = ReadAt(*data_, position_, buffer);
    position_ += count;
    return count;
  }
  Status Skip(std::uint64_t n) override {
    position_ += n;
    return Status::OK();
  }

 private:
  std::shared_ptr<FileData> data_;
  std::uint64_t position_ = 0;
};

class MemRandomAccessFile final : public RandomAccessFile {
 public:
  explicit MemRandomAccessFile(std::shared_ptr<FileData> data) : data_(std::move(data)) {}

  Result<std::size_t> Read(std::uint64_t offset, std::span<char> buffer) const override {
    return ReadAt(*data_, offset, buffer);
  }

 private:
  std::shared_ptr<FileData> data_;
};

}  // namespace

class MemFileLock final : public FileLock {
 public:
  MemFileLock(MemFileSystem* fs, MemFileSystem::Key key) : fs_(fs), key_(std::move(key)) {}
  ~MemFileLock() override { Release(); }
  MemFileLock(const MemFileLock&) = delete;
  MemFileLock& operator=(const MemFileLock&) = delete;
  MemFileLock(MemFileLock&&) = delete;
  MemFileLock& operator=(MemFileLock&&) = delete;

  void Release() {
    if (fs_ != nullptr) {
      fs_->ReleaseLock(key_);
      fs_ = nullptr;
    }
  }
  [[nodiscard]] const MemFileSystem* Owner() const { return fs_; }

 private:
  MemFileSystem* fs_;
  MemFileSystem::Key key_;
};

MemFileSystem::Key MemFileSystem::KeyOf(const std::filesystem::path& path) {
  return path.lexically_normal().native();
}

bool MemFileSystem::ParentExists(const Key& key) const {
  const std::filesystem::path parent = std::filesystem::path(key).parent_path();
  if (parent.empty() || parent == parent.root_path()) {
    return true;  // a top-level name, or directly under a root such as "C:\"
  }
  return dirs_.contains(parent.native());
}

std::shared_ptr<FileData> MemFileSystem::Find(const Key& key) const {
  const auto it = files_.find(key);
  return it == files_.end() ? nullptr : it->second;
}

void MemFileSystem::ReleaseLock(const Key& key) {
  const std::scoped_lock lock(mutex_);
  locked_.erase(key);
}

Result<std::unique_ptr<WritableFile>> MemFileSystem::NewWritableFile(
    const std::filesystem::path& path) {
  const Key key = KeyOf(path);
  const std::scoped_lock lock(mutex_);
  if (!ParentExists(key)) {
    return NotFound("NewWritableFile: parent directory of", path);
  }
  if (dirs_.contains(key)) {
    return Status::IOError("NewWritableFile '" + ToUtf8(path) + "'", "is a directory");
  }
  // Like CREATE_ALWAYS on Windows, an existing file is truncated in place:
  // handles already open on it see it become empty.
  std::shared_ptr<FileData>& data = files_[key];
  if (data) {
    const std::scoped_lock data_lock(data->mutex);
    data->contents.clear();
  } else {
    data = std::make_shared<FileData>();
  }
  return std::make_unique<MemWritableFile>(data, path);
}

Result<std::unique_ptr<SequentialFile>> MemFileSystem::NewSequentialFile(
    const std::filesystem::path& path) {
  const std::scoped_lock lock(mutex_);
  std::shared_ptr<FileData> data = Find(KeyOf(path));
  if (!data) {
    return NotFound("NewSequentialFile", path);
  }
  return std::make_unique<MemSequentialFile>(std::move(data));
}

Result<std::unique_ptr<RandomAccessFile>> MemFileSystem::NewRandomAccessFile(
    const std::filesystem::path& path) {
  const std::scoped_lock lock(mutex_);
  std::shared_ptr<FileData> data = Find(KeyOf(path));
  if (!data) {
    return NotFound("NewRandomAccessFile", path);
  }
  return std::make_unique<MemRandomAccessFile>(std::move(data));
}

Status MemFileSystem::RenameReplace(const std::filesystem::path& from,
                                    const std::filesystem::path& to) {
  const Key from_key = KeyOf(from);
  const Key to_key = KeyOf(to);
  const std::scoped_lock lock(mutex_);
  const auto it = files_.find(from_key);
  if (it == files_.end()) {
    return NotFound("RenameReplace", from);
  }
  if (!ParentExists(to_key)) {
    return NotFound("RenameReplace: parent directory of", to);
  }
  if (dirs_.contains(to_key)) {
    return Status::IOError("RenameReplace to '" + ToUtf8(to) + "'", "is a directory");
  }
  if (locked_.contains(from_key) || locked_.contains(to_key)) {
    return Status::Busy("RenameReplace '" + ToUtf8(from) + "'", "file is locked");
  }
  if (from_key == to_key) {
    return Status::OK();
  }
  std::shared_ptr<FileData> data = std::move(it->second);
  files_.erase(it);
  files_[to_key] = std::move(data);  // open handles on the old `to` keep its data
  return Status::OK();
}

Status MemFileSystem::RemoveFile(const std::filesystem::path& path) {
  const Key key = KeyOf(path);
  const std::scoped_lock lock(mutex_);
  if (!files_.contains(key)) {
    return NotFound("RemoveFile", path);
  }
  if (locked_.contains(key)) {
    return Status::Busy("RemoveFile '" + ToUtf8(path) + "'", "file is locked");
  }
  files_.erase(key);  // open handles keep the data alive (POSIX semantics)
  return Status::OK();
}

Result<bool> MemFileSystem::FileExists(const std::filesystem::path& path) {
  const Key key = KeyOf(path);
  const std::scoped_lock lock(mutex_);
  return files_.contains(key) || dirs_.contains(key);
}

Result<std::vector<std::string>> MemFileSystem::GetChildren(const std::filesystem::path& dir) {
  const Key key = KeyOf(dir);
  const std::filesystem::path dir_path(key);
  const std::scoped_lock lock(mutex_);
  if (!dirs_.contains(key)) {
    return NotFound("GetChildren", dir);
  }
  std::vector<std::string> names;
  const auto collect = [&](const Key& entry) {
    const std::filesystem::path entry_path(entry);
    if (entry_path.parent_path() == dir_path) {
      names.push_back(ToUtf8(entry_path.filename()));
    }
  };
  for (const auto& [entry, data] : files_) {
    collect(entry);
  }
  for (const Key& entry : dirs_) {
    collect(entry);
  }
  return names;
}

Result<std::uint64_t> MemFileSystem::GetFileSize(const std::filesystem::path& path) {
  std::shared_ptr<FileData> data;
  {
    const std::scoped_lock lock(mutex_);
    data = Find(KeyOf(path));
  }
  if (!data) {
    return NotFound("GetFileSize", path);
  }
  const std::scoped_lock data_lock(data->mutex);
  return static_cast<std::uint64_t>(data->contents.size());
}

Status MemFileSystem::SyncFile(const std::filesystem::path& path) {
  const std::scoped_lock lock(mutex_);
  if (!Find(KeyOf(path))) {
    return NotFound("SyncFile", path);
  }
  return Status::OK();  // memory has no cache to flush
}

Status MemFileSystem::CreateDir(const std::filesystem::path& path) {
  const Key key = KeyOf(path);
  const std::scoped_lock lock(mutex_);
  if (dirs_.contains(key)) {
    return Status::OK();
  }
  if (files_.contains(key)) {
    return Status::IOError("CreateDir '" + ToUtf8(path) + "'", "exists and is not a directory");
  }
  if (!ParentExists(key)) {
    return NotFound("CreateDir: parent directory of", path);
  }
  dirs_.insert(key);
  return Status::OK();
}

Result<std::unique_ptr<FileLock>> MemFileSystem::LockFile(const std::filesystem::path& path) {
  const Key key = KeyOf(path);
  const std::scoped_lock lock(mutex_);
  if (!ParentExists(key)) {
    return NotFound("LockFile: parent directory of", path);
  }
  if (locked_.contains(key)) {
    return Status::Busy("LockFile '" + ToUtf8(path) + "'", "already locked");
  }
  if (!files_.contains(key)) {
    files_[key] = std::make_shared<FileData>();  // like OPEN_ALWAYS
  }
  locked_.insert(key);
  return std::make_unique<MemFileLock>(this, key);
}

Status MemFileSystem::UnlockFile(std::unique_ptr<FileLock> lock) {
  auto* mem_lock = dynamic_cast<MemFileLock*>(lock.get());
  if (mem_lock == nullptr || mem_lock->Owner() != this) {
    return Status::InvalidArgument("UnlockFile: the lock does not come from this MemFileSystem");
  }
  mem_lock->Release();
  return Status::OK();
}

}  // namespace forgedb
