// DB::GetStats (TODO 4.4; forgedb/stats.h): every byte count is checked
// exactly against the files it describes.
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>

#include <gtest/gtest.h>

#include <memory>
#include <optional>
#include <span>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include "forgedb/db.h"
#include "forgedb/options.h"
#include "forgedb/result.h"
#include "forgedb/stats.h"
#include "forgedb/status.h"
#include "forgedb/write_batch.h"

#include "db/db_impl.h"
#include "db/filename.h"
#include "db/version_edit.h"
#include "db/version_set.h"
#include "env/file_system.h"
#include "env/mem_file_system.h"
#include "support/fault_injection_file_system.h"
#include "support/gated_file_system.h"
#include "tools/cli.h"

namespace forgedb {
namespace {

DBImpl& Impl(DB& db) { return dynamic_cast<DBImpl&>(db); }

// "key0000042": 10 bytes, sorted by i.
std::string Key(int i) {
  const std::string digits = std::to_string(i);
  return "key" + std::string(7 - digits.size(), '0') + digits;
}

// Only manual compactions.
Options NoAutomaticCompaction() {
  Options options;
  options.memtable_bytes = std::size_t{1} << 20;
  options.level0_compaction_trigger = 1 << 30;
  options.level0_slowdown_writes_trigger = 1 << 30;
  options.level0_stop_writes_trigger = 1 << 30;
  options.target_file_size = std::size_t{4} << 10;  // several outputs
  return options;
}

std::uint64_t SizeOf(const std::vector<FileMetaData>& files) {
  std::uint64_t bytes = 0;
  for (const FileMetaData& file : files) {
    bytes += file.file_size;
  }
  return bytes;
}

std::uint64_t CompactionBytesWritten(const DBStats& stats) {
  std::uint64_t bytes = 0;
  for (const LevelStats& level : stats.levels) {
    bytes += level.compactions.bytes_written;
  }
  return bytes;
}

class StatsTest : public testing::Test {
 protected:
  std::unique_ptr<DB> Open(const Options& options) {
    Result<std::unique_ptr<DB>> db = OpenDB(options, "db", fs_);
    EXPECT_TRUE(db.ok()) << db.status().ToString();
    return db.ok() ? std::move(db).value() : nullptr;
  }

  // Writes `count` keys from `first` with 100-byte values; returns the bytes.
  static std::uint64_t Fill(DB& db, int first, int count) {
    std::uint64_t bytes = 0;
    for (int i = first; i < first + count; ++i) {
      const std::string key = Key(i);
      const std::string value(100, static_cast<char>('a' + (i % 26)));
      EXPECT_TRUE(db.Put({}, key, value).ok());
      bytes += key.size() + value.size();
    }
    return bytes;
  }

  // Every level's size in the stats matches the current version.
  static void ExpectLevelsMatchTheVersion(DBImpl& db) {
    const DBStats stats = db.GetStats();
    const std::shared_ptr<const Version> version = db.CurrentVersion();
    ASSERT_EQ(stats.levels.size(), version->files.size());
    for (std::size_t level = 0; level < version->files.size(); ++level) {
      EXPECT_EQ(stats.levels[level].tables, version->files[level].size()) << level;
      EXPECT_EQ(stats.levels[level].bytes, SizeOf(version->files[level])) << level;
    }
  }

