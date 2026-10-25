// Opening an existing database: LOCK, and recovery from the write-ahead logs
// (TODO 2.3). Everything runs on an in-memory file system that outlives the
// DB objects, so "close and reopen" keeps the files, like a process restart.
#include <cstddef>
#include <cstdint>
#include <filesystem>

#include <gtest/gtest.h>

#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "forgedb/db.h"
#include "forgedb/iterator.h"
#include "forgedb/status.h"
#include "forgedb/write_batch.h"

#include "db/db_impl.h"
#include "db/filename.h"
#include "db/write_batch_internal.h"
#include "env/file_system.h"
#include "env/mem_file_system.h"
#include "log/log_writer.h"
#include "support/fault_injection_file_system.h"

namespace forgedb {
namespace {

class RecoveryTest : public testing::Test {
 protected:
  // A path, not a string, so tests can write db_ / "name". (A fixture member:
  // a static std::filesystem::path could throw during static initialization.)
  const std::filesystem::path db_{"db"};

  Result<std::unique_ptr<DB>> Open() { return OpenDB(Options{}, db_, fs_); }

  std::unique_ptr<DB> OpenOrDie() {
    Result<std::unique_ptr<DB>> db = Open();
    EXPECT_TRUE(db.ok()) << db.status().ToString();
    return db.ok() ? std::move(db).value() : nullptr;
  }

  static const RecoveryInfo& Info(DB& db) {
    return dynamic_cast<DBImpl&>(db).RecoveryInfoForTesting();
  }
  static SequenceNumber LastSequence(DB& db) {
    return dynamic_cast<DBImpl&>(db).LastSequenceForTesting();
  }

