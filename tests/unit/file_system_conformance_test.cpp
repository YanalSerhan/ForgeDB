// The FileSystem contract, checked against every implementation.
//
// Each test runs once per implementation (typed tests). WinFileSystem works in
// a fresh directory under %TEMP%; MemFileSystem in a fresh in-memory tree. A
// behavior both must share belongs here; Windows-only details go in
// win_file_system_test.cpp.
#include <algorithm>
#include <atomic>
#include <cstdint>
#include <filesystem>

#include <gtest/gtest.h>

#include <memory>
#include <random>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <vector>

#include "env/file_system.h"
#include "env/mem_file_system.h"
#include "env/win_file_system.h"

namespace forgedb {
namespace {

// --- Implementations under test ---------------------------------------------

struct WinFileSystemSetup {
  static constexpr const char* kName = "Win";
  static std::unique_ptr<FileSystem> MakeFileSystem() { return std::make_unique<WinFileSystem>(); }
  static std::filesystem::path MakeRoot() {
    static std::atomic<int> counter{0};
    std::random_device random;
    return std::filesystem::temp_directory_path() /
           ("forgedb-test-" + std::to_string(random()) + "-" + std::to_string(counter++));
  }
  static void RemoveRoot(const std::filesystem::path& root) {
    std::error_code ignored;
    std::filesystem::remove_all(root, ignored);
  }
};

struct MemFileSystemSetup {
  static constexpr const char* kName = "Mem";
  static std::unique_ptr<FileSystem> MakeFileSystem() { return std::make_unique<MemFileSystem>(); }
  static std::filesystem::path MakeRoot() { return "memroot"; }
  static void RemoveRoot(const std::filesystem::path& /*root*/) {}
};

// --- Helpers ----------------------------------------------------------------

::testing::AssertionResult IsOk(const Status& status) {
  if (status.ok()) {
    return ::testing::AssertionSuccess();
  }
  return ::testing::AssertionFailure() << status.ToString();
}

::testing::AssertionResult HasCode(const Status& status, Status::Code code) {
  if (status.code() == code) {
    return ::testing::AssertionSuccess();
  }
  return ::testing::AssertionFailure()
         << "expected " << CodeName(code) << ", got " << status.ToString();
}

std::string RandomBytes(std::size_t size, std::uint32_t seed) {
  std::mt19937 rng(seed);
  std::string data(size, '\0');
  for (char& c : data) {
    c = static_cast<char>(rng());
  }
  return data;
}

template <typename Setup>
class FileSystemConformanceTest : public ::testing::Test {
 protected:
  void SetUp() override {
    fs_ = Setup::MakeFileSystem();
    root_ = Setup::MakeRoot();
    ASSERT_TRUE(IsOk(fs_->CreateDir(root_)));
  }
  void TearDown() override { Setup::RemoveRoot(root_); }

  [[nodiscard]] std::filesystem::path Path(std::string_view name) const {
    return root_ / std::filesystem::path(name);
  }

  void WriteFile(const std::filesystem::path& path, std::string_view contents) {
    Result<std::unique_ptr<WritableFile>> file = fs_->NewWritableFile(path);
    ASSERT_TRUE(IsOk(file.status()));
    ASSERT_TRUE(IsOk((*file)->Append(contents)));
    ASSERT_TRUE(IsOk((*file)->Close()));
  }

  // Reads the whole file through SequentialFile, in small odd-sized pieces.
  Result<std::string> ReadAll(const std::filesystem::path& path) {
    FORGEDB_ASSIGN_OR_RETURN(const std::unique_ptr<SequentialFile> file,
                             fs_->NewSequentialFile(path));
    std::string contents;
    std::string buffer(7, '\0');
    while (true) {
      FORGEDB_ASSIGN_OR_RETURN(const std::size_t count, file->Read(buffer));
      if (count == 0) {
        return contents;
      }
      contents.append(buffer.data(), count);
    }
  }

