// FileSystem on the Win32 API (NTFS).
//
// The durability rules it implements are explained, with links to Microsoft's
// documentation, in docs/adr/0002-durability-on-windows.md.
//
// This header deliberately does not include <windows.h>; only the .cpp does.
#pragma once

#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include "env/file_system.h"

namespace forgedb {

class WinFileSystem final : public FileSystem {
 public:
  WinFileSystem() = default;

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
};

}  // namespace forgedb