  std::shared_ptr<MemFileSystem> fs_ = std::make_shared<MemFileSystem>();
};

TEST_F(StatsTest, ANewDatabaseHasDoneNothing) {
  const std::unique_ptr<DB> db = Open(Options{});
  ASSERT_NE(db, nullptr);
  const DBStats stats = db->GetStats();
  ASSERT_EQ(stats.levels.size(), static_cast<std::size_t>(kNumLevels));
  for (const LevelStats& level : stats.levels) {
    EXPECT_EQ(level.tables, 0U);
    EXPECT_EQ(level.bytes, 0U);
    EXPECT_EQ(level.compactions.count + level.compactions.trivial_moves, 0U);
    EXPECT_EQ(level.compactions.microseconds, 0U);
  }
  EXPECT_EQ(stats.user_bytes, 0U);
  EXPECT_EQ(stats.log_bytes, 0U);
  EXPECT_EQ(stats.flushes, 0U);
  EXPECT_EQ(stats.TableBytesWritten(), 0U);
  EXPECT_FALSE(stats.WriteAmplification().has_value());
  EXPECT_EQ(stats.memtable_full.count + stats.level0_slowdown.count + stats.level0_stop.count, 0U);
}

TEST_F(StatsTest, UserBytesAreKeysPlusValuesAndLogBytesAreTheLogFile) {
  const std::unique_ptr<DB> db = Open(Options{});
  ASSERT_NE(db, nullptr);
  ASSERT_TRUE(db->Put({}, "abc", "12345").ok());  // 3 + 5
  ASSERT_TRUE(db->Delete({}, "xy").ok());         // 2
  WriteBatch batch;
  batch.Put("k", "v");  // 2
  batch.Delete("zz");   // 2
  ASSERT_TRUE(db->Write({}, batch).ok());
  ASSERT_TRUE(db->Write({}, WriteBatch()).ok());  // nothing
  EXPECT_EQ(db->GetStats().user_bytes, 14U);

  // A write refused before it reaches the log adds nothing.
  EXPECT_FALSE(db->Put({}, std::string(kMaxKeySize + 1, 'k'), "v").ok());
  EXPECT_EQ(db->GetStats().user_bytes, 14U);

  // The open started one log, and nothing has switched it.
  const std::uint64_t log_number = Impl(*db).GetRecoveryInfo().new_log_number;
  const Result<std::uint64_t> log_size = fs_->GetFileSize(LogFileName("db", log_number));
  ASSERT_TRUE(log_size.ok());
  EXPECT_EQ(db->GetStats().log_bytes, *log_size);
  EXPECT_GT(*log_size, 14U);
}

TEST_F(StatsTest, AFlushCountsTheTableItWrote) {
  const std::unique_ptr<DB> db = Open(NoAutomaticCompaction());
  ASSERT_NE(db, nullptr);
  const std::uint64_t user_bytes = Fill(*db, 0, 200);
  const auto start = std::chrono::steady_clock::now();
  ASSERT_TRUE(Impl(*db).FlushForTesting().ok());
  const auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(
      std::chrono::steady_clock::now() - start);

  const DBStats stats = db->GetStats();
  const std::shared_ptr<const Version> version = Impl(*db).CurrentVersion();
  ASSERT_EQ(version->files[0].size(), 1U);
  EXPECT_EQ(stats.user_bytes, user_bytes);
  EXPECT_EQ(stats.flushes, 1U);
  EXPECT_EQ(stats.flush_bytes_written, version->files[0][0].file_size);
  EXPECT_GT(stats.flush_microseconds, 0U);
  EXPECT_LE(stats.flush_microseconds, static_cast<std::uint64_t>(elapsed.count()));
  EXPECT_EQ(stats.TableBytesWritten(), stats.flush_bytes_written);
  ASSERT_TRUE(stats.WriteAmplification().has_value());
  EXPECT_DOUBLE_EQ(
      stats.WriteAmplification().value_or(0),
      static_cast<double>(stats.flush_bytes_written) / static_cast<double>(user_bytes));
  ExpectLevelsMatchTheVersion(Impl(*db));
}

TEST_F(StatsTest, CompactionsCountWhatTheyReadAndWrote) {
  const std::unique_ptr<DB> db = Open(NoAutomaticCompaction());
  ASSERT_NE(db, nullptr);
  std::uint64_t user_bytes = Fill(*db, 0, 300);
  ASSERT_TRUE(Impl(*db).FlushForTesting().ok());
  user_bytes += Fill(*db, 150, 300);  // overlaps the first table
  ASSERT_TRUE(Impl(*db).FlushForTesting().ok());

  const std::uint64_t level0_bytes = SizeOf(Impl(*db).CurrentVersion()->files[0]);
  ASSERT_TRUE(Impl(*db).CompactLevelForTesting(0).ok());
  {
    const DBStats stats = db->GetStats();
    const CompactionStats& compactions = stats.levels[0].compactions;
    EXPECT_EQ(compactions.count, 1U);
    EXPECT_EQ(compactions.trivial_moves, 0U);
    EXPECT_EQ(compactions.bytes_read, level0_bytes) << "level 1 was empty";
    EXPECT_EQ(compactions.bytes_written, SizeOf(Impl(*db).CurrentVersion()->files[1]));
    EXPECT_GT(compactions.microseconds, 0U);
    EXPECT_LT(compactions.bytes_written, compactions.bytes_read) << "150 overwritten keys dropped";
    ExpectLevelsMatchTheVersion(Impl(*db));
  }

  // Again, over every key: this time every level-1 table overlaps and is read.
  user_bytes += Fill(*db, 0, 450);
  ASSERT_TRUE(Impl(*db).FlushForTesting().ok());
  {
    const std::shared_ptr<const Version> before = Impl(*db).CurrentVersion();
    const std::uint64_t inputs = SizeOf(before->files[0]) + SizeOf(before->files[1]);
    ASSERT_TRUE(Impl(*db).CompactLevelForTesting(0).ok());
    const CompactionStats compactions = db->GetStats().levels[0].compactions;
    EXPECT_EQ(compactions.count, 2U);
    EXPECT_EQ(compactions.bytes_read, level0_bytes + inputs);
  }

  // Down to the last level, one table at a time. Tables of one level have
  // disjoint key ranges, so what an earlier table wrote below never overlaps
  // a later one: each table is read once and nothing else is.
  const std::vector<FileMetaData> level1 = Impl(*db).CurrentVersion()->files[1];
  ASSERT_GT(level1.size(), 1U) << "target_file_size cut several tables";
  ASSERT_TRUE(Impl(*db).CompactAllForTesting().ok());
  const DBStats stats = db->GetStats();
  EXPECT_EQ(stats.levels[1].compactions.count, level1.size());
  EXPECT_EQ(stats.levels[1].compactions.bytes_read, SizeOf(level1));
  for (int level = 1; level < kNumLevels - 1; ++level) {
    EXPECT_GE(stats.levels[static_cast<std::size_t>(level)].compactions.count, 1U) << level;
  }
  EXPECT_EQ(stats.levels[6].compactions.count, 0U) << "the last level is never compacted";
  EXPECT_EQ(stats.levels[5].compactions.bytes_written,
            SizeOf(Impl(*db).CurrentVersion()->files[6]));
  EXPECT_EQ(stats.flushes, 3U) << "CompactAllForTesting's flush had an empty memtable";
  EXPECT_EQ(stats.TableBytesWritten(), stats.flush_bytes_written + CompactionBytesWritten(stats));
  EXPECT_DOUBLE_EQ(
      stats.WriteAmplification().value_or(0),
      static_cast<double>(stats.TableBytesWritten()) / static_cast<double>(user_bytes));
  ExpectLevelsMatchTheVersion(Impl(*db));
}

TEST_F(StatsTest, ATrivialMoveWritesNothing) {
  Options options = NoAutomaticCompaction();
  options.level0_compaction_trigger = 1;  // one table: moved straight to level 1
  const std::unique_ptr<DB> db = Open(options);
  ASSERT_NE(db, nullptr);
  Fill(*db, 0, 50);
  ASSERT_TRUE(Impl(*db).FlushForTesting().ok());  // waits for the compaction too
  const DBStats stats = db->GetStats();
  const CompactionStats& compactions = stats.levels[0].compactions;
  EXPECT_EQ(compactions.trivial_moves, 1U);
  EXPECT_EQ(compactions.count, 0U);
  EXPECT_EQ(compactions.bytes_read, 0U);
  EXPECT_EQ(compactions.bytes_written, 0U);
  EXPECT_EQ(stats.levels[0].tables, 0U);
  EXPECT_EQ(stats.levels[1].tables, 1U);
  EXPECT_EQ(stats.TableBytesWritten(), stats.flush_bytes_written);
}

TEST_F(StatsTest, TheCountersStartAgainAtEachOpenAndRecoveryIsNotCounted) {
  {
    const std::unique_ptr<DB> db = Open(NoAutomaticCompaction());
    ASSERT_NE(db, nullptr);
    Fill(*db, 0, 100);
    ASSERT_TRUE(Impl(*db).FlushForTesting().ok());
    Fill(*db, 100, 100);  // left in the log: the next open writes a table
  }
  const std::unique_ptr<DB> db = Open(NoAutomaticCompaction());
  ASSERT_NE(db, nullptr);
  ASSERT_TRUE(Impl(*db).GetRecoveryInfo().table_written.has_value());
  const DBStats stats = db->GetStats();
  EXPECT_EQ(stats.levels[0].tables, 2U) << "the data is still there";
  EXPECT_EQ(stats.user_bytes, 0U);
  EXPECT_EQ(stats.log_bytes, 0U);
  EXPECT_EQ(stats.flushes, 0U);
  EXPECT_EQ(stats.TableBytesWritten(), 0U) << "the recovery table belongs to the last open";
  EXPECT_FALSE(stats.WriteAmplification().has_value());
}

TEST(StatsErrorTest, AFailedCompactionAddsNothing) {
  Options options;
  options.memtable_bytes = std::size_t{8} << 10;
  options.level0_compaction_trigger = 2;
  const auto fs = std::make_shared<FaultInjectionFileSystem>(std::make_shared<MemFileSystem>());
  const std::unique_ptr<DB> db = std::move(OpenDB(options, "db", fs)).value();
  Impl(*db).SetSynchronousFlushForTesting(true);
  int written = 0;
  while (Impl(*db).FlushesForTesting() < 1) {
    ASSERT_TRUE(db->Put({}, Key(written++), std::string(100, 'v')).ok());
  }
  // As in CompactionErrorTest: the compaction after the second flush fails
  // syncing its output table.
  fs->FailNth(FaultInjectionFileSystem::Op::kSync, 4);
  while (db->Put({}, Key(written), std::string(100, 'v')).ok()) {
    ++written;
  }
  const DBStats stats = db->GetStats();
  EXPECT_EQ(stats.flushes, 2U);
  for (const LevelStats& level : stats.levels) {
    EXPECT_EQ(level.compactions.count + level.compactions.trivial_moves, 0U);
    EXPECT_EQ(level.compactions.bytes_written, 0U);
    EXPECT_EQ(level.compactions.microseconds, 0U);
  }
  EXPECT_EQ(stats.user_bytes, static_cast<std::uint64_t>(written) * (10 + 100))
      << "the refused writes are not counted";
}

// Slows down the sync of every table holding `marker` (its bytes appear in
// the table's data blocks). Everything else is forwarded.
class SlowTableFileSystem final : public FileSystem {
 public:
  SlowTableFileSystem(std::shared_ptr<FileSystem> base, std::string marker,
                      std::chrono::milliseconds delay)
      : base_(std::move(base)), marker_(std::move(marker)), delay_(delay) {}

