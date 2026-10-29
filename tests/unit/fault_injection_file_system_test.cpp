// Tests of the fault-injection file system itself (TODO 2.4). It decides what
// survives every simulated crash from 2.5 on, so it is checked first: that it
// never loses synced data, that every outcome it claims to produce actually
// occurs, and that its faults hit exactly where they are aimed.
#include "support/fault_injection_file_system.h"

#include <cstddef>
#include <cstdint>
#include <filesystem>

#include <gtest/gtest.h>

#include <memory>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include "forgedb/db.h"
#include "forgedb/options.h"

#include "db/db_impl.h"
#include "env/mem_file_system.h"

namespace forgedb {
namespace {

using CrashKind = FaultInjectionFileSystem::CrashKind;
using Op = FaultInjectionFileSystem::Op;
using Options = FaultInjectionFileSystem::Options;
using Outcome = FaultInjectionFileSystem::CrashedFile::Outcome;
using UnsyncedTail = FaultInjectionFileSystem::UnsyncedTail;

const std::filesystem::path& Dir() {
  static const auto* const kDir = new std::filesystem::path("d");  // never destroyed
  return *kDir;
}
std::filesystem::path File(std::string_view name) { return Dir() / name; }

// A fault-injection file system over a fresh MemFileSystem, with Dir() made.
struct TestFs {
  explicit TestFs(const Options& options = {})
      : base(std::make_shared<MemFileSystem>()),
        fs(std::make_unique<FaultInjectionFileSystem>(base, options)) {
    EXPECT_TRUE(fs->CreateDir(Dir()).ok());
  }
  std::shared_ptr<MemFileSystem> base;
  std::unique_ptr<FaultInjectionFileSystem> fs;
};

Options WithSeed(std::uint64_t seed, Options options = {}) {
  options.seed = seed;
  return options;
}

// The file's contents in `fs`, or nullopt if it doesn't exist.
std::optional<std::string> Contents(FileSystem& fs, const std::filesystem::path& path) {
  const Result<std::unique_ptr<SequentialFile>> file = fs.NewSequentialFile(path);
  if (!file.ok()) {
    EXPECT_EQ(file.status().code(), Status::Code::kNotFound) << file.status().ToString();
    return std::nullopt;
  }
  std::string contents;
  std::string buffer(4096, '\0');
  while (true) {
    const Result<std::size_t> count = (*file)->Read(buffer);
    if (!count.ok() || *count == 0) {
      EXPECT_TRUE(count.ok()) << count.status().ToString();
      return contents;
    }
    contents.append(buffer.data(), *count);
  }
}

std::unique_ptr<WritableFile> Create(FileSystem& fs, const std::filesystem::path& path) {
  Result<std::unique_ptr<WritableFile>> file = fs.NewWritableFile(path);
  EXPECT_TRUE(file.ok()) << file.status().ToString();
  return file.ok() ? std::move(file).value() : nullptr;
}

// Writes a whole file and syncs it.
void WriteSynced(FileSystem& fs, const std::filesystem::path& path, std::string_view contents) {
  const std::unique_ptr<WritableFile> file = Create(fs, path);
  ASSERT_NE(file, nullptr);
  ASSERT_TRUE(file->Append(contents).ok());
  ASSERT_TRUE(file->Sync().ok());
  ASSERT_TRUE(file->Close().ok());
}

std::string Bytes(std::size_t size, char seed) {
  std::string bytes(size, '\0');
  for (std::size_t i = 0; i < size; ++i) {
    bytes[i] = static_cast<char>(seed + static_cast<char>(i % 61) + 1);  // never zero
  }
  return bytes;
}

bool IsIOError(const Status& status) { return status.code() == Status::Code::kIOError; }

// --- Power loss: data ----------------------------------------------------------------

// The one promise everything rests on: synced bytes survive exactly, whatever
// the options and the seed.
TEST(FaultInjectionFileSystemTest, SyncedBytesAlwaysSurvivePowerLoss) {
  const std::string synced = Bytes(10000, 'a');  // not a whole number of 4 KiB pages
  for (const std::size_t unsynced_size : {std::size_t{9000}, std::size_t{7}}) {
    const std::string unsynced = Bytes(unsynced_size, 'A');
    for (const UnsyncedTail mode :
         {UnsyncedTail::kDrop, UnsyncedTail::kKeepPrefix, UnsyncedTail::kKeepPrefixTorn}) {
      for (const bool zero_pages : {false, true}) {
        for (std::uint64_t seed = 1; seed <= 50; ++seed) {
          SCOPED_TRACE(testing::Message()
                       << "unsynced " << unsynced_size << " mode " << static_cast<int>(mode)
                       << " zero_pages " << zero_pages << " seed " << seed);
          Options options = WithSeed(seed);
          options.unsynced_tail = mode;
          options.zero_pages_in_unsynced_tail = zero_pages;
          const TestFs setup(options);
          const std::unique_ptr<WritableFile> file = Create(*setup.fs, File("f"));
          ASSERT_TRUE(file->Append(synced).ok());
          ASSERT_TRUE(file->Sync().ok());
          ASSERT_TRUE(file->Append(unsynced).ok());
          ASSERT_TRUE(file->Flush().ok());
          ASSERT_TRUE(setup.fs->SimulateCrash(CrashKind::kPowerLoss).ok());

          const std::optional<std::string> contents = Contents(*setup.base, File("f"));
          ASSERT_TRUE(contents.has_value()) << "a synced file keeps its name";
          const std::string after = contents.value_or("");
          ASSERT_GE(after.size(), synced.size());
          EXPECT_EQ(after.substr(0, synced.size()), synced);
          EXPECT_LE(after.size(), synced.size() + unsynced.size());
          if (mode == UnsyncedTail::kDrop) {
            EXPECT_EQ(after, synced);
          }
        }
      }
    }
  }
}

TEST(FaultInjectionFileSystemTest, KeepPrefixKeepsSomeNoneOrAllOfTheTail) {
  const std::string synced = "synced|";
  const std::string unsynced = Bytes(300, 'k');
  std::set<std::string> kinds;
  for (std::uint64_t seed = 1; seed <= 400; ++seed) {
    Options options = WithSeed(seed);
    options.unsynced_tail = UnsyncedTail::kKeepPrefix;
    const TestFs setup(options);
    const std::unique_ptr<WritableFile> file = Create(*setup.fs, File("f"));
    ASSERT_TRUE(file->Append(synced).ok());
    ASSERT_TRUE(file->Sync().ok());
    ASSERT_TRUE(file->Append(unsynced).ok());
    ASSERT_TRUE(file->Flush().ok());
    ASSERT_TRUE(setup.fs->SimulateCrash(CrashKind::kPowerLoss).ok());
    const std::string after = Contents(*setup.base, File("f")).value_or("");
    const std::string all = synced + unsynced;
    ASSERT_EQ(after, all.substr(0, after.size())) << "an exact prefix, seed " << seed;
    if (after.size() == synced.size()) {
      kinds.insert("none");
    } else if (after.size() == all.size()) {
      kinds.insert("all");
    } else {
      kinds.insert("some");
    }
  }
  EXPECT_EQ(kinds, (std::set<std::string>{"none", "some", "all"}));
}

TEST(FaultInjectionFileSystemTest, TornTailDamagesOnlyTheEndOfTheUnsyncedBytes) {
  const std::string synced = Bytes(1000, 's');
  const std::string unsynced = Bytes(5000, 'u');
  const std::string all = synced + unsynced;
  std::size_t torn = 0;
  for (std::uint64_t seed = 1; seed <= 200; ++seed) {
    SCOPED_TRACE(seed);
    Options options = WithSeed(seed);
    options.unsynced_tail = UnsyncedTail::kKeepPrefixTorn;
    const TestFs setup(options);
    const std::unique_ptr<WritableFile> file = Create(*setup.fs, File("f"));
    ASSERT_TRUE(file->Append(synced).ok());
    ASSERT_TRUE(file->Sync().ok());
    ASSERT_TRUE(file->Append(unsynced).ok());
    ASSERT_TRUE(file->Flush().ok());
    ASSERT_TRUE(setup.fs->SimulateCrash(CrashKind::kPowerLoss).ok());
    const std::string after = Contents(*setup.base, File("f")).value_or("");
    ASSERT_GE(after.size(), synced.size());
    // Every byte that differs from what was written is unsynced and lies in
    // the last 512 bytes.
    std::size_t first_difference = after.size();
    for (std::size_t i = 0; i < after.size(); ++i) {
      if (after[i] != all[i]) {
        first_difference = i;
        break;
      }
    }
    ASSERT_GE(first_difference, synced.size());
    EXPECT_LE(after.size() - first_difference, 512U);
    torn += first_difference < after.size() ? 1U : 0U;
  }
  EXPECT_GT(torn, 100U) << "most crashes that keep a tail tear it";
}

TEST(FaultInjectionFileSystemTest, ZeroPagesLeaveHolesOnlyInTheUnsyncedBytes) {
  const std::string synced = Bytes(5000, 's');  // ends inside the second page
  const std::string unsynced = Bytes(20000, 'u');
  const std::string all = synced + unsynced;
  bool hole_before_data = false;
  for (std::uint64_t seed = 1; seed <= 100; ++seed) {
    SCOPED_TRACE(seed);
    Options options = WithSeed(seed);
    options.unsynced_tail = UnsyncedTail::kKeepPrefix;
    options.zero_pages_in_unsynced_tail = true;
    const TestFs setup(options);
    const std::unique_ptr<WritableFile> file = Create(*setup.fs, File("f"));
    ASSERT_TRUE(file->Append(synced).ok());
    ASSERT_TRUE(file->Sync().ok());
    ASSERT_TRUE(file->Append(unsynced).ok());
    ASSERT_TRUE(file->Flush().ok());
    ASSERT_TRUE(setup.fs->SimulateCrash(CrashKind::kPowerLoss).ok());
    const std::string after = Contents(*setup.base, File("f")).value_or("");
    ASSERT_EQ(after.substr(0, synced.size()), synced);
    bool seen_zero = false;
    for (std::size_t i = synced.size(); i < after.size(); ++i) {
      ASSERT_TRUE(after[i] == all[i] || after[i] == '\0') << "byte " << i;
      seen_zero = seen_zero || after[i] == '\0';
      hole_before_data = hole_before_data || (seen_zero && after[i] != '\0');
    }
  }
  EXPECT_TRUE(hole_before_data) << "a zeroed page followed by surviving data occurs";
}

TEST(FaultInjectionFileSystemTest, BytesNeverFlushedNeverSurvive) {
  for (std::uint64_t seed = 1; seed <= 30; ++seed) {
    for (const CrashKind kind : {CrashKind::kPowerLoss, CrashKind::kProcessCrash}) {
      const TestFs setup(WithSeed(seed));
      const std::unique_ptr<WritableFile> file = Create(*setup.fs, File("f"));
      ASSERT_TRUE(file->Append("synced").ok());
      ASSERT_TRUE(file->Sync().ok());
      ASSERT_TRUE(file->Append("buffered").ok());  // no Flush
      ASSERT_TRUE(setup.fs->SimulateCrash(kind).ok());
      EXPECT_EQ(Contents(*setup.base, File("f")), "synced");
    }
  }
}

TEST(FaultInjectionFileSystemTest, ProcessCrashKeepsEveryFlushedByte) {
  const TestFs setup;
  WriteSynced(*setup.fs, File("old"), "old");
  const std::unique_ptr<WritableFile> file = Create(*setup.fs, File("f"));
  ASSERT_TRUE(file->Append("flushed").ok());
  ASSERT_TRUE(file->Flush().ok());
  ASSERT_TRUE(file->Append("+buffered").ok());
  {
    const std::unique_ptr<WritableFile> closed = Create(*setup.fs, File("closed"));
    ASSERT_TRUE(closed->Append("closing flushes").ok());
    ASSERT_TRUE(closed->Close().ok());
  }
  {
    const std::unique_ptr<WritableFile> dropped = Create(*setup.fs, File("dropped"));
    ASSERT_TRUE(dropped->Append("destroying closes").ok());
  }
  ASSERT_TRUE(setup.fs->RenameReplace(File("old"), File("renamed")).ok());
  ASSERT_TRUE(setup.fs->SimulateCrash(CrashKind::kProcessCrash).ok());
  EXPECT_EQ(Contents(*setup.base, File("f")), "flushed");
  EXPECT_EQ(Contents(*setup.base, File("closed")), "closing flushes");
  EXPECT_EQ(Contents(*setup.base, File("dropped")), "destroying closes");
  EXPECT_EQ(Contents(*setup.base, File("renamed")), "old") << "the OS kept the rename";
  EXPECT_EQ(Contents(*setup.base, File("old")), std::nullopt);
}

// SyncFile: the same guarantee as WritableFile::Sync, for a closed file.
TEST(FaultInjectionFileSystemTest, SyncFileMakesAClosedFileDurable) {
  for (std::uint64_t seed = 1; seed <= 30; ++seed) {
    const TestFs setup(WithSeed(seed));
    {
      const std::unique_ptr<WritableFile> file = Create(*setup.fs, File("f"));
      ASSERT_TRUE(file->Append("written, closed, never synced").ok());
      ASSERT_TRUE(file->Close().ok());
    }
    ASSERT_TRUE(setup.fs->SyncFile(File("f")).ok());
    ASSERT_TRUE(setup.fs->SimulateCrash(CrashKind::kPowerLoss).ok());
    EXPECT_EQ(Contents(*setup.base, File("f")), "written, closed, never synced");
  }
  const TestFs setup;
  EXPECT_EQ(setup.fs->SyncFile(File("missing")).code(), Status::Code::kNotFound);
}

// A process crash leaves unsynced data in the OS's cache: still unsynced.
TEST(FaultInjectionFileSystemTest, AfterAProcessCrashUnsyncedDataCanStillBeLost) {
  Options options;
  options.unsynced_tail = UnsyncedTail::kDrop;
  std::set<std::string> new_file_outcomes;
  for (std::uint64_t seed = 1; seed <= 40; ++seed) {
    options.seed = seed;
    const TestFs setup(options);
    {
      const std::unique_ptr<WritableFile> file = Create(*setup.fs, File("f"));
      ASSERT_TRUE(file->Append("synced").ok());
      ASSERT_TRUE(file->Sync().ok());
      ASSERT_TRUE(file->Append("+flushed").ok());
      ASSERT_TRUE(file->Close().ok());
    }
    {
      const std::unique_ptr<WritableFile> file = Create(*setup.fs, File("new"));
      ASSERT_TRUE(file->Append("never synced").ok());
      ASSERT_TRUE(file->Close().ok());
    }
    ASSERT_TRUE(setup.fs->SimulateCrash(CrashKind::kProcessCrash).ok());
    ASSERT_EQ(Contents(*setup.base, File("f")), "synced+flushed");
    ASSERT_EQ(Contents(*setup.base, File("new")), "never synced");

    ASSERT_TRUE(setup.fs->SimulateCrash(CrashKind::kPowerLoss).ok());
    EXPECT_EQ(Contents(*setup.base, File("f")), "synced");
    new_file_outcomes.insert(Contents(*setup.base, File("new")).value_or("<missing>"));
  }
  EXPECT_EQ(new_file_outcomes, (std::set<std::string>{"<missing>", ""}))
      << "the never-synced file may vanish (its bytes are dropped by kDrop)";
}

// --- Power loss: names ---------------------------------------------------------------

TEST(FaultInjectionFileSystemTest, AFileNeverSyncedMayVanish) {
  std::set<std::string> outcomes;
  for (std::uint64_t seed = 1; seed <= 100; ++seed) {
    const TestFs setup(WithSeed(seed));
    const std::unique_ptr<WritableFile> file = Create(*setup.fs, File("f"));
    ASSERT_TRUE(file->Append("never synced").ok());
    ASSERT_TRUE(file->Close().ok());
    ASSERT_TRUE(setup.fs->SimulateCrash(CrashKind::kPowerLoss).ok());
    const std::optional<std::string> after = Contents(*setup.base, File("f"));
    outcomes.insert(after ? "exists" : "vanished");
  }
  EXPECT_EQ(outcomes, (std::set<std::string>{"exists", "vanished"}));
}

TEST(FaultInjectionFileSystemTest, ARecreatedFileComesBackOldOrNew) {
  std::set<std::string> outcomes;
  for (std::uint64_t seed = 1; seed <= 100; ++seed) {
    SCOPED_TRACE(seed);
    Options options = WithSeed(seed);
    options.unsynced_tail = UnsyncedTail::kKeepPrefix;
    const TestFs setup(options);
    WriteSynced(*setup.fs, File("f"), "old contents");
    const std::unique_ptr<WritableFile> file = Create(*setup.fs, File("f"));  // truncates
    ASSERT_TRUE(file->Append("new").ok());
    ASSERT_TRUE(file->Close().ok());
    ASSERT_TRUE(setup.fs->SimulateCrash(CrashKind::kPowerLoss).ok());
    const std::string after = Contents(*setup.base, File("f")).value_or("<missing>");
    if (after == "old contents") {
      outcomes.insert("old");
    } else {
      ASSERT_EQ(after, std::string("new").substr(0, after.size()));
      outcomes.insert("new");
    }
  }
  EXPECT_EQ(outcomes, (std::set<std::string>{"old", "new"}));
}

TEST(FaultInjectionFileSystemTest, SyncingTheNewFileMakesItTheDurableOne) {
  for (std::uint64_t seed = 1; seed <= 30; ++seed) {
    const TestFs setup(WithSeed(seed));
    WriteSynced(*setup.fs, File("f"), "old contents");
    WriteSynced(*setup.fs, File("f"), "new");
    ASSERT_TRUE(setup.fs->SimulateCrash(CrashKind::kPowerLoss).ok());
    EXPECT_EQ(Contents(*setup.base, File("f")), "new");
  }
}

TEST(FaultInjectionFileSystemTest, RemovalsAreNotDurableUnlessConfigured) {
  std::set<std::string> outcomes;
  for (std::uint64_t seed = 1; seed <= 60; ++seed) {
    for (const bool durable : {false, true}) {
      Options options = WithSeed(seed);
      options.durable_removals = durable;
      const TestFs setup(options);
      WriteSynced(*setup.fs, File("f"), "contents");
      ASSERT_TRUE(setup.fs->RemoveFile(File("f")).ok());
      ASSERT_TRUE(setup.fs->SimulateCrash(CrashKind::kPowerLoss).ok());
      const std::optional<std::string> after = Contents(*setup.base, File("f"));
      if (durable) {
        EXPECT_EQ(after, std::nullopt);
      } else {
        EXPECT_TRUE(!after || *after == "contents") << "it comes back whole, or not at all";
        outcomes.insert(after ? "came back" : "gone");
      }
    }
  }
  EXPECT_EQ(outcomes, (std::set<std::string>{"came back", "gone"}));
}

// The CURRENT switch of Phase 3.6: write a temporary file, sync it, rename it
// over the old one.
TEST(FaultInjectionFileSystemTest, RenamesAreNotDurableUnlessConfigured) {
  std::set<std::string> outcomes;
  for (std::uint64_t seed = 1; seed <= 100; ++seed) {
    for (const bool durable : {false, true}) {
      Options options = WithSeed(seed);
      options.durable_renames = durable;
      const TestFs setup(options);
      WriteSynced(*setup.fs, File("CURRENT"), "old");
      WriteSynced(*setup.fs, File("CURRENT.tmp"), "new");
      ASSERT_TRUE(setup.fs->RenameReplace(File("CURRENT.tmp"), File("CURRENT")).ok());
      ASSERT_TRUE(setup.fs->SimulateCrash(CrashKind::kPowerLoss).ok());
      const std::optional<std::string> current = Contents(*setup.base, File("CURRENT"));
      const std::optional<std::string> tmp = Contents(*setup.base, File("CURRENT.tmp"));
      if (durable) {
        EXPECT_EQ(current, "new");
        EXPECT_EQ(tmp, std::nullopt);
      } else {
        ASSERT_TRUE(current == "old" || current == "new");
        ASSERT_TRUE(tmp == std::nullopt || tmp == "new");
        outcomes.insert("CURRENT=" + current.value_or("") + (tmp ? " tmp=new" : " no tmp"));
      }
    }
  }
  // Each name is decided on its own: all four combinations occur.
  EXPECT_EQ(outcomes.size(), 4U);
}

TEST(FaultInjectionFileSystemTest, SyncAfterARenameMakesTheNewNameDurable) {
  for (std::uint64_t seed = 1; seed <= 30; ++seed) {
    const TestFs setup(WithSeed(seed));
    const std::unique_ptr<WritableFile> file = Create(*setup.fs, File("a"));
    ASSERT_TRUE(file->Append("data").ok());
    ASSERT_TRUE(setup.fs->RenameReplace(File("a"), File("b")).ok());
    ASSERT_TRUE(file->Sync().ok());
    ASSERT_TRUE(setup.fs->SimulateCrash(CrashKind::kPowerLoss).ok());
    EXPECT_EQ(Contents(*setup.base, File("b")), "data");
  }
}

TEST(FaultInjectionFileSystemTest, FilesFromBeforeTrackingAreDurable) {
  const TestFs setup;
  ASSERT_TRUE(setup.base->CreateDir("pre").ok());
  WriteSynced(*setup.base, "pre/f", "already there");  // straight into the wrapped FS
  ASSERT_TRUE(setup.fs->SimulateCrash(CrashKind::kPowerLoss).ok());
  EXPECT_EQ(Contents(*setup.base, "pre/f"), "already there");

  // Truncated through the wrapper but never synced: the old contents may return.
  std::set<std::string> outcomes;
  for (std::uint64_t seed = 1; seed <= 60; ++seed) {
    const TestFs again(WithSeed(seed));
    WriteSynced(*again.base, File("f"), "already there");
    {
      const std::unique_ptr<WritableFile> file = Create(*again.fs, File("f"));
      ASSERT_TRUE(file->Close().ok());
    }
    ASSERT_TRUE(again.fs->SimulateCrash(CrashKind::kPowerLoss).ok());
    outcomes.insert(Contents(*again.base, File("f")).value_or("<missing>"));
  }
  EXPECT_EQ(outcomes, (std::set<std::string>{"already there", ""}));
}

TEST(FaultInjectionFileSystemTest, AfterACrashEverythingIsDurable) {
  for (std::uint64_t seed = 1; seed <= 30; ++seed) {
    const TestFs setup(WithSeed(seed));
    const std::unique_ptr<WritableFile> file = Create(*setup.fs, File("f"));
    ASSERT_TRUE(file->Append(Bytes(3000, 'x')).ok());
    ASSERT_TRUE(file->Flush().ok());
    ASSERT_TRUE(setup.fs->SimulateCrash(CrashKind::kPowerLoss).ok());
    const std::optional<std::string> first = Contents(*setup.base, File("f"));
    ASSERT_TRUE(setup.fs->SimulateCrash(CrashKind::kPowerLoss).ok());
    EXPECT_EQ(Contents(*setup.base, File("f")), first) << "seed " << seed;
    EXPECT_TRUE(setup.fs->LastCrash().empty()) << "nothing left to decide";
  }
}

TEST(FaultInjectionFileSystemTest, LastCrashReportsEachDecision) {
  Options options = WithSeed(1);
  options.unsynced_tail = UnsyncedTail::kDrop;
  const TestFs setup(options);
  WriteSynced(*setup.fs, File("kept"), "kept");
  {
    const std::unique_ptr<WritableFile> file = Create(*setup.fs, File("cut"));
    ASSERT_TRUE(file->Append("synced").ok());
    ASSERT_TRUE(file->Sync().ok());
    ASSERT_TRUE(file->Append("unsynced").ok());
    ASSERT_TRUE(file->Close().ok());
  }
  ASSERT_TRUE(setup.fs->SimulateCrash(CrashKind::kPowerLoss).ok());
  const std::vector<FaultInjectionFileSystem::CrashedFile> crash = setup.fs->LastCrash();
  ASSERT_EQ(crash.size(), 2U);  // in path order
  EXPECT_EQ(crash[0].path, File("cut").generic_string());
  EXPECT_EQ(crash[0].outcome, Outcome::kRewritten);
  EXPECT_EQ(crash[0].size, 6U);
  EXPECT_EQ(crash[1].path, File("kept").generic_string());
  EXPECT_EQ(crash[1].outcome, Outcome::kUnchanged);
}

TEST(FaultInjectionFileSystemTest, ASeedDeterminesTheCrash) {
  const auto run = [](std::uint64_t seed) {
    const TestFs setup(WithSeed(seed));
    for (int i = 0; i < 20; ++i) {
      const std::unique_ptr<WritableFile> file = Create(*setup.fs, File(std::to_string(i)));
      EXPECT_TRUE(file->Append(Bytes(1000, static_cast<char>('a' + i))).ok());
      if (i % 3 == 0) {
        EXPECT_TRUE(file->Sync().ok());
      }
      EXPECT_TRUE(file->Append(Bytes(700, 'z')).ok());
      EXPECT_TRUE(file->Close().ok());
    }
    EXPECT_TRUE(setup.fs->SimulateCrash(CrashKind::kPowerLoss).ok());
    std::vector<std::optional<std::string>> files;
    files.reserve(20);
    for (int i = 0; i < 20; ++i) {
      files.push_back(Contents(*setup.base, File(std::to_string(i))));
    }
    return files;
  };
  EXPECT_EQ(run(7), run(7));
  EXPECT_NE(run(7), run(8));
}

// --- After a crash -----------------------------------------------------------------------

TEST(FaultInjectionFileSystemTest, HandlesFromBeforeACrashAreDead) {
  const TestFs setup;
  WriteSynced(*setup.fs, File("r"), "readable");
  const std::unique_ptr<WritableFile> writer = Create(*setup.fs, File("w"));
  ASSERT_TRUE(writer->Append("synced").ok());
  ASSERT_TRUE(writer->Sync().ok());
  const std::unique_ptr<SequentialFile> reader =
      std::move(setup.fs->NewSequentialFile(File("r"))).value();
  const std::unique_ptr<RandomAccessFile> random =
      std::move(setup.fs->NewRandomAccessFile(File("r"))).value();

  ASSERT_TRUE(setup.fs->SimulateCrash(CrashKind::kPowerLoss).ok());
  EXPECT_TRUE(IsIOError(writer->Append("more")));
  EXPECT_TRUE(IsIOError(writer->Sync()));
  EXPECT_TRUE(IsIOError(writer->Close()));
  std::string buffer(4, '\0');
  EXPECT_TRUE(IsIOError(reader->Read(buffer).status()));
  EXPECT_TRUE(IsIOError(reader->Skip(1)));
  EXPECT_TRUE(IsIOError(random->Read(0, buffer).status()));
  EXPECT_EQ(Contents(*setup.base, File("w")), "synced") << "dead handles change nothing";

  // New handles work.
  const std::unique_ptr<SequentialFile> fresh =
      std::move(setup.fs->NewSequentialFile(File("r"))).value();
  EXPECT_EQ(std::move(fresh->Read(buffer)).value(), 4U);
}

TEST(FaultInjectionFileSystemTest, ACrashReleasesLocks) {
  const TestFs setup;
  std::unique_ptr<FileLock> lock = std::move(setup.fs->LockFile(File("LOCK"))).value();
  EXPECT_EQ(setup.fs->LockFile(File("LOCK")).status().code(), Status::Code::kBusy);
  ASSERT_TRUE(setup.fs->SimulateCrash(CrashKind::kProcessCrash).ok());
  Result<std::unique_ptr<FileLock>> relocked = setup.fs->LockFile(File("LOCK"));
  ASSERT_TRUE(relocked.ok()) << relocked.status().ToString();
  std::unique_ptr<FileLock> second = std::move(relocked).value();
  EXPECT_TRUE(setup.fs->UnlockFile(std::move(lock)).ok()) << "the dead lock: nothing to do";
  EXPECT_EQ(setup.fs->LockFile(File("LOCK")).status().code(), Status::Code::kBusy)
      << "and the new lock is still held";
  EXPECT_TRUE(setup.fs->UnlockFile(std::move(second)).ok());
  EXPECT_TRUE(setup.fs->LockFile(File("LOCK")).ok());
}

TEST(FaultInjectionFileSystemTest, ForeignLocksAreRejected) {
  const TestFs setup;
  MemFileSystem other;
  ASSERT_TRUE(other.CreateDir("x").ok());
  std::unique_ptr<FileLock> foreign = std::move(other.LockFile("x/LOCK")).value();
  EXPECT_EQ(setup.fs->UnlockFile(std::move(foreign)).code(), Status::Code::kInvalidArgument);
}

// --- Injected errors -----------------------------------------------------------------------

TEST(FaultInjectionFileSystemTest, FailNthFailsExactlyThatCallAndErrorsStick) {
  Options options;
  options.partial_append_on_error = false;
  const TestFs setup(options);
  setup.fs->FailNth(Op::kAppend, 3);
  const std::unique_ptr<WritableFile> file = Create(*setup.fs, File("f"));
  EXPECT_TRUE(file->Append("1").ok());
  EXPECT_TRUE(file->Append("2").ok());
  const Status failed = file->Append("3");
  EXPECT_TRUE(IsIOError(failed));
  EXPECT_NE(failed.ToString().find("Append"), std::string::npos) << failed.ToString();
  EXPECT_EQ(file->Flush(), failed) << "sticky";
  EXPECT_EQ(file->Close(), failed);
  EXPECT_EQ(Contents(*setup.base, File("f")), "12");

  const std::unique_ptr<WritableFile> other = Create(*setup.fs, File("g"));
  EXPECT_TRUE(other->Append("fine").ok()) << "only the third Append fails";
}

TEST(FaultInjectionFileSystemTest, AFailedAppendMayWriteAPrefix) {
  std::set<std::size_t> written;
  for (std::uint64_t seed = 1; seed <= 40; ++seed) {
    const TestFs setup(WithSeed(seed));  // partial_append_on_error is on by default
    setup.fs->FailNth(Op::kAppend, 1);
    const std::unique_ptr<WritableFile> file = Create(*setup.fs, File("f"));
    EXPECT_TRUE(IsIOError(file->Append("0123456789")));
    const std::string after = Contents(*setup.base, File("f")).value_or("");
    ASSERT_LT(after.size(), 10U) << "a strict prefix";
    ASSERT_EQ(after, std::string("0123456789").substr(0, after.size()));
    written.insert(after.size());
  }
  EXPECT_GT(written.size(), 3U);
  EXPECT_TRUE(written.contains(0U));
}

TEST(FaultInjectionFileSystemTest, EveryOperationCanBeFailed) {
  const auto expect_fails = [](Op op, const auto& call) {
    SCOPED_TRACE(OpName(op));
    const TestFs setup;
    WriteSynced(*setup.fs, File("existing"), "x");
    std::unique_ptr<WritableFile> file = Create(*setup.fs, File("open"));
    setup.fs->FailNth(op, 1);
    EXPECT_TRUE(IsIOError(call(*setup.fs, file)));
    // A failed file-system call changed nothing.
    EXPECT_EQ(Contents(*setup.base, File("existing")), "x");
    EXPECT_EQ(Contents(*setup.base, File("new")), std::nullopt);
  };
  using FS = FaultInjectionFileSystem;
  using FilePtr = std::unique_ptr<WritableFile>;
  expect_fails(Op::kNewWritableFile,
               [](FS& fs, FilePtr&) { return fs.NewWritableFile(File("new")).status(); });
  expect_fails(Op::kAppend, [](FS&, FilePtr& f) { return f->Append("a"); });
  expect_fails(Op::kFlush, [](FS&, FilePtr& f) { return f->Flush(); });
  expect_fails(Op::kSync, [](FS&, FilePtr& f) { return f->Sync(); });
  expect_fails(Op::kClose, [](FS&, FilePtr& f) { return f->Close(); });
  expect_fails(Op::kRenameReplace,
               [](FS& fs, FilePtr&) { return fs.RenameReplace(File("existing"), File("new")); });
  expect_fails(Op::kRemoveFile, [](FS& fs, FilePtr&) { return fs.RemoveFile(File("existing")); });
  expect_fails(Op::kCreateDir, [](FS& fs, FilePtr&) { return fs.CreateDir(File("new")); });
  expect_fails(Op::kSyncFile, [](FS& fs, FilePtr&) { return fs.SyncFile(File("existing")); });
  expect_fails(Op::kLockFile, [](FS& fs, FilePtr&) { return fs.LockFile(File("new")).status(); });
}

TEST(FaultInjectionFileSystemTest, AFailedSyncLeavesTheDataUnsynced) {
  for (std::uint64_t seed = 1; seed <= 30; ++seed) {
    Options options = WithSeed(seed);
    options.unsynced_tail = UnsyncedTail::kDrop;
    const TestFs setup(options);
    WriteSynced(*setup.fs, File("f"), "");  // the name is durable
    const std::unique_ptr<WritableFile> file = Create(*setup.fs, File("g"));
    ASSERT_TRUE(file->Append("first").ok());
    ASSERT_TRUE(file->Sync().ok());
    ASSERT_TRUE(file->Append("second").ok());
    setup.fs->FailNth(Op::kSync, 1);
    EXPECT_TRUE(IsIOError(file->Sync()));
    ASSERT_TRUE(setup.fs->SimulateCrash(CrashKind::kPowerLoss).ok());
    EXPECT_EQ(Contents(*setup.base, File("g")), "first");
  }
}

TEST(FaultInjectionFileSystemTest, FailWithProbability) {
  const TestFs setup;
  setup.fs->FailWithProbability(Op::kCreateDir, 1.0);
  EXPECT_TRUE(IsIOError(setup.fs->CreateDir(File("a"))));
  setup.fs->FailWithProbability(Op::kCreateDir, 0.5);
  int failures = 0;
  for (int i = 0; i < 1000; ++i) {
    failures += setup.fs->CreateDir(File("b")).ok() ? 0 : 1;
  }
  EXPECT_GT(failures, 400);
  EXPECT_LT(failures, 600);
  setup.fs->ClearFaults();
  EXPECT_TRUE(setup.fs->CreateDir(File("c")).ok());
}

TEST(FaultInjectionFileSystemTest, CrashAtOperationKillsTheProcessThere) {
  const TestFs setup;
  WriteSynced(*setup.fs, File("f"), "synced");
  const std::unique_ptr<SequentialFile> reader =
      std::move(setup.fs->NewSequentialFile(File("f"))).value();
  const std::unique_ptr<RandomAccessFile> random =
      std::move(setup.fs->NewRandomAccessFile(File("f"))).value();
  const std::uint64_t before = setup.fs->OperationCount();
  setup.fs->CrashAtOperation(3);
  const std::unique_ptr<WritableFile> file = Create(*setup.fs, File("g"));  // 1
  ASSERT_TRUE(file->Append("a").ok());                                      // 2
  EXPECT_FALSE(setup.fs->ProcessIsDead());
  EXPECT_TRUE(IsIOError(file->Append("b")));  // 3: dies here
  EXPECT_TRUE(setup.fs->ProcessIsDead());
  EXPECT_EQ(setup.fs->OperationCount(), before + 3);

  // Everything fails now, reads included (also through readers opened before
  // the process died), and nothing is counted or changed.
  EXPECT_TRUE(IsIOError(file->Sync()));
  EXPECT_TRUE(IsIOError(setup.fs->RemoveFile(File("f"))));
  EXPECT_TRUE(IsIOError(setup.fs->NewWritableFile(File("h")).status()));
  EXPECT_TRUE(IsIOError(setup.fs->NewSequentialFile(File("f")).status()));
  EXPECT_TRUE(IsIOError(setup.fs->GetChildren(Dir()).status()));
  EXPECT_TRUE(IsIOError(setup.fs->FileExists(File("f")).status()));
  EXPECT_TRUE(IsIOError(setup.fs->GetFileSize(File("f")).status()));
  std::string buffer(2, '\0');
  EXPECT_TRUE(IsIOError(reader->Read(buffer).status()));
  EXPECT_TRUE(IsIOError(reader->Skip(1)));
  EXPECT_TRUE(IsIOError(random->Read(0, buffer).status()));
  EXPECT_EQ(setup.fs->OperationCount(), before + 3);
  EXPECT_EQ(Contents(*setup.base, File("f")), "synced");
  EXPECT_EQ(Contents(*setup.base, File("g")), "a") << "live state until the crash is simulated";

  ASSERT_TRUE(setup.fs->SimulateCrash(CrashKind::kProcessCrash).ok());
  EXPECT_FALSE(setup.fs->ProcessIsDead());
  EXPECT_EQ(Contents(*setup.base, File("g")), "") << "'a' was never flushed";
  EXPECT_TRUE(setup.fs->CreateDir(File("alive")).ok());
}

TEST(FaultInjectionFileSystemTest, KillProcessKillsItNow) {
  const TestFs setup;
  const std::unique_ptr<WritableFile> file = Create(*setup.fs, File("f"));
  ASSERT_TRUE(file->Append("flushed").ok());
  ASSERT_TRUE(file->Flush().ok());
  const std::uint64_t before = setup.fs->OperationCount();
  setup.fs->CrashAtOperation(100);  // superseded
  setup.fs->KillProcess();
  EXPECT_TRUE(setup.fs->ProcessIsDead());
  // Like a thread still running in the dead process: nothing it does counts.
  EXPECT_TRUE(IsIOError(file->Append(" more")));
  EXPECT_TRUE(IsIOError(setup.fs->NewWritableFile(File("zombie")).status()));
  EXPECT_TRUE(IsIOError(setup.fs->RemoveFile(File("f"))));
  EXPECT_EQ(setup.fs->OperationCount(), before);

  ASSERT_TRUE(setup.fs->SimulateCrash(CrashKind::kProcessCrash).ok());
  EXPECT_FALSE(setup.fs->ProcessIsDead());
  EXPECT_EQ(Contents(*setup.base, File("f")), "flushed");
  EXPECT_EQ(Contents(*setup.base, File("zombie")), std::nullopt);
  for (int i = 0; i < 200; ++i) {
    ASSERT_TRUE(setup.fs->CreateDir(File("alive")).ok()) << "the old crash point is gone";
  }
}

// --- With the database ---------------------------------------------------------------------

std::string Get(DB& db, std::string_view key) {
  Result<std::string> value = db.Get(ReadOptions{}, key);
  return value.ok() ? std::move(value).value() : value.status().ToString();
}

TEST(FaultInjectionFileSystemTest, SyncedDatabaseWritesSurvivePowerLoss) {
  for (std::uint64_t seed = 1; seed <= 50; ++seed) {
    SCOPED_TRACE(seed);
    const auto base = std::make_shared<MemFileSystem>();
    const auto fs = std::make_shared<FaultInjectionFileSystem>(base, WithSeed(seed));
    {
      const std::unique_ptr<DB> db = std::move(OpenDB({}, "db", fs)).value();
      ASSERT_TRUE(db->Put({.sync = true}, "a", "1").ok());
      ASSERT_TRUE(db->Put({}, "b", "2").ok());  // flushed, not synced
      ASSERT_TRUE(fs->SimulateCrash(CrashKind::kPowerLoss).ok());
    }  // the crashed DB is destroyed after the crash: it changes nothing
    const Result<std::unique_ptr<DB>> reopened = OpenDB({}, "db", fs);
    ASSERT_TRUE(reopened.ok()) << "a torn tail never fails recovery: "
                               << reopened.status().ToString();
    EXPECT_EQ(Get(**reopened, "a"), "1");
    const std::string b = Get(**reopened, "b");
    EXPECT_TRUE(b == "2" || b.starts_with("NotFound")) << b;
  }
}

TEST(FaultInjectionFileSystemTest, UnsyncedDatabaseWritesSurviveAProcessCrash) {
  const auto base = std::make_shared<MemFileSystem>();
  const auto fs = std::make_shared<FaultInjectionFileSystem>(base);
  {
    const std::unique_ptr<DB> db = std::move(OpenDB({}, "db", fs)).value();
    ASSERT_TRUE(db->Put({}, "a", "1").ok());
    ASSERT_TRUE(db->Put({}, "b", "2").ok());
    ASSERT_TRUE(fs->SimulateCrash(CrashKind::kProcessCrash).ok());
  }
  const std::unique_ptr<DB> db = std::move(OpenDB({}, "db", fs)).value();
  EXPECT_EQ(Get(*db, "a"), "1");
  EXPECT_EQ(Get(*db, "b"), "2");
}

TEST(FaultInjectionFileSystemTest, ADatabaseThatDiesMidWriteRecovers) {
  const auto base = std::make_shared<MemFileSystem>();
  const auto fs = std::make_shared<FaultInjectionFileSystem>(base);
  {
    const std::unique_ptr<DB> db = std::move(OpenDB({}, "db", fs)).value();
    const std::uint64_t before = fs->OperationCount();
    ASSERT_TRUE(db->Put({.sync = true}, "a", "1").ok());
    const std::uint64_t per_write = fs->OperationCount() - before;  // appends, flush, sync
    fs->CrashAtOperation(per_write);  // the next write's log append succeeds, its sync dies
    EXPECT_TRUE(IsIOError(db->Put({.sync = true}, "b", "2")));
    EXPECT_TRUE(IsIOError(db->Put({}, "c", "3"))) << "the DB refuses writes after a failure";
  }
  ASSERT_TRUE(fs->SimulateCrash(CrashKind::kPowerLoss).ok());
  const std::unique_ptr<DB> db = std::move(OpenDB({}, "db", fs)).value();
  EXPECT_EQ(Get(*db, "a"), "1");
}

// Many writers at once (group commit) through the wrapper; under TSan this
// checks its locking.
TEST(FaultInjectionFileSystemTest, ConcurrentDatabaseWritersThenAProcessCrash) {
  const auto base = std::make_shared<MemFileSystem>();
  const auto fs = std::make_shared<FaultInjectionFileSystem>(base);
  constexpr int kThreads = 4;
  constexpr int kWritesPerThread = 200;
  {
    const std::unique_ptr<DB> db = std::move(OpenDB({}, "db", fs)).value();
    std::vector<std::thread> threads;
    threads.reserve(kThreads);
    for (int t = 0; t < kThreads; ++t) {
      threads.emplace_back([&db, t] {
        for (int i = 0; i < kWritesPerThread; ++i) {
          const std::string key = std::to_string(t) + "-" + std::to_string(i);
          EXPECT_TRUE(db->Put({.sync = i % 10 == 0}, key, key).ok());
        }
      });
    }
    for (std::thread& thread : threads) {
      thread.join();
    }
    ASSERT_TRUE(fs->SimulateCrash(CrashKind::kProcessCrash).ok());
  }
  const std::unique_ptr<DB> db = std::move(OpenDB({}, "db", fs)).value();
  for (int t = 0; t < kThreads; ++t) {
    for (int i = 0; i < kWritesPerThread; ++i) {
      const std::string key = std::to_string(t) + "-" + std::to_string(i);
      ASSERT_EQ(Get(*db, key), key);
    }
  }
}

}  // namespace
}  // namespace forgedb
