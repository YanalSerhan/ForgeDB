// Opening an existing database: LOCK, and recovery from the write-ahead logs
// (TODO 2.3, 3.6). Everything runs on an in-memory file system that outlives
// the DB objects, so "close and reopen" keeps the files, like a process
// restart.
//
// File numbers in a new database: the first open writes log 1 and MANIFEST 2.
// Each later open replays the logs, writes what it recovered to a table, then
// a new log and a new MANIFEST, each with the next file number, and deletes
// the files it replaced.
#include <cstddef>
#include <cstdint>
#include <filesystem>

#include <gtest/gtest.h>

#include <memory>
#include <optional>
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

  static const RecoveryInfo& Info(DB& db) { return dynamic_cast<DBImpl&>(db).GetRecoveryInfo(); }
  static SequenceNumber LastSequence(DB& db) { return dynamic_cast<DBImpl&>(db).LastSequence(); }

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
  EXPECT_EQ(Info(*db).manifest.manifest_used, 2U);
  EXPECT_EQ(Info(*db).table_written, 3U);
  EXPECT_EQ(Info(*db).new_log_number, 4U);
  EXPECT_EQ(Info(*db).new_manifest_number, 5U);
  EXPECT_EQ(Info(*db).files_deleted, (std::vector<std::string>({"000001.log", "MANIFEST-000002"})));
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
  // Only the last session's log is left; the others are in tables 3, 6, 9.
  EXPECT_EQ(Info(*db).logs_replayed, std::vector<std::uint64_t>({7}));
  EXPECT_EQ(dynamic_cast<DBImpl&>(*db).CurrentVersion()->files[0].size(), 3U);
  EXPECT_EQ(LastSequence(*db), 6U);
}

TEST_F(RecoveryTest, AnEmptyLogIsFine) {
  {
    const std::unique_ptr<DB> db = OpenOrDie();
  }  // opened and closed, nothing written
  const std::unique_ptr<DB> db = OpenOrDie();
  EXPECT_EQ(Scan(*db), "");
  EXPECT_EQ(Info(*db).logs_replayed, std::vector<std::uint64_t>({1}));
  EXPECT_EQ(Info(*db).table_written, std::nullopt) << "nothing to write";
  EXPECT_EQ(Info(*db).new_log_number, 3U);
}

// A directory with logs and no MANIFEST (a Phase 2 database) replays them all.
TEST_F(RecoveryTest, GapsInLogNumbersAreFine) {
  WriteLogOfBatches(4, {{1, PutBatch("a", "1")}});
  WriteLogOfBatches(9, {{2, PutBatch("b", "2")}});
  const std::unique_ptr<DB> db = OpenOrDie();
  EXPECT_EQ(Scan(*db), "a=1 b=2 ");
  EXPECT_EQ(Info(*db).manifest.manifest_used, std::nullopt);
  EXPECT_EQ(Info(*db).logs_replayed, std::vector<std::uint64_t>({4, 9}));
  EXPECT_EQ(Info(*db).table_written, 10U) << "numbers continue above the highest file";
  EXPECT_EQ(Info(*db).new_log_number, 11U);
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

TEST_F(RecoveryTest, TornTailIsIgnoredAndItsLogRetired) {
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
    EXPECT_FALSE(Info(*db).torn_tails[0].valid_data_dropped);
    EXPECT_EQ(LastSequence(*db), 1U);
    ASSERT_TRUE(db->Put({}, "c", "3").ok());  // continues at sequence 2, in log 4
  }
  // Log 1's recovered prefix went into table 3 and log 1 was deleted, so the
  // torn tail is never seen again.
  EXPECT_FALSE(*fs_->FileExists(LogFileName(db_, 1)));
  const std::unique_ptr<DB> db = OpenOrDie();
  EXPECT_EQ(Scan(*db), "a=1 c=3 ");
  EXPECT_EQ(Info(*db).logs_replayed, std::vector<std::uint64_t>({4}));
  EXPECT_TRUE(Info(*db).torn_tails.empty());
  EXPECT_EQ(LastSequence(*db), 2U);
}

