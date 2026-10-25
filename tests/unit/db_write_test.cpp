// The write path: write-ahead log, group commit and sticky write errors
// (TODO 2.2). The DB runs on an in-memory file system wrapped so tests can
// slow down Sync (so writers pile up and groups form) and inject failures.
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>

#include <gtest/gtest.h>

#include <latch>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include "forgedb/db.h"
#include "forgedb/status.h"
#include "forgedb/write_batch.h"

#include "db/db_impl.h"
#include "db/filename.h"
#include "db/write_batch_internal.h"
#include "env/file_system.h"
#include "env/mem_file_system.h"
#include "log/log_reader.h"

namespace forgedb {
namespace {

// Knobs shared by a ControlledFileSystem and the files it opens.
struct Controls {
  std::atomic<bool> fail_appends{false};
  std::atomic<bool> fail_syncs{false};
  std::atomic<int> sync_delay_ms{0};
  std::atomic<int> syncs{0};  // Sync calls that succeeded
};

class ControlledWritableFile final : public WritableFile {
 public:
  ControlledWritableFile(std::unique_ptr<WritableFile> file, std::shared_ptr<Controls> controls)
      : file_(std::move(file)), controls_(std::move(controls)) {}

  Status Append(std::string_view data) override {
    if (controls_->fail_appends) {
      return Status::IOError("Append", "injected failure");
    }
    return file_->Append(data);
  }
  Status Flush() override { return file_->Flush(); }
  Status Sync() override {
    if (const int delay = controls_->sync_delay_ms; delay > 0) {
      std::this_thread::sleep_for(std::chrono::milliseconds(delay));
    }
    if (controls_->fail_syncs) {
      return Status::IOError("Sync", "injected failure");
    }
    FORGEDB_RETURN_IF_ERROR(file_->Sync());
    controls_->syncs.fetch_add(1);
    return Status::OK();
  }
  Status Close() override { return file_->Close(); }

 private:
  std::unique_ptr<WritableFile> file_;
  std::shared_ptr<Controls> controls_;
};

// A MemFileSystem whose writable files obey `controls`.
class ControlledFileSystem final : public FileSystem {
 public:
  ControlledFileSystem() = default;

  Result<std::unique_ptr<WritableFile>> NewWritableFile(
      const std::filesystem::path& path) override {
    FORGEDB_ASSIGN_OR_RETURN(std::unique_ptr<WritableFile> file, base_.NewWritableFile(path));
    return std::unique_ptr<WritableFile>(
        std::make_unique<ControlledWritableFile>(std::move(file), controls_));
  }
  Result<std::unique_ptr<SequentialFile>> NewSequentialFile(
      const std::filesystem::path& path) override {
    return base_.NewSequentialFile(path);
  }
  Result<std::unique_ptr<RandomAccessFile>> NewRandomAccessFile(
      const std::filesystem::path& path) override {
    return base_.NewRandomAccessFile(path);
  }
  Status RenameReplace(const std::filesystem::path& from,
                       const std::filesystem::path& to) override {
    return base_.RenameReplace(from, to);
  }
  Status RemoveFile(const std::filesystem::path& path) override { return base_.RemoveFile(path); }
  Result<bool> FileExists(const std::filesystem::path& path) override {
    return base_.FileExists(path);
  }
  Result<std::vector<std::string>> GetChildren(const std::filesystem::path& dir) override {
    return base_.GetChildren(dir);
  }
  Result<std::uint64_t> GetFileSize(const std::filesystem::path& path) override {
    return base_.GetFileSize(path);
  }
  Status SyncFile(const std::filesystem::path& path) override { return base_.SyncFile(path); }
  Status CreateDir(const std::filesystem::path& path) override { return base_.CreateDir(path); }
  Result<std::unique_ptr<FileLock>> LockFile(const std::filesystem::path& path) override {
    return base_.LockFile(path);
  }
  Status UnlockFile(std::unique_ptr<FileLock> lock) override {
    return base_.UnlockFile(std::move(lock));
  }

  [[nodiscard]] Controls& Knobs() noexcept { return *controls_; }