  Result<std::unique_ptr<WritableFile>> NewWritableFile(
      const std::filesystem::path& path) override {
    FORGEDB_ASSIGN_OR_RETURN(std::unique_ptr<WritableFile> file, base_->NewWritableFile(path));
    if (path.extension() != ".sst") {
      return file;
    }
    return std::unique_ptr<WritableFile>(std::make_unique<File>(std::move(file), this));
  }
  Result<std::unique_ptr<SequentialFile>> NewSequentialFile(
      const std::filesystem::path& path) override {
    return base_->NewSequentialFile(path);
  }
  Result<std::unique_ptr<RandomAccessFile>> NewRandomAccessFile(
      const std::filesystem::path& path) override {
    return base_->NewRandomAccessFile(path);
  }
  Status RenameReplace(const std::filesystem::path& from,
                       const std::filesystem::path& to) override {
    return base_->RenameReplace(from, to);
  }
  Status RemoveFile(const std::filesystem::path& path) override { return base_->RemoveFile(path); }
  Result<bool> FileExists(const std::filesystem::path& path) override {
    return base_->FileExists(path);
  }
  Result<std::vector<std::string>> GetChildren(const std::filesystem::path& dir) override {
    return base_->GetChildren(dir);
  }
  Result<std::uint64_t> GetFileSize(const std::filesystem::path& path) override {
    return base_->GetFileSize(path);
  }
  Status SyncFile(const std::filesystem::path& path) override { return base_->SyncFile(path); }
  Status CreateDir(const std::filesystem::path& path) override { return base_->CreateDir(path); }
  Result<std::unique_ptr<FileLock>> LockFile(const std::filesystem::path& path) override {
    return base_->LockFile(path);
  }
  Status UnlockFile(std::unique_ptr<FileLock> lock) override {
    return base_->UnlockFile(std::move(lock));
  }

