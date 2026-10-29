// GatedFileSystem: a FileSystem wrapper whose table syncs (files ending in
// .sst) wait at a Gate while it is closed. Tests use it to keep a flush or a
// compaction running for as long as they need (TODO 4.2, 4.3).
//
//   Gate gate;
//   auto fs = std::make_shared<GatedFileSystem>(std::make_shared<MemFileSystem>(), &gate);
//   gate.Close();           // the next table sync waits
//   gate.WaitForArrival();  // ... until one does
//   gate.Open();            // let it (and every later one) through
//
// Everything else is forwarded unchanged. The gate must outlive the file
// system's files.
#pragma once

#include <condition_variable>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "forgedb/result.h"
#include "forgedb/status.h"

#include "env/file_system.h"

namespace forgedb {

class Gate {
 public:
  // Closes the gate after letting the next `free_passes` syncs through.
  void Close(int free_passes = 0);
  // Opens the gate; the held syncs return `result` (and so do later ones,
  // until the next Close or Open).
  void Open(Status result = Status::OK());
  // Waits until a sync is held at the gate.
  void WaitForArrival();
  // Called by every table sync.
  [[nodiscard]] Status Pass();

 private:
  std::mutex mutex_;
  std::condition_variable cv_;
  bool open_ = true;
  int free_passes_ = 0;
  int waiting_ = 0;
  Status result_;
};

class GatedFileSystem final : public FileSystem {
 public:
  GatedFileSystem(std::shared_ptr<FileSystem> base, Gate* gate);

  Result<std::unique_ptr<WritableFile>> NewWritableFile(const std::filesystem::path& path) override;
  Result<std::unique_ptr<SequentialFile>> NewSequentialFile(
      const std::filesystem::path& path) override;
  Result<std::unique_ptr<RandomAccessFile>> NewRandomAccessFile(
      const std::filesystem::path& path) override;
  Status RenameReplace(const std::filesystem::path& from, const std::filesystem::path& to) override;
  Status RemoveFile(const std::filesystem::path& path) override;
  Result<bool> FileExists(const std::filesystem::path& path) override;
  Result<std::vector<std::string>> GetChildren(const std::filesystem::path& dir) override;
  Result<std::uint64_t> GetFileSize(const std::filesystem::path& path) override;
  Status SyncFile(const std::filesystem::path& path) override;
  Status CreateDir(const std::filesystem::path& path) override;
  Result<std::unique_ptr<FileLock>> LockFile(const std::filesystem::path& path) override;
  Status UnlockFile(std::unique_ptr<FileLock> lock) override;

 private:
  std::shared_ptr<FileSystem> base_;
  Gate* gate_;
};

}  // namespace forgedb
