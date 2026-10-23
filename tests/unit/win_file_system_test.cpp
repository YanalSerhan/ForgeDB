// Windows-only behavior of src/env/: UniqueHandle, UTF-8 conversion, error
// messages, and the retry around delete. (The shared FileSystem contract is in
// file_system_conformance_test.cpp.) Tests may call Win32 directly; the
// "Win32 only in src/env/" rule is about the library.
#include "env/win_file_system.h"

#include <windows.h>

#include <algorithm>
#include <chrono>
#include <filesystem>

#include <gtest/gtest.h>

#include <memory>
#include <random>
#include <string>
#include <system_error>
#include <thread>
#include <vector>

#include "env/mem_file_system.h"
#include "env/unique_handle.h"
#include "env/win_util.h"

namespace forgedb {
namespace {

// A fresh directory under %TEMP%, removed when the test ends.
class TempDir {
 public:
  TempDir() {
    std::random_device random;
    path_ = std::filesystem::temp_directory_path() /
            ("forgedb-wintest-" + std::to_string(random()) + "-" + std::to_string(random()));
    std::filesystem::create_directories(path_);
  }
  ~TempDir() {
    std::error_code ignored;
    std::filesystem::remove_all(path_, ignored);
  }
  TempDir(const TempDir&) = delete;
  TempDir& operator=(const TempDir&) = delete;
  TempDir(TempDir&&) = delete;
  TempDir& operator=(TempDir&&) = delete;

  [[nodiscard]] const std::filesystem::path& Path() const { return path_; }