  [[nodiscard]] int SlowedSyncs() const { return slowed_.load(); }

 private:
  class File final : public WritableFile {
   public:
    File(std::unique_ptr<WritableFile> base, SlowTableFileSystem* fs)
        : base_(std::move(base)), fs_(fs) {}
    Status Append(std::string_view data) override {
      marked_ = marked_ || data.find(fs_->marker_) != std::string_view::npos;
      return base_->Append(data);
    }
    Status Flush() override { return base_->Flush(); }
    Status Sync() override {
      if (marked_) {
        std::this_thread::sleep_for(fs_->delay_);
        fs_->slowed_.fetch_add(1);
      }
      return base_->Sync();
    }
    Status Close() override { return base_->Close(); }

   private:
    std::unique_ptr<WritableFile> base_;
    SlowTableFileSystem* fs_;
    bool marked_ = false;
  };

  std::shared_ptr<FileSystem> base_;
  std::string marker_;
  std::chrono::milliseconds delay_;
  std::atomic<int> slowed_{0};
};

// A flush run in the middle of a compaction is flush time, not compaction
// time: the background thread does one thing at a time, so its flush and
// compaction times add up to no more than the time that passed.
TEST(StatsTimeTest, AFlushInsideACompactionIsNotCompactionTime) {
  constexpr auto kDelay = std::chrono::milliseconds(200);
  Options options;
  options.memtable_bytes = std::size_t{16} << 10;
  options.level0_compaction_trigger = 2;
  options.target_file_size = std::size_t{2} << 10;  // many outputs: the gate holds the first
  Gate gate;
  const auto slow = std::make_shared<SlowTableFileSystem>(
      std::make_shared<GatedFileSystem>(std::make_shared<MemFileSystem>(), &gate), "flushmark",
      kDelay);
  const auto start = std::chrono::steady_clock::now();
  const std::unique_ptr<DB> db = std::move(OpenDB(options, "db", slow)).value();
  gate.Close(/*free_passes=*/2);  // the two flushes pass; the compaction's first output waits
  int written = 0;
  while (Impl(*db).FlushesForTesting() < 2) {
    ASSERT_TRUE(db->Put({}, Key(written++), std::string(20, 'v')).ok());
  }
  gate.WaitForArrival();
  // Queue a flush of keys the compaction does not have: only its table is
  // slow. The compaction runs it at its next check.
  for (int i = 0; !Impl(*db).FlushPendingForTesting(); ++i) {
    ASSERT_TRUE(db->Put({}, "flushmark" + std::to_string(i), std::string(20, 'v')).ok());
  }
  gate.Open();
  Impl(*db).WaitForBackgroundWorkForTesting();
  const auto elapsed =
      static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(
                                     std::chrono::steady_clock::now() - start)
                                     .count());

