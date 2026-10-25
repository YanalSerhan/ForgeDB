// The file-system abstraction used by everything above src/env/.
//
// Only src/env/ talks to the operating system. The log, tables, MANIFEST and
// recovery code all go through these interfaces, which is what lets tests use
// MemFileSystem (fast, in memory) and, from Phase 2.4, FaultInjectionFileSystem
// (simulated crashes and I/O errors) in place of the real WinFileSystem.
//
// Paths are std::filesystem::path, which stores UTF-16 natively on Windows, so
// no path is ever squeezed through a narrow code page. Names returned to callers
// (GetChildren) are UTF-8.
//
// Thread safety:
//   FileSystem                    every method may be called concurrently
//   RandomAccessFile::Read        may be called concurrently
//   SequentialFile, WritableFile  one thread at a time (callers synchronize)
#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "forgedb/result.h"
#include "forgedb/status.h"

namespace forgedb {

// Reads a file from start to end (log replay, MANIFEST replay).
class SequentialFile {
 public:
  virtual ~SequentialFile() = default;
  SequentialFile(const SequentialFile&) = delete;
  SequentialFile& operator=(const SequentialFile&) = delete;
  SequentialFile(SequentialFile&&) = delete;
  SequentialFile& operator=(SequentialFile&&) = delete;

  // Reads up to buffer.size() bytes at the current position into `buffer` and
  // advances past them. Returns how many bytes were read: fewer than requested
  // only at end of file, 0 once the end is reached.
  [[nodiscard]] virtual Result<std::size_t> Read(std::span<char> buffer) = 0;

  // Advances the position by n bytes. Skipping past the end is allowed; later
  // reads then return 0.
  [[nodiscard]] virtual Status Skip(std::uint64_t n) = 0;

 protected:
  SequentialFile() = default;
};

// Reads at arbitrary offsets (table files). Read() is safe to call from many
// threads at once.
class RandomAccessFile {
 public:
  virtual ~RandomAccessFile() = default;
  RandomAccessFile(const RandomAccessFile&) = delete;
  RandomAccessFile& operator=(const RandomAccessFile&) = delete;
  RandomAccessFile(RandomAccessFile&&) = delete;
  RandomAccessFile& operator=(RandomAccessFile&&) = delete;

  // Reads up to buffer.size() bytes starting at `offset`. Returns how many bytes
  // were read: fewer than requested only if the file ends first, 0 if `offset`
  // is at or past the end.
  [[nodiscard]] virtual Result<std::size_t> Read(std::uint64_t offset,
                                                 std::span<char> buffer) const = 0;

 protected:
  RandomAccessFile() = default;
};

// Appends to a new file (log, table, MANIFEST).
//
// Durability levels:
//   Append  the bytes may still sit in this object's memory buffer
//   Flush   handed to the operating system: they survive a crash of this
//           process and are visible to other readers, but are lost if the
//           operating system crashes or the power fails
//   Sync    Flush, then forced to stable storage: they survive power loss
//           (on a drive that honors flush requests; see docs/adr/0002)
//
// Errors are sticky: after any failed operation, every later call returns the
// first error, because the file's contents on disk are no longer known.
class WritableFile {
 public:
  virtual ~WritableFile() = default;
  WritableFile(const WritableFile&) = delete;
  WritableFile& operator=(const WritableFile&) = delete;
  WritableFile(WritableFile&&) = delete;
  WritableFile& operator=(WritableFile&&) = delete;

  [[nodiscard]] virtual Status Append(std::string_view data) = 0;
  [[nodiscard]] virtual Status Flush() = 0;
  [[nodiscard]] virtual Status Sync() = 0;

  // Flushes and closes. Always call it: it is the only way to learn whether the
  // last writes succeeded. The destructor also closes, but cannot report errors.
  // Every call after Close fails.
  [[nodiscard]] virtual Status Close() = 0;

 protected:
  WritableFile() = default;
};

// An exclusive lock on a file (the database's LOCK file). The lock is held
// until the object is passed to FileSystem::UnlockFile or destroyed.
class FileLock {
 public:
  virtual ~FileLock() = default;
  FileLock(const FileLock&) = delete;
  FileLock& operator=(const FileLock&) = delete;
  FileLock(FileLock&&) = delete;
  FileLock& operator=(FileLock&&) = delete;

 protected:
  FileLock() = default;
};

class FileSystem {
 public:
  virtual ~FileSystem() = default;
  FileSystem(const FileSystem&) = delete;
  FileSystem& operator=(const FileSystem&) = delete;
  FileSystem(FileSystem&&) = delete;
  FileSystem& operator=(FileSystem&&) = delete;

  // Creates `path` (truncating it if it exists) and opens it for appending.
  [[nodiscard]] virtual Result<std::unique_ptr<WritableFile>> NewWritableFile(
      const std::filesystem::path& path) = 0;

  // Opens an existing file for reading. NotFound if it doesn't exist.
  [[nodiscard]] virtual Result<std::unique_ptr<SequentialFile>> NewSequentialFile(
      const std::filesystem::path& path) = 0;
  [[nodiscard]] virtual Result<std::unique_ptr<RandomAccessFile>> NewRandomAccessFile(
      const std::filesystem::path& path) = 0;

  // Renames `from` to `to`, replacing `to` if it exists. Readers never see a
  // missing `to` or a half-written one: they see the old file or the new one.
  [[nodiscard]] virtual Status RenameReplace(const std::filesystem::path& from,
                                             const std::filesystem::path& to) = 0;

  // Deletes a file. Its name disappears immediately, even while the file is
  // still open elsewhere (open handles keep working), so the name can be reused
  // at once. NotFound if it doesn't exist.
  // (Not called DeleteFile: <windows.h> defines DeleteFile as a macro.)
  [[nodiscard]] virtual Status RemoveFile(const std::filesystem::path& path) = 0;

  [[nodiscard]] virtual Result<bool> FileExists(const std::filesystem::path& path) = 0;

  // The names (not full paths; UTF-8) of the entries in `dir`, without "." and
  // "..", in no particular order. NotFound if `dir` doesn't exist.
  [[nodiscard]] virtual Result<std::vector<std::string>> GetChildren(
      const std::filesystem::path& dir) = 0;

  [[nodiscard]] virtual Result<std::uint64_t> GetFileSize(const std::filesystem::path& path) = 0;

  // Forces an existing file's contents and size to stable storage, the same
  // guarantee as WritableFile::Sync, without changing the file. For files
  // written earlier and no longer open, e.g. logs replayed by recovery.
  // NotFound if it doesn't exist. Do not call it on a file that is open for
  // writing (on Windows that open would conflict).
  [[nodiscard]] virtual Status SyncFile(const std::filesystem::path& path) = 0;

  // Creates a directory; OK if it already exists. Its parent must exist.
  [[nodiscard]] virtual Status CreateDir(const std::filesystem::path& path) = 0;

  // Creates `path` if needed and locks it exclusively. Busy if anyone else holds
  // the lock, whether another process or another lock in this process. While
  // locked, the file cannot be removed or renamed.
  [[nodiscard]] virtual Result<std::unique_ptr<FileLock>> LockFile(
      const std::filesystem::path& path) = 0;

  // Releases a lock obtained from this FileSystem and reports any error.
  // (Destroying the FileLock also releases it, silently.)
  [[nodiscard]] virtual Status UnlockFile(std::unique_ptr<FileLock> lock) = 0;

 protected:
  FileSystem() = default;
};

}  // namespace forgedb
