// Flushing memtables to tables (TODO 3.5), reads across memtables and tables,
// options, and the files a DB leaves behind (TODO 3.6).
#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <filesystem>

#include <gtest/gtest.h>

#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include "forgedb/db.h"
#include "forgedb/iterator.h"
#include "forgedb/options.h"
#include "forgedb/status.h"

#include "db/db_impl.h"
#include "db/filename.h"
#include "env/file_system.h"
#include "env/mem_file_system.h"
#include "support/fault_injection_file_system.h"

namespace forgedb {
namespace {

std::string Key(int i) {
  const std::string digits = std::to_string(i);
  return "key" + std::string(digits.size() < 6 ? 6 - digits.size() : 0, '0') + digits;
}

std::string Value(int i, int version = 0) {
  return "value-" + std::to_string(i) + "-" + std::to_string(version) + std::string(200, 'x');
}

std::string Get(DB& db, std::string_view key) {
  Result<std::string> value = db.Get(ReadOptions{}, key);
  if (value.ok()) {
    return std::move(value).value();
  }
  return value.status().code() == Status::Code::kNotFound ? "NOT_FOUND"
                                                          : "ERROR: " + value.status().ToString();
}

DBImpl& Impl(DB& db) { return dynamic_cast<DBImpl&>(db); }

Options SmallMemtable() {
  Options options;
  options.memtable_bytes = std::size_t{16} << 10;
  return options;
}

class FlushTest : public testing::Test {
 protected:
  std::unique_ptr<DB> Open(const Options& options = SmallMemtable()) {
    Result<std::unique_ptr<DB>> db = OpenDB(options, db_, fs_);
    EXPECT_TRUE(db.ok()) << db.status().ToString();
    return db.ok() ? std::move(db).value() : nullptr;
  }

  std::vector<std::string> Files(FileType type) {
    std::vector<std::string> names;
    for (const std::string& child : std::move(fs_->GetChildren(db_)).value()) {
      if (const std::optional<ParsedFileName> parsed = ParseFileName(child);
          parsed && parsed->type == type) {
        names.push_back(child);
      }
    }
    std::ranges::sort(names);
    return names;
  }

  void WriteFile(const std::filesystem::path& path, std::string_view contents) {
    const std::unique_ptr<WritableFile> file = std::move(fs_->NewWritableFile(path)).value();
    ASSERT_TRUE(file->Append(contents).ok());
    ASSERT_TRUE(file->Close().ok());
  }