// The newest log stops at its first problem, even with valid data after it
// (point-in-time recovery, ADR 0004 rule 6); the dropped part is reported.
TEST_F(RecoveryTest, DamageInTheNewestLogIsPointInTime) {
  {
    const std::unique_ptr<DB> db = OpenOrDie();
    ASSERT_TRUE(db->Put({}, "a", "1").ok());
    ASSERT_TRUE(db->Put({}, "b", "2").ok());
    ASSERT_TRUE(db->Put({}, "c", "3").ok());
  }
  std::string log = FileContents(LogFileName(db_, 1));
  // Each record is 24 bytes: a 7-byte header and a 17-byte batch (12-byte
  // header, then 0x01, key length, key, value length, value). Damage the second.
  log[24 + 10] = static_cast<char>(log[24 + 10] ^ 0x01);
  WriteFile(LogFileName(db_, 1), log);

  const std::unique_ptr<DB> db = OpenOrDie();
  EXPECT_EQ(Scan(*db), "a=1 ");
  ASSERT_EQ(Info(*db).torn_tails.size(), 1U);
  EXPECT_EQ(Info(*db).torn_tails[0].offset, 24U);
  EXPECT_TRUE(Info(*db).torn_tails[0].valid_data_dropped);
  EXPECT_EQ(LastSequence(*db), 1U);
}

// Older logs are synced before a newer one starts, so damage there is
// corruption (ADR 0004).
TEST_F(RecoveryTest, MidLogCorruptionInAnOlderLogFailsTheOpen) {
  WriteLogOfBatches(1, {{1, PutBatch("a", "1")}, {2, PutBatch("b", "2")}});
  WriteLogOfBatches(2, {{3, PutBatch("c", "3")}});
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

// Without a MANIFEST every log is replayed, so the oldest must start at sequence 1.
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
// Since 3.6 the open writes the recovered data to a synced table.
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

// The same with renames and removals that may not survive (the default): a
// power loss can undo the CURRENT switch and the deletion of the old files,
// and recovery still uses the newest complete MANIFEST (ADR 0007).
TEST(RecoveryDurabilityTest, UndoneCurrentSwitchIsHarmless) {
  for (std::uint64_t seed = 1; seed <= 20; ++seed) {
    SCOPED_TRACE(seed);
    FaultInjectionFileSystem::Options options;
    options.seed = seed;
    const auto fs =
        std::make_shared<FaultInjectionFileSystem>(std::make_shared<MemFileSystem>(), options);
    for (int session = 0; session < 3; ++session) {
      const std::unique_ptr<DB> db = std::move(OpenDB(Options{}, "db", fs)).value();
      ASSERT_TRUE(db->Put({.sync = true}, "k" + std::to_string(session), "v").ok());
    }
    ASSERT_TRUE(fs->SimulateCrash(FaultInjectionFileSystem::CrashKind::kPowerLoss).ok());
    const Result<std::unique_ptr<DB>> db = OpenDB(Options{}, "db", fs);
    ASSERT_TRUE(db.ok()) << db.status().ToString();
    EXPECT_EQ(Get(**db, "k0"), "v");
    EXPECT_EQ(Get(**db, "k1"), "v");
    EXPECT_EQ(Get(**db, "k2"), "v");
  }
}

TEST(RecoveryDurabilityTest, ASyncFailureFailsTheOpen) {
  const auto fs = std::make_shared<FaultInjectionFileSystem>(std::make_shared<MemFileSystem>());
  {
    const std::unique_ptr<DB> db = std::move(OpenDB(Options{}, "db", fs)).value();
    ASSERT_TRUE(db->Put({}, "k", "v").ok());
  }
  fs->FailNth(FaultInjectionFileSystem::Op::kSync, 1);  // the recovered table's sync
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