  [[nodiscard]] std::string ReadAt(const RandomAccessFile& file, std::uint64_t offset,
                                   std::size_t size) const {
    std::string buffer(size, '\0');
    const Result<std::size_t> count = file.Read(offset, buffer);
    EXPECT_TRUE(IsOk(count.status()));
    buffer.resize(count.ok() ? *count : 0);
    return buffer;
  }

  std::unique_ptr<FileSystem> fs_;
  std::filesystem::path root_;
};

class SetupNames {
 public:
  template <typename T>
  static std::string GetName(int /*index*/) {
    return T::kName;
  }
};

using Implementations = ::testing::Types<WinFileSystemSetup, MemFileSystemSetup>;
TYPED_TEST_SUITE(FileSystemConformanceTest, Implementations, SetupNames);

// --- Writing and reading -----------------------------------------------------

TYPED_TEST(FileSystemConformanceTest, AppendThenReadBack) {
  const auto path = this->Path("file");
  this->WriteFile(path, "hello, world");
  EXPECT_EQ(this->ReadAll(path).value(), "hello, world");
  EXPECT_EQ(this->fs_->GetFileSize(path).value(), 12U);
}

TYPED_TEST(FileSystemConformanceTest, EmptyFile) {
  const auto path = this->Path("empty");
  this->WriteFile(path, "");
  EXPECT_EQ(this->ReadAll(path).value(), "");
  EXPECT_EQ(this->fs_->GetFileSize(path).value(), 0U);
}

TYPED_TEST(FileSystemConformanceTest, ManyAppendsOfMixedSizes) {
  // Sizes from 1 byte to well past WinWritableFile's 64 KiB buffer, so data
  // goes through the buffer, around it, and both in one file.
  const std::string expected = RandomBytes(1'500'000, 1);
  std::mt19937 rng(2);
  std::uniform_int_distribution<std::size_t> size(1, 200'000);
  const auto path = this->Path("mixed");
  Result<std::unique_ptr<WritableFile>> file = this->fs_->NewWritableFile(path);
  ASSERT_TRUE(IsOk(file.status()));
  std::string_view rest = expected;
  while (!rest.empty()) {
    const std::size_t n = std::min(rest.size(), rng() % 4 == 0 ? size(rng) : (size(rng) % 300) + 1);
    ASSERT_TRUE(IsOk((*file)->Append(rest.substr(0, n))));
    rest.remove_prefix(n);
  }
  ASSERT_TRUE(IsOk((*file)->Close()));
  EXPECT_EQ(this->fs_->GetFileSize(path).value(), expected.size());
  EXPECT_TRUE(this->ReadAll(path).value() == expected);
}

TYPED_TEST(FileSystemConformanceTest, NewWritableFileTruncates) {
  const auto path = this->Path("file");
  this->WriteFile(path, "a longer first version");
  this->WriteFile(path, "v2");
  EXPECT_EQ(this->ReadAll(path).value(), "v2");
}

TYPED_TEST(FileSystemConformanceTest, SyncKeepsDataAndFileUsable) {
  const auto path = this->Path("synced");
  Result<std::unique_ptr<WritableFile>> file = this->fs_->NewWritableFile(path);
  ASSERT_TRUE(IsOk(file.status()));
  ASSERT_TRUE(IsOk((*file)->Append("first")));
  ASSERT_TRUE(IsOk((*file)->Sync()));
  ASSERT_TRUE(IsOk((*file)->Append(" second")));
  ASSERT_TRUE(IsOk((*file)->Sync()));
  ASSERT_TRUE(IsOk((*file)->Close()));
  EXPECT_EQ(this->ReadAll(path).value(), "first second");
}

TYPED_TEST(FileSystemConformanceTest, FlushMakesDataVisibleToReaders) {
  const auto path = this->Path("growing");
  Result<std::unique_ptr<WritableFile>> writer = this->fs_->NewWritableFile(path);
  ASSERT_TRUE(IsOk(writer.status()));
  ASSERT_TRUE(IsOk((*writer)->Append("abc")));
  ASSERT_TRUE(IsOk((*writer)->Flush()));

  // Opened while the writer is still open.
  Result<std::unique_ptr<RandomAccessFile>> reader = this->fs_->NewRandomAccessFile(path);
  ASSERT_TRUE(IsOk(reader.status()));
  EXPECT_EQ(this->ReadAt(**reader, 0, 10), "abc");

  ASSERT_TRUE(IsOk((*writer)->Append("def")));
  ASSERT_TRUE(IsOk((*writer)->Flush()));
  EXPECT_EQ(this->ReadAt(**reader, 3, 10), "def");
  ASSERT_TRUE(IsOk((*writer)->Close()));
}

TYPED_TEST(FileSystemConformanceTest, ClosedFileRejectsEverything) {
  Result<std::unique_ptr<WritableFile>> file = this->fs_->NewWritableFile(this->Path("f"));
  ASSERT_TRUE(IsOk(file.status()));
  ASSERT_TRUE(IsOk((*file)->Close()));
  EXPECT_FALSE((*file)->Append("x").ok());
  EXPECT_FALSE((*file)->Flush().ok());
  EXPECT_FALSE((*file)->Sync().ok());
  EXPECT_FALSE((*file)->Close().ok());
}

// --- Sequential and positional reads ------------------------------------------

TYPED_TEST(FileSystemConformanceTest, SequentialReadAndSkip) {
  const auto path = this->Path("digits");
  this->WriteFile(path, "0123456789");
  Result<std::unique_ptr<SequentialFile>> file = this->fs_->NewSequentialFile(path);
  ASSERT_TRUE(IsOk(file.status()));
  std::string buffer(3, '\0');
  ASSERT_EQ((*file)->Read(buffer).value(), 3U);
  EXPECT_EQ(buffer, "012");
  ASSERT_TRUE(IsOk((*file)->Skip(4)));
  buffer.assign(10, '\0');
  ASSERT_EQ((*file)->Read(buffer).value(), 3U);  // short read only at the end
  EXPECT_EQ(buffer.substr(0, 3), "789");
  EXPECT_EQ((*file)->Read(buffer).value(), 0U);
  ASSERT_TRUE(IsOk((*file)->Skip(100)));  // past the end is allowed
  EXPECT_EQ((*file)->Read(buffer).value(), 0U);
}

TYPED_TEST(FileSystemConformanceTest, PositionalReads) {
  const std::string contents = RandomBytes(100'000, 3);
  const auto path = this->Path("data");
  this->WriteFile(path, contents);
  Result<std::unique_ptr<RandomAccessFile>> file = this->fs_->NewRandomAccessFile(path);
  ASSERT_TRUE(IsOk(file.status()));

  std::mt19937 rng(4);
  for (int i = 0; i < 200; ++i) {
    const std::size_t offset = rng() % contents.size();
    const std::size_t size = rng() % 5000;
    const std::size_t expected = std::min(size, contents.size() - offset);
    EXPECT_TRUE(this->ReadAt(**file, offset, size) == contents.substr(offset, expected));
  }
  // Straddling the end: short. At or past the end: empty.
  EXPECT_EQ(this->ReadAt(**file, contents.size() - 5, 100).size(), 5U);
  EXPECT_EQ(this->ReadAt(**file, contents.size(), 100), "");
  EXPECT_EQ(this->ReadAt(**file, contents.size() + 1000, 100), "");
}

TYPED_TEST(FileSystemConformanceTest, ConcurrentPositionalReads) {
  const std::string contents = RandomBytes(200'000, 5);
  const auto path = this->Path("shared");
  this->WriteFile(path, contents);
  Result<std::unique_ptr<RandomAccessFile>> opened = this->fs_->NewRandomAccessFile(path);
  ASSERT_TRUE(IsOk(opened.status()));
  const RandomAccessFile& file = **opened;

  // GoogleTest assertions aren't thread-safe on Windows, so threads only count.
  std::atomic<int> mismatches{0};
  std::atomic<int> errors{0};
  constexpr std::uint32_t kThreads = 4;
  std::vector<std::thread> threads;
  threads.reserve(kThreads);
  for (std::uint32_t t = 0; t < kThreads; ++t) {
    threads.emplace_back([&, t] {
      std::mt19937 rng(100 + t);
      std::string buffer;
      for (int i = 0; i < 500; ++i) {
        const std::size_t offset = rng() % contents.size();
        buffer.assign((rng() % 4096) + 1, '\0');
        const Result<std::size_t> count = file.Read(offset, buffer);
        if (!count.ok()) {
          ++errors;
          continue;
        }
        if (std::string_view(buffer.data(), *count) !=
                std::string_view(contents).substr(offset, *count) ||
            *count != std::min(buffer.size(), contents.size() - offset)) {
          ++mismatches;
        }
      }
    });
  }
  for (std::thread& thread : threads) {
    thread.join();
  }
  EXPECT_EQ(errors.load(), 0);
  EXPECT_EQ(mismatches.load(), 0);
}

// --- Missing files -----------------------------------------------------------

TYPED_TEST(FileSystemConformanceTest, MissingFileErrors) {
  const auto path = this->Path("does-not-exist");
  EXPECT_TRUE(HasCode(this->fs_->NewSequentialFile(path).status(), Status::Code::kNotFound));
  EXPECT_TRUE(HasCode(this->fs_->NewRandomAccessFile(path).status(), Status::Code::kNotFound));
  EXPECT_TRUE(HasCode(this->fs_->GetFileSize(path).status(), Status::Code::kNotFound));
  EXPECT_TRUE(HasCode(this->fs_->RemoveFile(path), Status::Code::kNotFound));
  EXPECT_TRUE(HasCode(this->fs_->RenameReplace(path, this->Path("x")), Status::Code::kNotFound));
  EXPECT_FALSE(this->fs_->FileExists(path).value());
}

TYPED_TEST(FileSystemConformanceTest, CreateInMissingDirectoryFails) {
  const auto path = this->Path("no-such-dir") / "file";
  EXPECT_TRUE(HasCode(this->fs_->NewWritableFile(path).status(), Status::Code::kNotFound));
}

// --- Rename ------------------------------------------------------------------

TYPED_TEST(FileSystemConformanceTest, RenameToNewName) {
  this->WriteFile(this->Path("a"), "contents");
  ASSERT_TRUE(IsOk(this->fs_->RenameReplace(this->Path("a"), this->Path("b"))));
  EXPECT_FALSE(this->fs_->FileExists(this->Path("a")).value());
  EXPECT_EQ(this->ReadAll(this->Path("b")).value(), "contents");
}

TYPED_TEST(FileSystemConformanceTest, RenameReplacesExistingTarget) {
  // The CURRENT-file pattern: write a temporary file, then rename it over the
  // old one.
  this->WriteFile(this->Path("CURRENT"), "MANIFEST-000001\n");
  this->WriteFile(this->Path("CURRENT.tmp"), "MANIFEST-000002\n");
  ASSERT_TRUE(IsOk(this->fs_->RenameReplace(this->Path("CURRENT.tmp"), this->Path("CURRENT"))));
  EXPECT_EQ(this->ReadAll(this->Path("CURRENT")).value(), "MANIFEST-000002\n");
  EXPECT_FALSE(this->fs_->FileExists(this->Path("CURRENT.tmp")).value());
}

// --- Remove ------------------------------------------------------------------

TYPED_TEST(FileSystemConformanceTest, RemoveFile) {
  const auto path = this->Path("doomed");
  this->WriteFile(path, "x");
  ASSERT_TRUE(IsOk(this->fs_->RemoveFile(path)));
  EXPECT_FALSE(this->fs_->FileExists(path).value());
  EXPECT_TRUE(HasCode(this->ReadAll(path).status(), Status::Code::kNotFound));
}

TYPED_TEST(FileSystemConformanceTest, RemoveWhileOpenKeepsHandleAndFreesName) {
  // Compaction deletes table files that the table cache may still have open.
  const auto path = this->Path("000007.sst");
  this->WriteFile(path, "old table");
  Result<std::unique_ptr<RandomAccessFile>> reader = this->fs_->NewRandomAccessFile(path);
  ASSERT_TRUE(IsOk(reader.status()));

  ASSERT_TRUE(IsOk(this->fs_->RemoveFile(path)));
  EXPECT_FALSE(this->fs_->FileExists(path).value()) << "the name must disappear at once";
  EXPECT_EQ(this->ReadAt(**reader, 0, 100), "old table") << "the open handle must keep working";

  this->WriteFile(path, "new table");  // and the name can be reused immediately
  EXPECT_EQ(this->ReadAll(path).value(), "new table");
  EXPECT_EQ(this->ReadAt(**reader, 0, 100), "old table");
}

// --- Directories ---------------------------------------------------------------

TYPED_TEST(FileSystemConformanceTest, GetChildrenListsEntries) {
  this->WriteFile(this->Path("000005.log"), "");
  this->WriteFile(this->Path("CURRENT"), "");
  ASSERT_TRUE(IsOk(this->fs_->CreateDir(this->Path("subdir"))));
  Result<std::vector<std::string>> children = this->fs_->GetChildren(this->root_);
  ASSERT_TRUE(IsOk(children.status()));
  std::vector<std::string> names = *children;
  std::ranges::sort(names);
  EXPECT_EQ(names, (std::vector<std::string>{"000005.log", "CURRENT", "subdir"}));
  EXPECT_TRUE(
      HasCode(this->fs_->GetChildren(this->Path("missing")).status(), Status::Code::kNotFound));
}

TYPED_TEST(FileSystemConformanceTest, CreateDirIsIdempotentAndNeedsParent) {
  const auto dir = this->Path("sub");
  ASSERT_TRUE(IsOk(this->fs_->CreateDir(dir)));
  EXPECT_TRUE(IsOk(this->fs_->CreateDir(dir)));
  EXPECT_TRUE(this->fs_->FileExists(dir).value());
  EXPECT_TRUE(
      HasCode(this->fs_->CreateDir(this->Path("missing") / "child"), Status::Code::kNotFound));
  this->WriteFile(this->Path("plain-file"), "");
  EXPECT_FALSE(this->fs_->CreateDir(this->Path("plain-file")).ok());
}

// --- Locks -------------------------------------------------------------------

TYPED_TEST(FileSystemConformanceTest, LockIsExclusive) {
  const auto path = this->Path("LOCK");
  Result<std::unique_ptr<FileLock>> first = this->fs_->LockFile(path);
  ASSERT_TRUE(IsOk(first.status()));
  EXPECT_TRUE(this->fs_->FileExists(path).value()) << "locking creates the file";

  // A second lock fails, even from the same process.
  EXPECT_TRUE(HasCode(this->fs_->LockFile(path).status(), Status::Code::kBusy));

  ASSERT_TRUE(IsOk(this->fs_->UnlockFile(std::move(first).value())));
  Result<std::unique_ptr<FileLock>> second = this->fs_->LockFile(path);
  ASSERT_TRUE(IsOk(second.status()));

  // Destroying the lock object releases the lock too (RAII).
  second = Status::Busy("replaced");  // destroys the held FileLock
  EXPECT_TRUE(IsOk(this->fs_->LockFile(path).status()));
}

TYPED_TEST(FileSystemConformanceTest, LockedFileCannotBeRemoved) {
  const auto path = this->Path("LOCK");
  Result<std::unique_ptr<FileLock>> lock = this->fs_->LockFile(path);
  ASSERT_TRUE(IsOk(lock.status()));
  EXPECT_FALSE(this->fs_->RemoveFile(path).ok());
  ASSERT_TRUE(IsOk(this->fs_->UnlockFile(std::move(lock).value())));
  EXPECT_TRUE(IsOk(this->fs_->RemoveFile(path)));
}

}  // namespace
}  // namespace forgedb