  std::string FileContents(const std::filesystem::path& path) {
    const std::unique_ptr<SequentialFile> file = std::move(fs_->NewSequentialFile(path)).value();
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

  void WriteFile(const std::filesystem::path& path, std::string_view contents) {
    const std::unique_ptr<WritableFile> file = std::move(fs_->NewWritableFile(path)).value();
    ASSERT_TRUE(file->Append(contents).ok());
    ASSERT_TRUE(file->Close().ok());
  }

  // Writes a log whose records are the given batches, numbered as given.
  void WriteLogOfBatches(std::uint64_t number,
                         const std::vector<std::pair<SequenceNumber, WriteBatch>>& batches) {
    ASSERT_TRUE(fs_->CreateDir(db_).ok());
    const std::unique_ptr<WritableFile> file =
        std::move(fs_->NewWritableFile(LogFileName(db_, number))).value();
    log::Writer writer(file.get());
    for (const auto& [sequence, original] : batches) {
      WriteBatch batch = original;
      WriteBatchInternal::SetSequence(batch, sequence);
      ASSERT_TRUE(writer.AddRecord(WriteBatchInternal::Contents(batch)).ok());
    }
    ASSERT_TRUE(file->Close().ok());
  }

  std::shared_ptr<MemFileSystem> fs_ = std::make_shared<MemFileSystem>();
};

std::string Get(DB& db, std::string_view key) {
  Result<std::string> value = db.Get(ReadOptions{}, key);
  if (value.ok()) {
    return std::move(value).value();
  }
  return value.status().code() == Status::Code::kNotFound ? "NOT_FOUND"
                                                          : "ERROR: " + value.status().ToString();
}

std::string Scan(DB& db) {
  const std::unique_ptr<Iterator> it = db.NewIterator(ReadOptions{});
  std::string result;
  for (it->SeekToFirst(); it->Valid(); it->Next()) {
    result.append(it->key()).append("=").append(it->value()).append(" ");
  }
  return result;
}

WriteBatch PutBatch(std::string_view key, std::string_view value) {
  WriteBatch batch;
  batch.Put(key, value);
  return batch;
}

// --- Reopening -----------------------------------------------------------------------

TEST_F(RecoveryTest, ReopenSeesEveryWrite) {
  {
    const std::unique_ptr<DB> db = OpenOrDie();
    ASSERT_TRUE(db->Put({}, "a", "1").ok());
    ASSERT_TRUE(db->Put({}, "b", "2").ok());
    ASSERT_TRUE(db->Delete({}, "a").ok());
    WriteBatch batch;
    batch.Put("c", "3");
    batch.Put("b", "22");
    ASSERT_TRUE(db->Write({}, batch).ok());
  }
  const std::unique_ptr<DB> db = OpenOrDie();
  EXPECT_EQ(Scan(*db), "b=22 c=3 ");
  EXPECT_EQ(Get(*db, "a"), "NOT_FOUND");
  EXPECT_EQ(LastSequence(*db), 5U);
  EXPECT_EQ(Info(*db).logs_replayed, std::vector<std::uint64_t>({1}));
  EXPECT_EQ(Info(*db).records_replayed, 4U);
  EXPECT_TRUE(Info(*db).torn_tails.empty());
  EXPECT_EQ(Info(*db).new_log_number, 2U);
}

TEST_F(RecoveryTest, SeveralSessionsReplayInOrderAndSequencesContinue) {
  for (int session = 1; session <= 3; ++session) {
    const std::unique_ptr<DB> db = OpenOrDie();
    EXPECT_EQ(LastSequence(*db), static_cast<SequenceNumber>(2 * (session - 1)));
    ASSERT_TRUE(db->Put({}, "key", "session " + std::to_string(session)).ok());
    ASSERT_TRUE(db->Put({}, "s" + std::to_string(session), "x").ok());
  }
  const std::unique_ptr<DB> db = OpenOrDie();
  EXPECT_EQ(Get(*db, "key"), "session 3") << "later logs win";
  EXPECT_EQ(Scan(*db), "key=session 3 s1=x s2=x s3=x ");
  EXPECT_EQ(Info(*db).logs_replayed, std::vector<std::uint64_t>({1, 2, 3}));
  EXPECT_EQ(LastSequence(*db), 6U);
}

TEST_F(RecoveryTest, AnEmptyLogIsFine) {
  { const std::unique_ptr<DB> db = OpenOrDie(); }  // opened and closed, nothing written
  const std::unique_ptr<DB> db = OpenOrDie();
  EXPECT_EQ(Scan(*db), "");
  EXPECT_EQ(Info(*db).logs_replayed, std::vector<std::uint64_t>({1}));
  EXPECT_EQ(Info(*db).new_log_number, 2U);
}

TEST_F(RecoveryTest, GapsInLogNumbersAreFine) {
  WriteLogOfBatches(4, {{1, PutBatch("a", "1")}});
  WriteLogOfBatches(9, {{2, PutBatch("b", "2")}});
  const std::unique_ptr<DB> db = OpenOrDie();
  EXPECT_EQ(Scan(*db), "a=1 b=2 ");
  EXPECT_EQ(Info(*db).logs_replayed, std::vector<std::uint64_t>({4, 9}));
  EXPECT_EQ(Info(*db).new_log_number, 10U);
}

TEST_F(RecoveryTest, OtherFilesInTheDirectoryAreLeftAlone) {
  ASSERT_TRUE(fs_->CreateDir(db_).ok());
  WriteFile(db_ / "notes.txt", "hello");
  WriteFile(db_ / "5.log", "not a log name ForgeDB writes");  // non-canonical: ignored
  {
    const std::unique_ptr<DB> db = OpenOrDie();
    ASSERT_TRUE(db->Put({}, "k", "v").ok());
  }
  const std::unique_ptr<DB> db = OpenOrDie();
  EXPECT_EQ(Scan(*db), "k=v ");
  EXPECT_EQ(FileContents(db_ / "notes.txt"), "hello");
  EXPECT_EQ(FileContents(db_ / "5.log"), "not a log name ForgeDB writes");
}

// --- Crash leftovers and damage ------------------------------------------------------

TEST_F(RecoveryTest, TornTailIsIgnoredNowAndInLaterRecoveries) {
  {
    const std::unique_ptr<DB> db = OpenOrDie();
    ASSERT_TRUE(db->Put({}, "a", "1").ok());
    ASSERT_TRUE(db->Put({}, "b", "2").ok());
  }
  // A crash cut the last record short.
  const std::string log = FileContents(LogFileName(db_, 1));
  WriteFile(LogFileName(db_, 1), std::string_view(log).substr(0, log.size() - 3));
  {
    const std::unique_ptr<DB> db = OpenOrDie();
    EXPECT_EQ(Scan(*db), "a=1 ");
    ASSERT_EQ(Info(*db).torn_tails.size(), 1U);
    EXPECT_EQ(Info(*db).torn_tails[0].log_number, 1U);
    EXPECT_EQ(LastSequence(*db), 1U);
    ASSERT_TRUE(db->Put({}, "c", "3").ok());  // continues at sequence 2, in log 2
  }
  // The torn tail is still in log 1; it is ignored again, and log 2 continues
  // the sequence exactly.
  const std::unique_ptr<DB> db = OpenOrDie();
  EXPECT_EQ(Scan(*db), "a=1 c=3 ");
  EXPECT_EQ(Info(*db).logs_replayed, std::vector<std::uint64_t>({1, 2}));
  EXPECT_EQ(Info(*db).torn_tails.size(), 1U);
  EXPECT_EQ(LastSequence(*db), 2U);
}

TEST_F(RecoveryTest, MidLogCorruptionFailsTheOpen) {
  {
    const std::unique_ptr<DB> db = OpenOrDie();
    ASSERT_TRUE(db->Put({}, "a", "1").ok());
    ASSERT_TRUE(db->Put({}, "b", "2").ok());
  }
  std::string log = FileContents(LogFileName(db_, 1));
  log[10] = static_cast<char>(log[10] ^ 0x01);  // inside the first record; the second is intact
  WriteFile(LogFileName(db_, 1), log);

  const Result<std::unique_ptr<DB>> db = Open();
  EXPECT_EQ(db.status().code(), Status::Code::kCorruption) << db.status().ToString();
  EXPECT_NE(db.status().message().find("000001.log"), std::string::npos) << db.status().message();
  // A failed open releases LOCK: trying again fails the same way, not with Busy.
  EXPECT_EQ(Open().status().code(), Status::Code::kCorruption);
}

TEST_F(RecoveryTest, SequenceGapIsCorruption) {
  WriteLogOfBatches(1, {{1, PutBatch("a", "1")}, {5, PutBatch("b", "2")}});
  const Result<std::unique_ptr<DB>> db = Open();
  EXPECT_EQ(db.status().code(), Status::Code::kCorruption);
  EXPECT_NE(db.status().message().find("not contiguous"), std::string::npos)
      << db.status().message();
}

TEST_F(RecoveryTest, SequenceGapAcrossLogsIsCorruption) {
  WriteLogOfBatches(1, {{1, PutBatch("a", "1")}});
  WriteLogOfBatches(2, {{3, PutBatch("b", "2")}});  // sequence 2 is missing
  EXPECT_EQ(Open().status().code(), Status::Code::kCorruption);
}

// Phase 2 keeps every log, so the oldest one must start at sequence 1.
TEST_F(RecoveryTest, AFirstRecordAfterSequenceOneIsCorruption) {
  WriteLogOfBatches(1, {{7, PutBatch("a", "1")}});
  EXPECT_EQ(Open().status().code(), Status::Code::kCorruption);
}

TEST_F(RecoveryTest, MalformedBatchInAValidRecordIsCorruption) {
  ASSERT_TRUE(fs_->CreateDir(db_).ok());
  {
    const std::unique_ptr<WritableFile> file =
        std::move(fs_->NewWritableFile(LogFileName(db_, 1))).value();
    log::Writer writer(file.get());
    ASSERT_TRUE(writer.AddRecord("short").ok());  // 5 bytes: no room for a batch header
    ASSERT_TRUE(file->Close().ok());
  }
  EXPECT_EQ(Open().status().code(), Status::Code::kCorruption);
}

TEST_F(RecoveryTest, EmptyBatchRecordsDoNotUseSequenceNumbers) {
  WriteLogOfBatches(1, {{1, WriteBatch{}}, {1, PutBatch("a", "1")}, {2, WriteBatch{}}});
  const std::unique_ptr<DB> db = OpenOrDie();
  EXPECT_EQ(Scan(*db), "a=1 ");
  EXPECT_EQ(LastSequence(*db), 1U);
}

// --- Durability of recovered data ---------------------------------------------------

// Found by the crash torture test (2.5): data recovered from a log that was
// never synced must be made durable before newer writes can depend on it.
// Otherwise a power loss can drop the old log's tail while the new log's
// synced writes survive, and the next open sees a gap in sequence numbers.
TEST(RecoveryDurabilityTest, RecoveredLogsAreSyncedBeforeNewWrites) {
  for (std::uint64_t seed = 1; seed <= 20; ++seed) {
    SCOPED_TRACE(seed);
    FaultInjectionFileSystem::Options options;
    options.seed = seed;
    options.unsynced_tail = FaultInjectionFileSystem::UnsyncedTail::kDrop;
    const auto fs =
        std::make_shared<FaultInjectionFileSystem>(std::make_shared<MemFileSystem>(), options);
    {
      const std::unique_ptr<DB> db = std::move(OpenDB(Options{}, "db", fs)).value();
      ASSERT_TRUE(db->Put({}, "old", "unsynced").ok());
    }  // closed cleanly: flushed, not synced
    {
      const std::unique_ptr<DB> db = std::move(OpenDB(Options{}, "db", fs)).value();
      ASSERT_TRUE(db->Put({.sync = true}, "new", "synced").ok());
    }
    ASSERT_TRUE(fs->SimulateCrash(FaultInjectionFileSystem::CrashKind::kPowerLoss).ok());
    const Result<std::unique_ptr<DB>> db = OpenDB(Options{}, "db", fs);
    ASSERT_TRUE(db.ok()) << db.status().ToString();
    EXPECT_EQ(Get(**db, "old"), "unsynced") << "recovered, so made durable by the second open";
    EXPECT_EQ(Get(**db, "new"), "synced");
  }
}

TEST(RecoveryDurabilityTest, ASyncFailureFailsTheOpen) {
  const auto fs = std::make_shared<FaultInjectionFileSystem>(std::make_shared<MemFileSystem>());
  {
    const std::unique_ptr<DB> db = std::move(OpenDB(Options{}, "db", fs)).value();
    ASSERT_TRUE(db->Put({}, "k", "v").ok());
  }
  fs->FailNth(FaultInjectionFileSystem::Op::kSyncFile, 1);
  EXPECT_EQ(OpenDB(Options{}, "db", fs).status().code(), Status::Code::kIOError);
  const Result<std::unique_ptr<DB>> db = OpenDB(Options{}, "db", fs);
  ASSERT_TRUE(db.ok()) << "the failed open released LOCK: " << db.status().ToString();
  EXPECT_EQ(Get(**db, "k"), "v");
}

// --- LOCK -----------------------------------------------------------------------------

TEST_F(RecoveryTest, ASecondOpenIsBusyUntilTheFirstCloses) {
  std::unique_ptr<DB> first = OpenOrDie();
  const Result<std::unique_ptr<DB>> second = Open();
  EXPECT_EQ(second.status().code(), Status::Code::kBusy) << second.status().ToString();
  EXPECT_NE(second.status().message().find("database is already open"), std::string::npos)
      << second.status().ToString();
  first.reset();
  EXPECT_TRUE(Open().ok());
}

}  // namespace
}  // namespace forgedb
