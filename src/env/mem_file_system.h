// FileSystem kept entirely in memory: fast, isolated tests (and, from Phase
// 2.4, the base that FaultInjectionFileSystem wraps).
//
// It follows the same contract as WinFileSystem, checked by the shared
// conformance suite (tests/unit/file_system_conformance_test.cpp):
//   - a file's data lives as long as any handle to it, so removing or renaming
//     over an open file works like POSIX delete on NTFS
//   - Append is visible to readers immediately; Flush and Sync do nothing,
//     since there is no operating system cache to model
//   - a file can only be created in an existing directory. A top-level path
//     (e.g. "db" or "C:\db") counts as being inside an existing directory
//   - paths are compared after lexical normalization, case-sensitively (NTFS
//     is case-insensitive; ForgeDB itself never relies on either behavior)
//
// A FileLock must not outlive the MemFileSystem that created it.
#pragma once

#include <filesystem>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <vector>

#include "env/file_system.h"

namespace forgedb {

class MemFileSystem final : public FileSystem {
 public:
  MemFileSystem() = default;

  [[nodiscard]] Result<std::unique_ptr<WritableFile>> NewWritableFile(
      const std::filesystem::path& path) override;
  [[nodiscard]] Result<std::unique_ptr<SequentialFile>> NewSequentialFile(
      const std::filesystem::path& path) override;
  [[nodiscard]] Result<std::unique_ptr<RandomAccessFile>> NewRandomAccessFile(
      const std::filesystem::path& path) override;
  [[nodiscard]] Status RenameReplace(const std::filesystem::path& from,
                                     const std::filesystem::path& to) override;
  [[nodiscard]] Status RemoveFile(const std::filesystem::path& path) override;
  [[nodiscard]] Result<bool> FileExists(const std::filesystem::path& path) override;
  [[nodiscard]] Result<std::vector<std::string>> GetChildren(
      const std::filesystem::path& dir) override;
  [[nodiscard]] Result<std::uint64_t> GetFileSize(const std::filesystem::path& path) override;
  [[nodiscard]] Status CreateDir(const std::filesystem::path& path) override;
  [[nodiscard]] Result<std::unique_ptr<FileLock>> LockFile(
      const std::filesystem::path& path) override;
  [[nodiscard]] Status UnlockFile(std::unique_ptr<FileLock> lock) override;

  // The contents of a file shared by every handle to it.
  struct FileData {
    std::mutex mutex;
    std::string contents;
  };

 private:
  friend class MemFileLock;
  using Key = std::filesystem::path::string_type;

  [[nodiscard]] static Key KeyOf(const std::filesystem::path& path);
  // Requires mutex_.
  [[nodiscard]] bool ParentExists(const Key& key) const;
  [[nodiscard]] std::shared_ptr<FileData> Find(const Key& key) const;
  void ReleaseLock(const Key& key);

  mutable std::mutex mutex_;
  std::map<Key, std::shared_ptr<FileData>> files_;
  std::set<Key> dirs_;
  std::set<Key> locked_;
};

}  // namespace forgedb