 private:
  std::filesystem::path path_;
};

void WriteFile(FileSystem& fs, const std::filesystem::path& path, std::string_view contents) {
  Result<std::unique_ptr<WritableFile>> file = fs.NewWritableFile(path);
  ASSERT_TRUE(file.ok()) << file.status().ToString();
  ASSERT_TRUE((*file)->Append(contents).ok());
  ASSERT_TRUE((*file)->Close().ok());
}

bool IsOpenHandle(HANDLE handle) {
  DWORD flags = 0;
  return ::GetHandleInformation(handle, &flags) != FALSE;
}

// --- UniqueHandle ------------------------------------------------------------

TEST(UniqueHandleTest, ClosesOnDestruction) {
  HANDLE raw = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
  ASSERT_NE(raw, nullptr);
  {
    const UniqueHandle owner(raw);
    EXPECT_TRUE(owner.IsValid());
    EXPECT_TRUE(IsOpenHandle(raw));
  }
  EXPECT_FALSE(IsOpenHandle(raw));
}

TEST(UniqueHandleTest, MoveTransfersOwnership) {
  UniqueHandle first(::CreateEventW(nullptr, TRUE, FALSE, nullptr));
  HANDLE raw_first = first.Get();
  UniqueHandle second(std::move(first));
  // Checking the moved-from state is the point here.
  // NOLINTNEXTLINE(bugprone-use-after-move,clang-analyzer-cplusplus.Move)
  EXPECT_FALSE(first.IsValid());
  EXPECT_EQ(second.Get(), raw_first);

  UniqueHandle third(::CreateEventW(nullptr, TRUE, FALSE, nullptr));
  HANDLE raw_third = third.Get();
  third = std::move(second);  // closes the event third owned
  EXPECT_FALSE(IsOpenHandle(raw_third));
  EXPECT_TRUE(IsOpenHandle(raw_first));
}

TEST(UniqueHandleTest, DefaultAndInvalidHandlesAreNotValid) {
  EXPECT_FALSE(UniqueHandle().IsValid());
  EXPECT_FALSE(UniqueHandle(INVALID_HANDLE_VALUE).IsValid());
  EXPECT_FALSE(UniqueHandle(nullptr).IsValid());
}

// --- UTF-8 and error messages ---------------------------------------------------

TEST(WinUtilTest, WideToUtf8) {
  EXPECT_EQ(win::WideToUtf8(L""), "");
  EXPECT_EQ(win::WideToUtf8(L"000005.log"), "000005.log");
  EXPECT_EQ(win::WideToUtf8(L"caf\u00e9"), "caf\xC3\xA9");
  EXPECT_EQ(win::WideToUtf8(L"\U0001F600"), "\xF0\x9F\x98\x80");  // surrogate pair
  const std::wstring unpaired(1, static_cast<wchar_t>(0xD800));
  EXPECT_EQ(win::WideToUtf8(unpaired), "\xEF\xBF\xBD");  // U+FFFD, not a failure
}

TEST(WinUtilTest, ErrorMessageHasNoTrailingNewline) {
  const std::string message = win::ErrorMessage(ERROR_FILE_NOT_FOUND);
  ASSERT_FALSE(message.empty());
  EXPECT_NE(message.back(), '\n');
  EXPECT_NE(message.back(), '\r');
}

TEST(WinUtilTest, ErrorStatusMapsCodes) {
  const std::filesystem::path path = L"C:\\db\\000005.log";
  EXPECT_EQ(win::ErrorStatus(ERROR_FILE_NOT_FOUND, "op", path).code(), Status::Code::kNotFound);
  EXPECT_EQ(win::ErrorStatus(ERROR_PATH_NOT_FOUND, "op", path).code(), Status::Code::kNotFound);
  EXPECT_EQ(win::ErrorStatus(ERROR_SHARING_VIOLATION, "op", path).code(), Status::Code::kBusy);
  EXPECT_EQ(win::ErrorStatus(ERROR_LOCK_VIOLATION, "op", path).code(), Status::Code::kBusy);
  EXPECT_EQ(win::ErrorStatus(ERROR_DISK_FULL, "op", path).code(), Status::Code::kIOError);

  const Status status = win::ErrorStatus(ERROR_FILE_NOT_FOUND, "ReadFile", path);
  EXPECT_NE(status.message().find("ReadFile 'C:\\db\\000005.log'"), std::string::npos)
      << status.message();
  EXPECT_NE(status.message().find("(Win32 error 2)"), std::string::npos) << status.message();
}

TEST(WinFileSystemTest, ErrorsNameThePath) {
  const TempDir dir;
  WinFileSystem fs;
  const Status status = fs.GetFileSize(dir.Path() / "missing-file").status();
  EXPECT_EQ(status.code(), Status::Code::kNotFound);
  EXPECT_NE(status.message().find("missing-file"), std::string::npos) << status.message();
}

TEST(WinFileSystemTest, NonAsciiNamesComeBackAsUtf8) {
  const TempDir dir;
  WinFileSystem fs;
  WriteFile(fs, dir.Path() / L"caf\u00e9.log", "x");
  Result<std::vector<std::string>> children = fs.GetChildren(dir.Path());
  ASSERT_TRUE(children.ok()) << children.status().ToString();
  EXPECT_EQ(*children, (std::vector<std::string>{"caf\xC3\xA9.log"}));
}

// --- Retry around delete -----------------------------------------------------

// Opens `path` the way a virus scanner might: reading, without FILE_SHARE_DELETE.
UniqueHandle OpenWithoutShareDelete(const std::filesystem::path& path) {
  return UniqueHandle(::CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                                    OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));
}

TEST(WinFileSystemTest, RemoveRetriesUntilAScannerLetsGo) {
  const TempDir dir;
  WinFileSystem fs;
  const auto path = dir.Path() / "scanned";
  WriteFile(fs, path, "x");
  UniqueHandle scanner = OpenWithoutShareDelete(path);
  ASSERT_TRUE(scanner.IsValid());

  // The "scanner" closes its handle after 50 ms, well inside the ~0.5 s of retries.
  std::thread release([&scanner] {
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    scanner.Reset();
  });
  const Status status = fs.RemoveFile(path);
  release.join();
  EXPECT_TRUE(status.ok()) << status.ToString();
  EXPECT_FALSE(fs.FileExists(path).value());
}

TEST(WinFileSystemTest, RemoveGivesUpWhileTheFileStaysOpen) {
  const TempDir dir;
  WinFileSystem fs;
  const auto path = dir.Path() / "held";
  WriteFile(fs, path, "x");
  const UniqueHandle holder = OpenWithoutShareDelete(path);
  ASSERT_TRUE(holder.IsValid());

  const auto start = std::chrono::steady_clock::now();
  const Status status = fs.RemoveFile(path);
  const auto elapsed = std::chrono::steady_clock::now() - start;
  EXPECT_EQ(status.code(), Status::Code::kBusy) << status.ToString();
  EXPECT_LT(elapsed, std::chrono::seconds(5)) << "retries must be bounded";
  EXPECT_TRUE(fs.FileExists(path).value());
}

TEST(WinFileSystemTest, UnlockRejectsForeignLocks) {
  MemFileSystem mem;
  WinFileSystem win;
  Result<std::unique_ptr<FileLock>> lock = mem.LockFile("LOCK");
  ASSERT_TRUE(lock.ok());
  EXPECT_EQ(win.UnlockFile(std::move(lock).value()).code(), Status::Code::kInvalidArgument);
}

}  // namespace
}  // namespace forgedb