  const std::filesystem::path db_{"db"};
  std::shared_ptr<MemFileSystem> fs_ = std::make_shared<MemFileSystem>();
};

// --- Options -------------------------------------------------------------------

TEST(OptionsTest, DefaultsAreValid) { EXPECT_TRUE(ValidateOptions(Options{}).ok()); }

TEST(OptionsTest, OutOfRangeFieldsAreRejected) {
  const auto rejected = [](void (*change)(Options&)) {
    Options options;
    change(options);
    return OpenDB(options, "db", std::make_shared<MemFileSystem>()).status().code() ==
           Status::Code::kInvalidArgument;
  };
  EXPECT_TRUE(rejected([](Options& o) { o.memtable_bytes = 0; }));
  EXPECT_TRUE(rejected([](Options& o) { o.max_open_tables = 0; }));
  EXPECT_TRUE(rejected([](Options& o) { o.filter_bits_per_key = -1; }));
  EXPECT_TRUE(rejected([](Options& o) { o.filter_bits_per_key = 31; }));
  EXPECT_TRUE(rejected([](Options& o) { o.block_size = 255; }));
  EXPECT_TRUE(rejected([](Options& o) { o.block_size = (std::size_t{1} << 20) + 1; }));
  EXPECT_FALSE(rejected([](Options& o) {
    o.block_cache_bytes = 0;    // no block cache: allowed
    o.filter_bits_per_key = 0;  // no filters: allowed
  }));
}

// --- Flushing --------------------------------------------------------------------

TEST_F(FlushTest, FullMemtablesAreFlushedAndEverythingStaysReadable) {
  const std::unique_ptr<DB> db = Open();
  for (int i = 0; i < 1000; ++i) {
    ASSERT_TRUE(db->Put({}, Key(i), Value(i)).ok());
  }
  Impl(*db).WaitForFlushForTesting();  // the last one runs in the background
  const std::uint64_t flushes = Impl(*db).FlushesForTesting();
  EXPECT_GE(flushes, 10U) << "1000 writes of ~220 bytes into 16 KiB memtables";
  EXPECT_EQ(Impl(*db).CurrentVersion()->files[0].size(), flushes);
  EXPECT_EQ(Files(FileType::kTable).size(), flushes);
  // The current log and the one the last flush retired, kept until the next
  // MANIFEST record (ADR 0007); each flush deleted the one before.
  EXPECT_EQ(Files(FileType::kLog).size(), 2U);
  EXPECT_EQ(Files(FileType::kDescriptor).size(), 1U);

  for (int i = 0; i < 1000; ++i) {
    ASSERT_EQ(Get(*db, Key(i)), Value(i)) << i;
  }
  EXPECT_EQ(Get(*db, "missing"), "NOT_FOUND");
  const std::unique_ptr<Iterator> it = db->NewIterator(ReadOptions{});
  int i = 0;
  for (it->SeekToFirst(); it->Valid(); it->Next(), ++i) {
    ASSERT_EQ(it->key(), Key(i));
    ASSERT_EQ(it->value(), Value(i));
  }
  EXPECT_TRUE(it->status().ok());
  EXPECT_EQ(i, 1000);
}

// The newest version wins wherever it is: memtable over tables, a newer table
// over an older one, and a deletion hides older values in any of them.
TEST_F(FlushTest, NewerVersionsAndDeletionsWinAcrossTables) {
  const std::unique_ptr<DB> db = Open();
  for (int version = 0; version < 4; ++version) {
    for (int i = 0; i < 300; ++i) {
      if (version == 2 && i % 3 == 0) {
        ASSERT_TRUE(db->Delete({}, Key(i)).ok());
      } else {
        ASSERT_TRUE(db->Put({}, Key(i), Value(i, version)).ok());
      }
    }
  }
  for (int i = 0; i < 300; i += 2) {
    ASSERT_TRUE(db->Delete({}, Key(i)).ok());  // the newest: some in the memtable
  }
  EXPECT_GE(Impl(*db).FlushesForTesting(), 4U);

  std::string expected_scan;
  for (int i = 0; i < 300; ++i) {
    const std::string expected = i % 2 == 0 ? "NOT_FOUND" : Value(i, 3);
    ASSERT_EQ(Get(*db, Key(i)), expected) << i;
    if (i % 2 != 0) {
      expected_scan += Key(i) + " ";
    }
  }
  std::string scan;
  const std::unique_ptr<Iterator> it = db->NewIterator(ReadOptions{});
  for (it->SeekToFirst(); it->Valid(); it->Next()) {
    scan.append(it->key()).append(" ");
  }
  EXPECT_EQ(scan, expected_scan);
  it->Seek(Key(100));
  ASSERT_TRUE(it->Valid());
  EXPECT_EQ(it->key(), Key(101));
}

TEST_F(FlushTest, ReopenAfterFlushes) {
  {
    const std::unique_ptr<DB> db = Open();
    for (int i = 0; i < 500; ++i) {
      ASSERT_TRUE(db->Put({}, Key(i), Value(i)).ok());
    }
    // Otherwise a flush still queued at the close is left to the logs, and
    // the open replays two of them.
    Impl(*db).WaitForFlushForTesting();
  }
  const std::unique_ptr<DB> db = Open();
  const RecoveryInfo& info = Impl(*db).GetRecoveryInfo();
  EXPECT_EQ(info.logs_replayed.size(), 1U) << "only the log written since the last flush";
  for (int i = 0; i < 500; ++i) {
    ASSERT_EQ(Get(*db, Key(i)), Value(i)) << i;
  }
}

TEST_F(FlushTest, AnIteratorKeepsItsViewThroughFlushes) {
  const std::unique_ptr<DB> db = Open();
  for (int i = 0; i < 100; ++i) {
    ASSERT_TRUE(db->Put({}, Key(i), Value(i)).ok());
  }
  const std::unique_ptr<Iterator> it = db->NewIterator(ReadOptions{});
  const std::uint64_t flushes_before = Impl(*db).FlushesForTesting();
  for (int i = 0; i < 1000; ++i) {
    ASSERT_TRUE(db->Put({}, Key(i), Value(i, 1)).ok());
  }
  ASSERT_GT(Impl(*db).FlushesForTesting(), flushes_before);
  int count = 0;
  for (it->SeekToFirst(); it->Valid(); it->Next(), ++count) {
    ASSERT_EQ(it->value(), Value(count)) << "the view from before the writes";
  }
  EXPECT_EQ(count, 100);
}

TEST_F(FlushTest, WorksWithoutBlockCacheOrFilters) {
  Options options = SmallMemtable();
  options.block_cache_bytes = 0;
  options.filter_bits_per_key = 0;
  options.block_size = 256;
  const std::unique_ptr<DB> db = Open(options);
  for (int i = 0; i < 300; ++i) {
    ASSERT_TRUE(db->Put({}, Key(i), Value(i)).ok());
  }
  ASSERT_GT(Impl(*db).FlushesForTesting(), 0U);
  for (int i = 0; i < 300; ++i) {
    ASSERT_EQ(Get(*db, Key(i)), Value(i));
  }
}

// Writers fill memtables (flushes happen under the readers) while readers
// check that every acknowledged key reads back, by Get and by iterator.
TEST_F(FlushTest, ReadsDuringFlushes) {
  constexpr int kWriters = 4;
  constexpr int kKeysPerWriter = 400;
  const std::unique_ptr<DB> db = Open();
  std::array<std::atomic<int>, kWriters> written{};
  std::atomic<bool> done{false};
  std::atomic<int> wrong{0};

  std::vector<std::thread> threads;
  threads.reserve(kWriters + 3);
  for (int w = 0; w < kWriters; ++w) {
    threads.emplace_back([&, w] {
      for (int i = 0; i < kKeysPerWriter; ++i) {
        const int key = (i * kWriters) + w;
        if (!db->Put({}, Key(key), Value(key)).ok()) {
          wrong.fetch_add(1);
        }
        written[static_cast<std::size_t>(w)].store(i + 1, std::memory_order_release);
      }
    });
  }
  for (int r = 0; r < 3; ++r) {
    threads.emplace_back([&, r] {
      int round = 0;
      while (!done.load(std::memory_order_acquire)) {
        const int w = (round++ + r) % kWriters;
        const int n = written[static_cast<std::size_t>(w)].load(std::memory_order_acquire);
        if (n > 0) {
          const int key = (((round * 7) % n) * kWriters) + w;
          if (Get(*db, Key(key)) != Value(key)) {
            wrong.fetch_add(1);
          }
        }
        if (round % 50 == 0) {
          const std::unique_ptr<Iterator> it = db->NewIterator(ReadOptions{});
          std::string previous;
          for (it->SeekToFirst(); it->Valid(); it->Next()) {
            if (std::string(it->key()) <= previous) {
              wrong.fetch_add(1);
            }
            previous = std::string(it->key());
          }
          if (!it->status().ok()) {
            wrong.fetch_add(1);
          }
        }
      }
    });
  }
  for (int w = 0; w < kWriters; ++w) {
    threads[static_cast<std::size_t>(w)].join();
  }
  done.store(true, std::memory_order_release);
  for (std::size_t t = kWriters; t < threads.size(); ++t) {
    threads[t].join();
  }
  EXPECT_EQ(wrong.load(), 0);
  EXPECT_GE(Impl(*db).FlushesForTesting(), 5U);
  for (int key = 0; key < kWriters * kKeysPerWriter; ++key) {
    ASSERT_EQ(Get(*db, Key(key)), Value(key));
  }
}

// --- Failures --------------------------------------------------------------------

// A flush that fails at any of its syncs (the old log, the table, the
// MANIFEST) refuses later writes, keeps serving reads, and loses nothing:
// the old log is deleted only after the MANIFEST is synced.
TEST(FlushFailureTest, AFailedFlushRefusesWritesAndLosesNothing) {
  for (int failing_sync = 1; failing_sync <= 3; ++failing_sync) {
    SCOPED_TRACE(failing_sync);
    const auto fs = std::make_shared<FaultInjectionFileSystem>(std::make_shared<MemFileSystem>());
    int written = 0;
    {
      const std::unique_ptr<DB> db = std::move(OpenDB(SmallMemtable(), "db", fs)).value();
      fs->FailNth(FaultInjectionFileSystem::Op::kSync, failing_sync);
      Status status;
      while (status.ok() && written < 10000) {
        status = db->Put({}, Key(written), Value(written));
        written += status.ok() ? 1 : 0;
      }
      ASSERT_FALSE(status.ok()) << "a flush was attempted and failed";
      EXPECT_FALSE(db->Put({}, "after", "failure").ok()) << "sticky";
      for (int i = 0; i < written; ++i) {
        ASSERT_EQ(Get(*db, Key(i)), Value(i)) << "reads go on";
      }
    }
    fs->ClearFaults();
    const std::unique_ptr<DB> db = std::move(OpenDB(SmallMemtable(), "db", fs)).value();
    for (int i = 0; i < written; ++i) {
      ASSERT_EQ(Get(*db, Key(i)), Value(i)) << i;
    }
  }
}

// --- Damage to the MANIFEST's last record ----------------------------------------
//
// A flush's MANIFEST record stays the last one until the next flush. If it is
// damaged later (a bit flip, a truncation), recovery cannot tell it from a
// write cut short and ignores it (ADR 0007). The flush's table is then
// unlisted, so its data must still be in a log: the log the flush retired is
// kept until the next MANIFEST record. Every byte of the record is damaged in
// turn, and every truncation of it is tried; nothing may be lost.

std::string ReadAll(FileSystem& fs, const std::filesystem::path& path) {
  const std::unique_ptr<SequentialFile> file = std::move(fs.NewSequentialFile(path)).value();
  std::string contents;
  std::string buffer(4096, '\0');
  while (true) {
    const std::size_t read = std::move(file->Read(std::span<char>(buffer))).value();
    if (read == 0) {
      return contents;
    }
    contents.append(buffer, 0, read);
  }
}

void Overwrite(FileSystem& fs, const std::filesystem::path& path, std::string_view contents) {
  const std::unique_ptr<WritableFile> file = std::move(fs.NewWritableFile(path)).value();
  ASSERT_TRUE(file->Append(contents).ok());
  ASSERT_TRUE(file->Close().ok());
}

// A database whose MANIFEST ends with the record of its third flush. The new
// log holds the write that set the flush off and `writes_after` more. (Were
// the retired log deleted, the open would fail: that log's sequence numbers
// would be missing between the MANIFEST and the new log.)
struct FlushedDatabase {
  std::shared_ptr<MemFileSystem> fs = std::make_shared<MemFileSystem>();
  std::filesystem::path manifest;
  std::uint64_t last_record = 0;  // where the last flush's record starts
  int written = 0;
};

FlushedDatabase BuildFlushedDatabase(int writes_after) {
  FlushedDatabase built;
  const std::unique_ptr<DB> db = std::move(OpenDB(SmallMemtable(), "db", built.fs)).value();
  Impl(*db).SetSynchronousFlushForTesting(true);  // a flush is done when its write returns
  built.manifest = DescriptorFileName("db", Impl(*db).GetRecoveryInfo().new_manifest_number);
  std::uint64_t flushes = 0;
  while (flushes < 3) {
    const std::uint64_t size = std::move(built.fs->GetFileSize(built.manifest)).value();
    EXPECT_TRUE(db->Put({}, Key(built.written), Value(built.written)).ok());
    ++built.written;
    if (Impl(*db).FlushesForTesting() != flushes) {
      flushes = Impl(*db).FlushesForTesting();
      built.last_record = size;
    }
  }
  for (int i = 0; i < writes_after; ++i) {
    EXPECT_TRUE(db->Put({}, Key(built.written), Value(built.written)).ok());
    ++built.written;
  }
  return built;
}

void ExpectAllThere(const FlushedDatabase& built) {
  const Result<std::unique_ptr<DB>> db = OpenDB(SmallMemtable(), "db", built.fs);
  ASSERT_TRUE(db.ok()) << db.status().ToString();
  EXPECT_TRUE(Impl(**db).GetRecoveryInfo().manifest.torn_tail.has_value())
      << "the damaged record is ignored as a torn tail";
  for (int i = 0; i < built.written; ++i) {
    ASSERT_EQ(Get(**db, Key(i)), Value(i)) << i;
  }
}

TEST(ManifestDamageTest, DamageToTheLastRecordLosesNothing) {
  for (const int writes_after : {0, 10}) {
    const std::uint64_t size = [&] {
      const FlushedDatabase built = BuildFlushedDatabase(writes_after);
      return std::move(built.fs->GetFileSize(built.manifest)).value();
    }();
    const std::uint64_t first = BuildFlushedDatabase(writes_after).last_record;
    ASSERT_GT(size, first);
    for (std::uint64_t offset = first; offset < size; ++offset) {
      SCOPED_TRACE("writes after the flush: " + std::to_string(writes_after) +
                   ", byte flipped at " + std::to_string(offset));
      const FlushedDatabase built = BuildFlushedDatabase(writes_after);
      std::string contents = ReadAll(*built.fs, built.manifest);
      contents[offset] = static_cast<char>(contents[offset] ^ 0x20);
      Overwrite(*built.fs, built.manifest, contents);
      ExpectAllThere(built);
    }
    for (std::uint64_t length = first + 1; length < size; ++length) {
      SCOPED_TRACE("writes after the flush: " + std::to_string(writes_after) + ", truncated to " +
                   std::to_string(length));
      const FlushedDatabase built = BuildFlushedDatabase(writes_after);
      const std::string contents = ReadAll(*built.fs, built.manifest);
      Overwrite(*built.fs, built.manifest, std::string_view(contents).substr(0, length));
      ExpectAllThere(built);
    }
  }
}

// --- Crashes at every step (TODO 3.9) ---------------------------------------------
//
// The crash torture test crashes at random points; these crash at every file
// operation in turn, so every step of a flush and of an open (table, new log,
// MANIFEST, CURRENT.tmp, the rename, the deletions) is hit. Flushes are made
// synchronous (SetSynchronousFlushForTesting), so the background thread's
// operations come in the same order in every run. Each crash is
// followed by a power loss or a process crash; renames and removals may be
// undone (the fault-injection defaults). The reopen must keep every
// acknowledged synced write and nothing after the failed one.

using CrashKind = FaultInjectionFileSystem::CrashKind;

std::shared_ptr<FaultInjectionFileSystem> NewFaultFs(std::uint64_t seed) {
  FaultInjectionFileSystem::Options options;
  options.seed = seed;
  return std::make_shared<FaultInjectionFileSystem>(std::make_shared<MemFileSystem>(), options);
}

// Puts keys [first, first + count) with sync; returns how many were acknowledged.
int WriteSynced(DB& db, int first, int count) {
  for (int i = 0; i < count; ++i) {
    if (!db.Put({.sync = true}, Key(first + i), Value(first + i)).ok()) {
      return i;
    }
  }
  return count;
}

// Keys [0, acknowledged) are there; keys after `acknowledged` (the failed
// write, which may or may not be there) are not.
void ExpectSyncedPrefix(DB& db, int acknowledged, int attempted) {
  for (int i = 0; i < acknowledged; ++i) {
    ASSERT_EQ(Get(db, Key(i)), Value(i)) << "acknowledged synced write " << i;
  }
  for (int i = acknowledged + 1; i < attempted; ++i) {
    ASSERT_EQ(Get(db, Key(i)), "NOT_FOUND") << "never attempted " << i;
  }
}

TEST(FlushCrashTest, ACrashAtAnyStepOfAFlushLosesNoSyncedWrite) {
  constexpr int kWrites = 120;  // ~220-byte values into 16 KiB memtables: several flushes
  std::uint64_t total = 0;
  {
    const auto fs = NewFaultFs(1);
    const std::unique_ptr<DB> db = std::move(OpenDB(SmallMemtable(), "db", fs)).value();
    Impl(*db).SetSynchronousFlushForTesting(true);  // the same operations in every run
    const std::uint64_t base = fs->OperationCount();
    ASSERT_EQ(WriteSynced(*db, 0, kWrites), kWrites);
    ASSERT_GE(Impl(*db).FlushesForTesting(), 2U);
    total = fs->OperationCount() - base;
    std::printf(  // NOLINT(cppcoreguidelines-pro-type-vararg)
        "[ crash    ] %llu writes with %llu flushes: crashing at each of %llu operations, twice\n",
        static_cast<unsigned long long>(kWrites),
        static_cast<unsigned long long>(Impl(*db).FlushesForTesting()),
        static_cast<unsigned long long>(total));
  }
  for (std::uint64_t step = 1; step <= total; ++step) {
    for (const CrashKind kind : {CrashKind::kPowerLoss, CrashKind::kProcessCrash}) {
      SCOPED_TRACE("crash at operation " + std::to_string(step) + " of " + std::to_string(total) +
                   (kind == CrashKind::kPowerLoss ? ", power loss" : ", process crash"));
      const auto fs = NewFaultFs(step);
      int acknowledged = 0;
      {
        const std::unique_ptr<DB> db = std::move(OpenDB(SmallMemtable(), "db", fs)).value();
        Impl(*db).SetSynchronousFlushForTesting(true);
        fs->CrashAtOperation(step);
        acknowledged = WriteSynced(*db, 0, kWrites);
      }
      ASSERT_TRUE(fs->SimulateCrash(kind).ok());
      fs->ClearFaults();
      const Result<std::unique_ptr<DB>> db = OpenDB(SmallMemtable(), "db", fs);
      ASSERT_TRUE(db.ok()) << db.status().ToString();
      ExpectSyncedPrefix(**db, acknowledged, kWrites);
    }
  }
}

// Puts kOpenTestWrites synced keys into a new database with tables and a log
// that the next open must turn into a table, a new log, a new MANIFEST and a
// CURRENT switch.
constexpr int kOpenTestWrites = 100;

std::shared_ptr<FaultInjectionFileSystem> DatabaseToReopen(std::uint64_t seed) {
  auto fs = NewFaultFs(seed);
  const std::unique_ptr<DB> db = std::move(OpenDB(SmallMemtable(), "db", fs)).value();
  Impl(*db).SetSynchronousFlushForTesting(true);  // the same files to reopen every time
  EXPECT_EQ(WriteSynced(*db, 0, kOpenTestWrites), kOpenTestWrites);
  return fs;
}

TEST(FlushCrashTest, ACrashAtAnyStepOfAnOpenLosesNothing) {
  constexpr int kWrites = kOpenTestWrites;
  const auto make_database = &DatabaseToReopen;
  std::uint64_t total = 0;
  {
    const auto fs = make_database(1);
    const std::uint64_t base = fs->OperationCount();
    ASSERT_TRUE(OpenDB(SmallMemtable(), "db", fs).ok());
    total = fs->OperationCount() - base;
  }
  ASSERT_GE(total, 8U) << "table, log, MANIFEST, CURRENT.tmp, rename, deletions";
  std::printf(  // NOLINT(cppcoreguidelines-pro-type-vararg)
      "[ crash    ] an open: crashing at each of its %llu operations, twice\n",
      static_cast<unsigned long long>(total));
  for (std::uint64_t step = 1; step <= total; ++step) {
    for (const CrashKind kind : {CrashKind::kPowerLoss, CrashKind::kProcessCrash}) {
      SCOPED_TRACE("crash at operation " + std::to_string(step) + " of " + std::to_string(total) +
                   (kind == CrashKind::kPowerLoss ? ", power loss" : ", process crash"));
      const auto fs = make_database(step);
      fs->CrashAtOperation(step);
      static_cast<void>(OpenDB(SmallMemtable(), "db", fs));  // dies somewhere inside
      ASSERT_TRUE(fs->SimulateCrash(kind).ok());
      fs->ClearFaults();
      const Result<std::unique_ptr<DB>> db = OpenDB(SmallMemtable(), "db", fs);
      ASSERT_TRUE(db.ok()) << db.status().ToString();
      ExpectSyncedPrefix(**db, kWrites, kWrites);
    }
  }
}

// --- Files -------------------------------------------------------------------------

TEST_F(FlushTest, OpenDeletesFilesTheManifestDoesNotNeed) {
  {
    const std::unique_ptr<DB> db = Open();
    ASSERT_TRUE(db->Put({}, "k", "v").ok());
  }
  // Leftovers of crashes: an unfinished table, an older MANIFEST, a temp
  // CURRENT, an old log; and files that are not ForgeDB's.
  WriteFile(TableFileName(db_, 50), "half a table");
  WriteFile(DescriptorFileName(db_, 1), "");
  WriteFile(CurrentTempFileName(db_), "MANIFEST-000001\n");
  WriteFile(db_ / "notes.txt", "mine");
  WriteFile(db_ / "12.sst", "not a ForgeDB name");

  const std::unique_ptr<DB> db = Open();
  EXPECT_EQ(Get(*db, "k"), "v");
  const RecoveryInfo& info = Impl(*db).GetRecoveryInfo();
  for (const std::string_view gone :
       {"000050.sst", "MANIFEST-000001", "000001.log", "MANIFEST-000002"}) {
    EXPECT_NE(std::ranges::find(info.files_deleted, gone), info.files_deleted.end()) << gone;
    EXPECT_FALSE(*fs_->FileExists(db_ / gone)) << gone;
  }
  // The open writes CURRENT.tmp itself and renames it over CURRENT.
  EXPECT_FALSE(*fs_->FileExists(CurrentTempFileName(db_)));
  EXPECT_TRUE(*fs_->FileExists(db_ / "notes.txt"));
  EXPECT_TRUE(*fs_->FileExists(db_ / "12.sst"));
  EXPECT_EQ(Files(FileType::kDescriptor),
            (std::vector<std::string>{DescriptorFileName("", info.new_manifest_number).string()}));
  EXPECT_GT(info.new_log_number, 50U) << "file numbers continue above the leftover";
}

}  // namespace
}  // namespace forgedb
