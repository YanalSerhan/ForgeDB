#include "env/win_file_system.h"

#include <windows.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <iterator>
#include <limits>
#include <memory>
#include <string>
#include <string_view>
#include <thread>
#include <utility>

#include "env/unique_handle.h"
#include "env/win_util.h"

namespace forgedb {
namespace {

using win::ErrorStatus;
using win::PathToUtf8;

// ReadFile/WriteFile take a DWORD length; larger requests are split into
// chunks of at most 1 GiB.
constexpr std::size_t kMaxIoChunk = std::size_t{1} << 30;

// WinWritableFile's buffer: small appends (log records) are collected here and
// reach the operating system as one WriteFile call.
constexpr std::size_t kWriteBufferSize = std::size_t{64} << 10;

// Sharing modes (docs/adr/0002):
//  - Readers and writers allow FILE_SHARE_DELETE, so a file can be deleted or
//    renamed over while another handle (e.g. the table cache) has it open.
//  - Writers allow readers and readers allow a writer, so a file can be read
//    while it is being written. Two writers can't coexist: a writer doesn't
//    grant FILE_SHARE_WRITE.
//  - The LOCK file does NOT allow FILE_SHARE_DELETE: with POSIX delete
//    semantics another process could otherwise delete the locked file, create
//    a new LOCK file under the same name, and lock that one as well.
constexpr DWORD kWriterShareMode = FILE_SHARE_READ | FILE_SHARE_DELETE;
constexpr DWORD kReaderShareMode = FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE;
constexpr DWORD kLockShareMode = FILE_SHARE_READ | FILE_SHARE_WRITE;

// ---------------------------------------------------------------------------
// Bounded retry for rename and delete.
//
// Antivirus scanners, the search indexer and backup tools briefly open files
// without FILE_SHARE_DELETE. Deleting or renaming such a file fails with
// ERROR_SHARING_VIOLATION or ERROR_ACCESS_DENIED until they let go. Retrying
// with exponential backoff (1, 2, 4, ... 256 ms: 10 attempts, about half a
// second of waiting) rides out these interruptions. A lasting problem (missing
// permission, a program that keeps the file open) still fails, just later.
// ---------------------------------------------------------------------------
constexpr int kMaxAttempts = 10;

bool IsTransientSharingError(DWORD error) noexcept {
  return error == ERROR_SHARING_VIOLATION || error == ERROR_ACCESS_DENIED;
}

// `operation` returns ERROR_SUCCESS or a Win32 error code.
template <typename Operation>
DWORD RetryWhileShared(const Operation& operation) {
  DWORD error = operation();
  auto delay = std::chrono::milliseconds(1);
  for (int attempt = 1; attempt < kMaxAttempts && IsTransientSharingError(error); ++attempt) {
    std::this_thread::sleep_for(delay);
    delay *= 2;
    error = operation();
  }
  return error;
}

// ---------------------------------------------------------------------------
// POSIX-style delete.
//
// Classic Windows deletion only marks an open file "delete on close": its name
// stays visible, and creating a file with that name fails, until the last
// handle closes. With FILE_DISPOSITION_POSIX_SEMANTICS the name disappears as
// soon as the deleting handle closes while existing handles keep working.
// ForgeDB relies on that: compaction deletes table files the table cache may
// still have open, and file names are reused.
//
// The Windows SDK declares FILE_DISPOSITION_INFO_EX and FileDispositionInfoEx
// only when targeting Windows 10 1607 or later, so ForgeDB carries its own
// copy: a single DWORD of flags (values from FILE_DISPOSITION_INFORMATION_EX)
// and information class 21 (its position in FILE_INFO_BY_HANDLE_CLASS).
// ---------------------------------------------------------------------------
struct DispositionInfoEx {
  DWORD flags;
};
static_assert(sizeof(DispositionInfoEx) == sizeof(DWORD));
constexpr DWORD kDispositionDelete = 0x00000001;
constexpr DWORD kDispositionPosixSemantics = 0x00000002;
constexpr auto kFileDispositionInfoEx = static_cast<FILE_INFO_BY_HANDLE_CLASS>(21);

DWORD DeleteWithPosixSemantics(const std::filesystem::path& path) {
  // DELETE access is all that's needed. FILE_FLAG_OPEN_REPARSE_POINT deletes a
  // symbolic link itself rather than its target, as DeleteFileW does.
  HANDLE handle = ::CreateFileW(path.c_str(), DELETE, kReaderShareMode, nullptr, OPEN_EXISTING,
                                FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
  if (handle == INVALID_HANDLE_VALUE) {
    return ::GetLastError();
  }
  const UniqueHandle file(handle);

  DispositionInfoEx posix{kDispositionDelete | kDispositionPosixSemantics};
  if (::SetFileInformationByHandle(file.Get(), kFileDispositionInfoEx, &posix,
                                   static_cast<DWORD>(sizeof(posix))) != FALSE) {
    return ERROR_SUCCESS;
  }
  const DWORD error = ::GetLastError();
  if (error != ERROR_INVALID_PARAMETER && error != ERROR_NOT_SUPPORTED &&
      error != ERROR_INVALID_FUNCTION) {
    return error;
  }
  // This file system has no POSIX delete (FAT32, some network shares): fall
  // back to classic delete-on-close.
  FILE_DISPOSITION_INFO classic{TRUE};
  if (::SetFileInformationByHandle(file.Get(), FileDispositionInfo, &classic,
                                   static_cast<DWORD>(sizeof(classic))) != FALSE) {
    return ERROR_SUCCESS;
  }
  return ::GetLastError();
}

// Name of a directory entry, bounded by the array even if it lacked a NUL.
std::wstring_view EntryName(const WIN32_FIND_DATAW& data) {
  const wchar_t* begin = std::begin(data.cFileName);
  const wchar_t* end = std::find(begin, std::end(data.cFileName), L'\0');
  return {begin, static_cast<std::size_t>(end - begin)};
}

// ---------------------------------------------------------------------------
// Files
// ---------------------------------------------------------------------------

class WinWritableFile final : public WritableFile {
 public:
  WinWritableFile(UniqueHandle file, std::filesystem::path path)
      : file_(std::move(file)), path_(std::move(path)) {
    buffer_.reserve(kWriteBufferSize);
  }
  ~WinWritableFile() override {
    if (!closed_) {
      static_cast<void>(CloseFile());
    }
  }
  WinWritableFile(const WinWritableFile&) = delete;
  WinWritableFile& operator=(const WinWritableFile&) = delete;
  WinWritableFile(WinWritableFile&&) = delete;
  WinWritableFile& operator=(WinWritableFile&&) = delete;

  Status Append(std::string_view data) override {
    FORGEDB_RETURN_IF_ERROR(CheckWritable());
    if (data.size() <= kWriteBufferSize - buffer_.size()) {
      buffer_.append(data);
      return Status::OK();
    }
    FORGEDB_RETURN_IF_ERROR(WriteBuffer());
    if (data.size() < kWriteBufferSize) {
      buffer_.append(data);
      return Status::OK();
    }
    return WriteToOs(data);  // big appends skip the buffer
  }

  Status Flush() override {
    FORGEDB_RETURN_IF_ERROR(CheckWritable());
    return WriteBuffer();
  }

  Status Sync() override {
    FORGEDB_RETURN_IF_ERROR(CheckWritable());
    FORGEDB_RETURN_IF_ERROR(WriteBuffer());
    // Writes this file's cached data and metadata (such as its size) to the
    // device: the durability point (docs/adr/0002, F2-F4).
    if (::FlushFileBuffers(file_.Get()) == FALSE) {
      return Fail(ErrorStatus(::GetLastError(), "FlushFileBuffers", path_));
    }
    return Status::OK();
  }

  Status Close() override { return CloseFile(); }

 private:
  // Non-virtual, so the destructor can call it.
  Status CloseFile() {
    if (closed_) {
      return ClosedError();
    }
    Status status = error_.ok() ? WriteBuffer() : error_;
    closed_ = true;
    if (::CloseHandle(file_.Release()) == FALSE && status.ok()) {
      status = ErrorStatus(::GetLastError(), "CloseHandle", path_);
    }
    return status;
  }

  [[nodiscard]] Status CheckWritable() const { return closed_ ? ClosedError() : error_; }
  [[nodiscard]] Status ClosedError() const {
    return Status::InvalidArgument("file is closed", PathToUtf8(path_));
  }

  // Records the first error; every later call returns it.
  Status Fail(Status status) {
    error_ = status;
    return status;
  }

  Status WriteBuffer() {
    if (buffer_.empty()) {
      return Status::OK();
    }
    Status status = WriteToOs(buffer_);
    buffer_.clear();
    return status;
  }

  Status WriteToOs(std::string_view data) {
    while (!data.empty()) {
      const auto chunk = static_cast<DWORD>(std::min(data.size(), kMaxIoChunk));
      DWORD written = 0;
      // chunk <= data.size(), so this never reads past `data`.
      // NOLINTNEXTLINE(bugprone-suspicious-stringview-data-usage)
      if (::WriteFile(file_.Get(), data.data(), chunk, &written, nullptr) == FALSE) {
        return Fail(ErrorStatus(::GetLastError(), "WriteFile", path_));
      }
      if (written != chunk) {
        return Fail(Status::IOError("WriteFile '" + PathToUtf8(path_) + "'", "short write"));
      }
      data.remove_prefix(written);
    }
    return Status::OK();
  }

  UniqueHandle file_;
  std::filesystem::path path_;
  std::string buffer_;
  Status error_;
  bool closed_ = false;
};

class WinSequentialFile final : public SequentialFile {
 public:
  WinSequentialFile(UniqueHandle file, std::filesystem::path path)
      : file_(std::move(file)), path_(std::move(path)) {}

  Result<std::size_t> Read(std::span<char> buffer) override {
    std::size_t total = 0;
    while (total < buffer.size()) {
      const auto chunk = static_cast<DWORD>(std::min(buffer.size() - total, kMaxIoChunk));
      DWORD read = 0;
      if (::ReadFile(file_.Get(), buffer.data() + total, chunk, &read, nullptr) == FALSE) {
        const DWORD error = ::GetLastError();
        if (error == ERROR_HANDLE_EOF) {
          break;
        }
        return ErrorStatus(error, "ReadFile", path_);
      }
      if (read == 0) {
        break;  // end of file
      }
      total += read;
    }
    return total;
  }

  Status Skip(std::uint64_t n) override {
    if (n > static_cast<std::uint64_t>(std::numeric_limits<LONGLONG>::max())) {
      return Status::InvalidArgument("Skip distance too large", PathToUtf8(path_));
    }
    LARGE_INTEGER distance{};
    distance.QuadPart = static_cast<LONGLONG>(n);
    if (::SetFilePointerEx(file_.Get(), distance, nullptr, FILE_CURRENT) == FALSE) {
      return ErrorStatus(::GetLastError(), "SetFilePointerEx", path_);
    }
    return Status::OK();
  }

 private:
  UniqueHandle file_;
  std::filesystem::path path_;
};

class WinRandomAccessFile final : public RandomAccessFile {
 public:
  WinRandomAccessFile(UniqueHandle file, std::filesystem::path path)
      : file_(std::move(file)), path_(std::move(path)) {}

  // Each ReadFile names its own offset in an OVERLAPPED structure, so
  // concurrent calls don't depend on (or race on) the handle's shared file
  // position. On a handle opened without FILE_FLAG_OVERLAPPED, Windows runs
  // these synchronous calls one at a time: correct, though not parallel.
  Result<std::size_t> Read(std::uint64_t offset, std::span<char> buffer) const override {
    std::size_t total = 0;
    while (total < buffer.size()) {
      const std::uint64_t position = offset + total;
      OVERLAPPED at{};
      at.Offset = static_cast<DWORD>(position & 0xFFFFFFFFU);
      at.OffsetHigh = static_cast<DWORD>(position >> 32);
      const auto chunk = static_cast<DWORD>(std::min(buffer.size() - total, kMaxIoChunk));
      DWORD read = 0;
      if (::ReadFile(file_.Get(), buffer.data() + total, chunk, &read, &at) == FALSE) {
        const DWORD error = ::GetLastError();
        if (error == ERROR_HANDLE_EOF) {  // the read started at or past the end
          break;
        }
        return ErrorStatus(error, "ReadFile", path_);
      }
      if (read == 0) {
        break;
      }
      total += read;
    }
    return total;
  }

 private:
  UniqueHandle file_;
  std::filesystem::path path_;
};

class WinFileLock final : public FileLock {
 public:
  WinFileLock(UniqueHandle file, std::filesystem::path path)
      : file_(std::move(file)), path_(std::move(path)) {}
  ~WinFileLock() override { static_cast<void>(Unlock()); }
  WinFileLock(const WinFileLock&) = delete;
  WinFileLock& operator=(const WinFileLock&) = delete;
  WinFileLock(WinFileLock&&) = delete;
  WinFileLock& operator=(WinFileLock&&) = delete;

  Status Unlock() {
    if (!file_.IsValid()) {
      return Status::OK();
    }
    Status status;
    // Unlock explicitly before closing: for a closed handle, the LockFileEx
    // documentation says the time until the lock is released "depends upon
    // available system resources".
    OVERLAPPED whole_file{};
    if (::UnlockFileEx(file_.Get(), 0, MAXDWORD, MAXDWORD, &whole_file) == FALSE) {
      status = ErrorStatus(::GetLastError(), "UnlockFileEx", path_);
    }
    if (::CloseHandle(file_.Release()) == FALSE && status.ok()) {
      status = ErrorStatus(::GetLastError(), "CloseHandle", path_);
    }
    return status;
  }

 private:
  UniqueHandle file_;
  std::filesystem::path path_;
};

// Opens an existing file for reading with the reader sharing mode.
Result<UniqueHandle> OpenForReading(const std::filesystem::path& path, DWORD access_hint) {
  HANDLE handle = ::CreateFileW(path.c_str(), GENERIC_READ, kReaderShareMode, nullptr,
                                OPEN_EXISTING, access_hint, nullptr);
  if (handle == INVALID_HANDLE_VALUE) {
    return ErrorStatus(::GetLastError(), "CreateFileW (read)", path);
  }
  return UniqueHandle(handle);
}

}  // namespace

Result<std::unique_ptr<WritableFile>> WinFileSystem::NewWritableFile(
    const std::filesystem::path& path) {
  UniqueHandle file;
  // Also retried: CREATE_ALWAYS on an existing file (e.g. a reused temporary
  // name) meets the same short-lived scanners as delete and rename.
  const DWORD error = RetryWhileShared([&] {
    HANDLE handle = ::CreateFileW(path.c_str(), GENERIC_WRITE, kWriterShareMode, nullptr,
                                  CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    const DWORD result = handle == INVALID_HANDLE_VALUE ? ::GetLastError() : ERROR_SUCCESS;
    file.Reset(handle);
    return result;
  });
  if (error != ERROR_SUCCESS) {
    return ErrorStatus(error, "CreateFileW (write)", path);
  }
  return std::make_unique<WinWritableFile>(std::move(file), path);
}

Result<std::unique_ptr<SequentialFile>> WinFileSystem::NewSequentialFile(
    const std::filesystem::path& path) {
  FORGEDB_ASSIGN_OR_RETURN(UniqueHandle file, OpenForReading(path, FILE_FLAG_SEQUENTIAL_SCAN));
  return std::make_unique<WinSequentialFile>(std::move(file), path);
}

Result<std::unique_ptr<RandomAccessFile>> WinFileSystem::NewRandomAccessFile(
    const std::filesystem::path& path) {
  FORGEDB_ASSIGN_OR_RETURN(UniqueHandle file, OpenForReading(path, FILE_FLAG_RANDOM_ACCESS));
  return std::make_unique<WinRandomAccessFile>(std::move(file), path);
}

Status WinFileSystem::RenameReplace(const std::filesystem::path& from,
                                    const std::filesystem::path& to) {
  const DWORD error = RetryWhileShared([&] {
    return ::MoveFileExW(from.c_str(), to.c_str(),
                         MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != FALSE
               ? static_cast<DWORD>(ERROR_SUCCESS)
               : ::GetLastError();
  });
  if (error != ERROR_SUCCESS) {
    return ErrorStatus(error, "MoveFileExW to '" + PathToUtf8(to) + "' from", from);
  }
  return Status::OK();
}

Status WinFileSystem::RemoveFile(const std::filesystem::path& path) {
  const DWORD error = RetryWhileShared([&] { return DeleteWithPosixSemantics(path); });
  if (error != ERROR_SUCCESS) {
    return ErrorStatus(error, "RemoveFile", path);
  }
  return Status::OK();
}

Result<bool> WinFileSystem::FileExists(const std::filesystem::path& path) {
  if (::GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES) {
    return true;
  }
  const DWORD error = ::GetLastError();
  if (error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND) {
    return false;
  }
  return ErrorStatus(error, "GetFileAttributesW", path);
}

Result<std::vector<std::string>> WinFileSystem::GetChildren(const std::filesystem::path& dir) {
  WIN32_FIND_DATAW entry{};
  const std::filesystem::path pattern = dir / L"*";
  HANDLE handle = ::FindFirstFileExW(pattern.c_str(), FindExInfoBasic, &entry,
                                     FindExSearchNameMatch, nullptr, FIND_FIRST_EX_LARGE_FETCH);
  if (handle == INVALID_HANDLE_VALUE) {
    return ErrorStatus(::GetLastError(), "FindFirstFileExW", dir);
  }
  const UniqueFindHandle find(handle);

  std::vector<std::string> names;
  do {
    const std::wstring_view name = EntryName(entry);
    if (name != L"." && name != L"..") {
      names.push_back(win::WideToUtf8(name));
    }
  } while (::FindNextFileW(find.Get(), &entry) != FALSE);

  if (const DWORD error = ::GetLastError(); error != ERROR_NO_MORE_FILES) {
    return ErrorStatus(error, "FindNextFileW", dir);
  }
  return names;
}

Result<std::uint64_t> WinFileSystem::GetFileSize(const std::filesystem::path& path) {
  WIN32_FILE_ATTRIBUTE_DATA data{};
  if (::GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &data) == FALSE) {
    return ErrorStatus(::GetLastError(), "GetFileAttributesExW", path);
  }
  return (static_cast<std::uint64_t>(data.nFileSizeHigh) << 32) | data.nFileSizeLow;
}

Status WinFileSystem::SyncFile(const std::filesystem::path& path) {
  // FlushFileBuffers needs a handle with GENERIC_WRITE (F2) and flushes the
  // file's cached data, which belongs to the file, not to the handle that
  // wrote it (ADR 0002, rule 13). OPEN_EXISTING: nothing is created or
  // truncated. Retried like the other opens that scanners can block.
  UniqueHandle file;
  const DWORD error = RetryWhileShared([&] {
    HANDLE handle = ::CreateFileW(path.c_str(), GENERIC_WRITE, kWriterShareMode, nullptr,
                                  OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    const DWORD result = handle == INVALID_HANDLE_VALUE ? ::GetLastError() : ERROR_SUCCESS;
    file.Reset(handle);
    return result;
  });
  if (error != ERROR_SUCCESS) {
    return ErrorStatus(error, "CreateFileW (sync)", path);
  }
  if (::FlushFileBuffers(file.Get()) == FALSE) {
    return ErrorStatus(::GetLastError(), "FlushFileBuffers", path);
  }
  if (::CloseHandle(file.Release()) == FALSE) {
    return ErrorStatus(::GetLastError(), "CloseHandle", path);
  }
  return Status::OK();
}

Status WinFileSystem::CreateDir(const std::filesystem::path& path) {
  if (::CreateDirectoryW(path.c_str(), nullptr) != FALSE) {
    return Status::OK();
  }
  const DWORD error = ::GetLastError();
  if (error == ERROR_ALREADY_EXISTS) {
    const DWORD attributes = ::GetFileAttributesW(path.c_str());
    if (attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0) {
      return Status::OK();
    }
    return Status::IOError("CreateDirectoryW '" + PathToUtf8(path) + "'",
                           "exists and is not a directory");
  }
  return ErrorStatus(error, "CreateDirectoryW", path);
}

Result<std::unique_ptr<FileLock>> WinFileSystem::LockFile(const std::filesystem::path& path) {
  HANDLE handle = ::CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE, kLockShareMode, nullptr,
                                OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (handle == INVALID_HANDLE_VALUE) {
    return ErrorStatus(::GetLastError(), "CreateFileW (lock)", path);
  }
  UniqueHandle file(handle);
  // Lock the whole file: offset 0 (from the zeroed OVERLAPPED), length 2^64 - 1.
  // A lock belongs to its handle, so a second lock attempt fails even from
  // this same process (unlike POSIX record locks, which are per process).
  OVERLAPPED whole_file{};
  if (::LockFileEx(file.Get(), LOCKFILE_EXCLUSIVE_LOCK | LOCKFILE_FAIL_IMMEDIATELY, 0, MAXDWORD,
                   MAXDWORD, &whole_file) == FALSE) {
    return ErrorStatus(::GetLastError(), "LockFileEx (database in use?)", path);
  }
  return std::make_unique<WinFileLock>(std::move(file), path);
}

Status WinFileSystem::UnlockFile(std::unique_ptr<FileLock> lock) {
  auto* win_lock = dynamic_cast<WinFileLock*>(lock.get());
  if (win_lock == nullptr) {
    return Status::InvalidArgument("UnlockFile: the lock does not come from this WinFileSystem");
  }
  return win_lock->Unlock();
}

}  // namespace forgedb