 private:
  MemFileSystem base_;
  std::shared_ptr<Controls> controls_ = std::make_shared<Controls>();
};

struct TestDB {
  std::shared_ptr<ControlledFileSystem> fs = std::make_shared<ControlledFileSystem>();
  std::unique_ptr<DB> db;
  [[nodiscard]] DBImpl& Impl() const { return dynamic_cast<DBImpl&>(*db); }
};

TestDB OpenTestDB() {
  TestDB test;
  Result<std::unique_ptr<DB>> db = OpenDB(Options{}, "db", test.fs);
  EXPECT_TRUE(db.ok()) << db.status().ToString();
  test.db = std::move(db).value();
  return test;
}

WriteOptions Sync(bool sync) {
  WriteOptions options;
  options.sync = sync;
  return options;
}

std::string Get(DB& db, std::string_view key) {
  Result<std::string> value = db.Get(ReadOptions{}, key);
  if (value.ok()) {
    return std::move(value).value();
  }
  return value.status().code() == Status::Code::kNotFound ? "NOT_FOUND"
                                                          : "ERROR: " + value.status().ToString();
}

// One log record, decoded as a write batch.
struct LoggedBatch {
  SequenceNumber sequence = 0;
  std::uint32_t count = 0;
  std::string operations;  // "Put(k, v) Delete(k) "
  std::size_t bytes = 0;
};

class Recorder final : public WriteBatch::Handler {
 public:
  void Put(std::string_view key, std::string_view value) override {
    text.append("Put(").append(key).append(", ").append(value).append(") ");
  }
  void Delete(std::string_view key) override { text.append("Delete(").append(key).append(") "); }
  std::string text;  // NOLINT(misc-non-private-member-variables-in-classes)
};

// Every record in the database's log. The DB must not be writing.
std::vector<LoggedBatch> ReadLog(FileSystem& fs) {
  log::Reader reader(std::move(fs.NewSequentialFile(LogFileName("db", 1))).value());
  std::vector<LoggedBatch> batches;
  while (true) {
    const Result<std::optional<std::string_view>> record = reader.ReadRecord();
    EXPECT_TRUE(record.ok()) << record.status().ToString();
    if (!record.ok() || !record->has_value()) {
      return batches;
    }
    WriteBatch batch;
    EXPECT_TRUE(WriteBatchInternal::SetContents(batch, **record).ok());
    Recorder recorder;
    EXPECT_TRUE(batch.Iterate(recorder).ok());
    batches.push_back({.sequence = WriteBatchInternal::Sequence(batch),
                       .count = batch.Count(),
                       .operations = std::move(recorder.text),
                       .bytes = (*record)->size()});
  }
}

// --- The log ------------------------------------------------------------------------

TEST(DBWriteTest, EveryWriteIsLoggedWithItsSequenceNumbers) {
  const TestDB test = OpenTestDB();
  ASSERT_TRUE(test.db->Put({}, "a", "1").ok());
  WriteBatch batch;
  batch.Put("b", "2");
  batch.Delete("a");
  ASSERT_TRUE(test.db->Write({}, batch).ok());
  ASSERT_TRUE(test.db->Delete({}, "b").ok());

  const std::vector<LoggedBatch> log = ReadLog(*test.fs);
  ASSERT_EQ(log.size(), 3U);
  EXPECT_EQ(log[0].sequence, 1U);
  EXPECT_EQ(log[0].operations, "Put(a, 1) ");
  EXPECT_EQ(log[1].sequence, 2U);
  EXPECT_EQ(log[1].operations, "Put(b, 2) Delete(a) ");
  EXPECT_EQ(log[2].sequence, 4U);
  EXPECT_EQ(log[2].operations, "Delete(b) ");
  EXPECT_EQ(test.Impl().LastSequenceForTesting(), 4U);
}

TEST(DBWriteTest, SyncOnlyWhenAsked) {
  const TestDB test = OpenTestDB();
  const Controls& controls = test.fs->Knobs();
  ASSERT_TRUE(test.db->Put(Sync(false), "a", "1").ok());
  EXPECT_EQ(controls.syncs.load(), 0);
  ASSERT_TRUE(test.db->Put(Sync(true), "b", "2").ok());
  EXPECT_EQ(controls.syncs.load(), 1);
  // An empty batch with sync=true syncs earlier unsynced writes (and is
  // logged as an empty record; no sequence number is used).
  ASSERT_TRUE(test.db->Put(Sync(false), "c", "3").ok());
  ASSERT_TRUE(test.db->Write(Sync(true), WriteBatch{}).ok());
  EXPECT_EQ(controls.syncs.load(), 2);
  EXPECT_EQ(test.Impl().LastSequenceForTesting(), 3U);
}

// --- Group commit -------------------------------------------------------------------
//
// Eight threads write concurrently; Sync is slowed to 1 ms so writers queue up
// behind each leader. Then: every acknowledged write is visible, sequence
// numbers are unique and dense, fewer log records than writes were needed
// (groups formed), and the log replays to exactly the same writes.

TEST(DBWriteTest, ConcurrentWritersAreGroupedAndNothingIsLost) {
  constexpr int kThreads = 8;
  constexpr int kWritesPerThread = 300;
  const TestDB test = OpenTestDB();
  test.fs->Knobs().sync_delay_ms = 1;

  std::latch start(kThreads);
  std::vector<std::thread> threads;
  threads.reserve(kThreads);
  std::atomic<int> failures{0};
  for (int t = 0; t < kThreads; ++t) {
    threads.emplace_back([&, t] {
      start.arrive_and_wait();
      for (int i = 0; i < kWritesPerThread; ++i) {
        const std::string key = "t" + std::to_string(t) + "-" + std::to_string(i);
        if (!test.db->Put(Sync(i % 4 == 0), key, "v" + key).ok()) {
          failures.fetch_add(1);
        }
      }
    });
  }
  for (std::thread& thread : threads) {
    thread.join();
  }
  ASSERT_EQ(failures.load(), 0);

  constexpr std::uint64_t kWrites = std::uint64_t{kThreads} * kWritesPerThread;
  for (int t = 0; t < kThreads; ++t) {
    for (int i = 0; i < kWritesPerThread; ++i) {
      const std::string key = "t" + std::to_string(t) + "-" + std::to_string(i);
      ASSERT_EQ(Get(*test.db, key), "v" + key);
    }
  }
  EXPECT_EQ(test.Impl().LastSequenceForTesting(), kWrites) << "one sequence number per write";
  const std::uint64_t groups = test.Impl().GroupsWrittenForTesting();
  EXPECT_LT(groups, kWrites) << "writers that queued up behind a leader shared its log record";

  // The log: one record per group, sequence numbers dense from 1, every
  // write present exactly once, and no group over the size cap.
  const std::vector<LoggedBatch> log = ReadLog(*test.fs);
  EXPECT_EQ(log.size(), groups);
  SequenceNumber expected = 1;
  std::uint64_t operations = 0;
  for (const LoggedBatch& batch : log) {
    ASSERT_EQ(batch.sequence, expected) << "sequence numbers are dense";
    expected += batch.count;
    operations += batch.count;
    EXPECT_LE(batch.bytes, std::size_t{1} << 20U);
  }
  EXPECT_EQ(operations, kWrites);
}

// A writer that asks for sync must get it even inside a group led by a writer
// that didn't. Set up with timing: A (sync) holds the leadership inside a slow
// Sync; meanwhile B (no sync) and then C (sync) queue up, so the next group
// is {B, C}, led by B. That group must still be synced for C.
TEST(DBWriteTest, SyncRequestedByAFollowerIsHonoured) {
  const TestDB test = OpenTestDB();
  Controls& controls = test.fs->Knobs();
  controls.sync_delay_ms = 300;

  std::thread a([&] { EXPECT_TRUE(test.db->Put(Sync(true), "a", "1").ok()); });
  std::this_thread::sleep_for(std::chrono::milliseconds(50));  // A is now inside Sync
  std::thread b([&] { EXPECT_TRUE(test.db->Put(Sync(false), "b", "2").ok()); });
  std::this_thread::sleep_for(std::chrono::milliseconds(50));  // B is queued first
  std::thread c([&] { EXPECT_TRUE(test.db->Put(Sync(true), "c", "3").ok()); });
  a.join();
  b.join();
  c.join();

  ASSERT_EQ(test.Impl().GroupsWrittenForTesting(), 2U)
      << "timing: B and C should have been written as one group";
  EXPECT_EQ(controls.syncs.load(), 2) << "the {B, C} group is synced because C asked";
}

// --- Sticky errors ---------------------------------------------------------------------

TEST(DBWriteTest, FailedSyncRefusesAllLaterWritesButReadsWork) {
  const TestDB test = OpenTestDB();
  ASSERT_TRUE(test.db->Put(Sync(true), "before", "1").ok());

  test.fs->Knobs().fail_syncs = true;
  const Status failed = test.db->Put(Sync(true), "during", "2");
  EXPECT_EQ(failed.code(), Status::Code::kIOError) << failed.ToString();
  test.fs->Knobs().fail_syncs = false;  // even if the disk recovers...

  EXPECT_EQ(test.db->Put(Sync(false), "after", "3"), failed) << "...the error is sticky";
  EXPECT_EQ(test.db->Write(Sync(true), WriteBatch{}), failed);
  EXPECT_EQ(Get(*test.db, "before"), "1") << "reads still work";
  EXPECT_EQ(Get(*test.db, "during"), "NOT_FOUND") << "the failed write was not applied";
  EXPECT_EQ(Get(*test.db, "after"), "NOT_FOUND");
  EXPECT_EQ(test.Impl().LastSequenceForTesting(), 1U);
}

TEST(DBWriteTest, FailedAppendRefusesAllLaterWrites) {
  const TestDB test = OpenTestDB();
  ASSERT_TRUE(test.db->Put({}, "before", "1").ok());
  test.fs->Knobs().fail_appends = true;
  const Status failed = test.db->Put({}, "during", "2");
  EXPECT_EQ(failed.code(), Status::Code::kIOError);
  test.fs->Knobs().fail_appends = false;
  EXPECT_EQ(test.db->Put({}, "after", "3"), failed);
  EXPECT_EQ(Get(*test.db, "before"), "1");
  EXPECT_EQ(Get(*test.db, "during"), "NOT_FOUND");
}

// Writers racing a sync failure: each gets either success or the error, none
// hangs, and exactly the acknowledged writes are in the DB.
TEST(DBWriteTest, FailureDuringConcurrentWritesFailsWholeGroupsAndNoOneHangs) {
  constexpr int kThreads = 8;
  constexpr int kWritesPerThread = 100;
  const TestDB test = OpenTestDB();
  test.fs->Knobs().sync_delay_ms = 1;

  std::latch start(kThreads + 1);
  std::vector<std::vector<bool>> acknowledged(kThreads, std::vector<bool>(kWritesPerThread));
  std::vector<std::thread> threads;
  threads.reserve(kThreads);
  for (int t = 0; t < kThreads; ++t) {
    threads.emplace_back([&, t] {
      start.arrive_and_wait();
      for (int i = 0; i < kWritesPerThread; ++i) {
        acknowledged[static_cast<std::size_t>(t)][static_cast<std::size_t>(i)] =
            test.db->Put(Sync(true), "t" + std::to_string(t) + "-" + std::to_string(i), "v").ok();
      }
    });
  }
  start.arrive_and_wait();
  // Inject the failure early (after 50 of 800 writes), so every thread is
  // still writing when it hits.
  while (test.Impl().LastSequenceForTesting() < 50) {
    std::this_thread::yield();
  }
  test.fs->Knobs().fail_syncs = true;
  for (std::thread& thread : threads) {
    thread.join();  // a deadlock would hang here (and time out in ctest)
  }

  std::uint64_t acknowledged_count = 0;
  for (int t = 0; t < kThreads; ++t) {
    bool failed_once = false;
    for (int i = 0; i < kWritesPerThread; ++i) {
      const bool ok = acknowledged[static_cast<std::size_t>(t)][static_cast<std::size_t>(i)];
      const std::string key = "t" + std::to_string(t) + "-" + std::to_string(i);
      EXPECT_FALSE(failed_once && ok) << key << ": a write succeeded after an earlier one failed";
      failed_once = failed_once || !ok;
      EXPECT_EQ(Get(*test.db, key), ok ? "v" : "NOT_FOUND") << key;
      acknowledged_count += ok ? 1U : 0U;
    }
    EXPECT_TRUE(failed_once) << "thread " << t << " should have hit the injected failure";
  }
  EXPECT_EQ(test.Impl().LastSequenceForTesting(), acknowledged_count);
}

}  // namespace
}  // namespace forgedb