  const DBStats stats = db->GetStats();
  ASSERT_EQ(slow->SlowedSyncs(), 1);
  ASSERT_EQ(stats.flushes, 3U);
  ASSERT_EQ(stats.levels[0].compactions.count, 1U) << "the flush ran inside it";
  EXPECT_GE(stats.flush_microseconds,
            static_cast<std::uint64_t>(std::chrono::microseconds(kDelay).count()));
  EXPECT_LE(stats.flush_microseconds + stats.levels[0].compactions.microseconds, elapsed);
}

// Writers race (their batches are grouped) while a reader takes stats: the
// sums stay exact and only grow.
TEST_F(StatsTest, StatsStayExactUnderConcurrentWrites) {
  Options options;
  options.memtable_bytes = std::size_t{32} << 10;
  options.level0_compaction_trigger = 2;
  options.max_bytes_for_level_base = std::size_t{64} << 10;
  options.target_file_size = std::size_t{8} << 10;
  const std::unique_ptr<DB> db = Open(options);
  ASSERT_NE(db, nullptr);
  constexpr int kWriters = 4;
  constexpr int kPerWriter = 2000;
  std::atomic<bool> done{false};
  std::thread reader([&] {
    std::uint64_t last_user = 0;
    std::uint64_t last_table = 0;
    while (!done.load()) {
      const DBStats stats = db->GetStats();
      EXPECT_GE(stats.user_bytes, last_user);
      EXPECT_GE(stats.TableBytesWritten(), last_table);
      last_user = stats.user_bytes;
      last_table = stats.TableBytesWritten();
    }
  });
  std::vector<std::thread> writers;
  writers.reserve(kWriters);
  for (int w = 0; w < kWriters; ++w) {
    writers.emplace_back([&db, w] {
      for (int i = 0; i < kPerWriter; ++i) {
        EXPECT_TRUE(db->Put({}, Key((w * kPerWriter) + i), std::string(50, 'v')).ok());
      }
    });
  }
  for (std::thread& writer : writers) {
    writer.join();
  }
  done.store(true);
  reader.join();
  Impl(*db).WaitForBackgroundWorkForTesting();
  const DBStats stats = db->GetStats();
  EXPECT_EQ(stats.user_bytes, std::uint64_t{kWriters} * kPerWriter * (10 + 50));
  EXPECT_GT(stats.flushes, 5U);
  EXPECT_GT(CompactionBytesWritten(stats), 0U);
  ExpectLevelsMatchTheVersion(Impl(*db));
}

// --- forgedb_cli stats: the "since this open" lines (tools/cli.h) ------------------

TEST(PrintActivityTest, FormatsEveryLine) {
  DBStats stats;
  stats.levels.resize(static_cast<std::size_t>(kNumLevels));
  stats.levels[0].compactions = {.count = 2,
                                 .trivial_moves = 1,
                                 .microseconds = 1234567,
                                 .bytes_read = 1000,
                                 .bytes_written = 900};
  stats.levels[2].compactions = {
      .count = 1, .trivial_moves = 0, .microseconds = 5, .bytes_read = 70, .bytes_written = 60};
  stats.user_bytes = 500;
  stats.log_bytes = 640;
  stats.flushes = 3;
  stats.flush_microseconds = 1000;
  stats.flush_bytes_written = 540;
  stats.level0_slowdown = {.count = 4, .microseconds = 4321};
  std::ostringstream out;
  tools::PrintActivity(stats, out);
  EXPECT_EQ(out.str(),
            "since this open:\n"
            "  user writes: 500 bytes (log: 640 bytes)\n"
            "  flushes: 3 (540 bytes written, 1.000 ms)\n"
            "  compactions level 0 -> 1: 2 (+1 trivial move), read 1000 bytes, wrote 900 bytes, "
            "1234.567 ms\n"
            "  compactions level 2 -> 3: 1 (+0 trivial moves), read 70 bytes, wrote 60 bytes, "
            "0.005 ms\n"
            "  write amplification: 3.00 (1500 table bytes / 500 user bytes)\n"
            "  stalls: memtable full 0 (0.000 ms), level-0 slowdown 4 (4.321 ms), level-0 stop 0 "
            "(0.000 ms)\n");
}

TEST(PrintActivityTest, NothingDone) {
  DBStats stats;
  stats.levels.resize(static_cast<std::size_t>(kNumLevels));
  std::ostringstream out;
  tools::PrintActivity(stats, out);
  EXPECT_NE(out.str().find("  compactions: none\n"), std::string::npos);
  EXPECT_NE(out.str().find("  write amplification: n/a, no writes (0 table bytes written)\n"),
            std::string::npos);
}

}  // namespace
}  // namespace forgedb
