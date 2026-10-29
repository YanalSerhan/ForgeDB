// Snapshots (TODO 4.1): reads at an earlier point in time.
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <filesystem>

#include <gtest/gtest.h>

#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include "forgedb/db.h"
#include "forgedb/iterator.h"
#include "forgedb/options.h"
#include "forgedb/snapshot.h"
#include "forgedb/status.h"
#include "forgedb/write_batch.h"

#include "db/db_impl.h"
#include "env/mem_file_system.h"

namespace forgedb {
namespace {

std::string Key(int i) {
  const std::string digits = std::to_string(i);
  return "key" + std::string(digits.size() < 6 ? 6 - digits.size() : 0, '0') + digits;
}

std::string Value(int i, int version) {
  return "value-" + std::to_string(i) + "-" + std::to_string(version) + std::string(150, 'x');
}

DBImpl& Impl(DB& db) { return dynamic_cast<DBImpl&>(db); }

std::string Get(DB& db, std::string_view key, std::shared_ptr<const Snapshot> snapshot = nullptr) {
  Result<std::string> value = db.Get({.snapshot = std::move(snapshot)}, key);
  if (value.ok()) {
    return std::move(value).value();
  }
  return value.status().code() == Status::Code::kNotFound ? "NOT_FOUND"
                                                          : "ERROR: " + value.status().ToString();
}

// "key=value " for every entry, as of `snapshot`.
std::string Scan(DB& db, std::shared_ptr<const Snapshot> snapshot = nullptr) {
  const std::unique_ptr<Iterator> it = db.NewIterator({.snapshot = std::move(snapshot)});
  std::string out;
  for (it->SeekToFirst(); it->Valid(); it->Next()) {
    out.append(it->key()).append("=").append(it->value()).append(" ");
  }
  if (!it->status().ok()) {
    out += "ERROR: " + it->status().ToString();
  }
  return out;
}

class SnapshotTest : public testing::Test {
 protected:
  std::unique_ptr<DB> Open(std::size_t memtable_bytes = std::size_t{16} << 10) {
    Options options;
    options.memtable_bytes = memtable_bytes;
    Result<std::unique_ptr<DB>> db = OpenDB(options, "db", fs_);
    EXPECT_TRUE(db.ok()) << db.status().ToString();
    return db.ok() ? std::move(db).value() : nullptr;
  }

  std::shared_ptr<MemFileSystem> fs_ = std::make_shared<MemFileSystem>();
};

TEST_F(SnapshotTest, ASnapshotSeesAFrozenViewThroughWritesAndFlushes) {
  const std::unique_ptr<DB> db = Open();
  for (int i = 0; i < 300; ++i) {
    ASSERT_TRUE(db->Put({}, Key(i), Value(i, 0)).ok());
  }
  const std::shared_ptr<const Snapshot> snapshot = db->GetSnapshot();
  const std::string scan_before = Scan(*db);
  const std::uint64_t flushes_before = Impl(*db).FlushesForTesting();

  // Overwrite everything, delete a third, add new keys: many flushes.
  for (int round = 1; round <= 3; ++round) {
    for (int i = 0; i < 400; ++i) {
      if (i % 3 == 0 && i < 300) {
        ASSERT_TRUE(db->Delete({}, Key(i)).ok());
      } else {
        ASSERT_TRUE(db->Put({}, Key(i), Value(i, round)).ok());
      }
    }
  }
  Impl(*db).WaitForFlushForTesting();
  ASSERT_GT(Impl(*db).FlushesForTesting(), flushes_before + 5) << "the old versions are in tables";

  for (int i = 0; i < 400; ++i) {
    ASSERT_EQ(Get(*db, Key(i), snapshot), i < 300 ? Value(i, 0) : "NOT_FOUND") << i;
    ASSERT_EQ(Get(*db, Key(i)), i % 3 == 0 && i < 300 ? "NOT_FOUND" : Value(i, 3)) << i;
  }
  EXPECT_EQ(Scan(*db, snapshot), scan_before);

  const std::unique_ptr<Iterator> it = db->NewIterator({.snapshot = snapshot});
  it->Seek(Key(150));
  ASSERT_TRUE(it->Valid());
  EXPECT_EQ(it->key(), Key(150));
  EXPECT_EQ(it->value(), Value(150, 0));
  it->Seek(Key(299) + "~");
  EXPECT_FALSE(it->Valid()) << "keys written after the snapshot are invisible";
  EXPECT_TRUE(it->status().ok());
}

TEST_F(SnapshotTest, AnIteratorAtASnapshotKeepsItsViewWhileWritesContinue) {
  const std::unique_ptr<DB> db = Open();
  for (int i = 0; i < 100; ++i) {
    ASSERT_TRUE(db->Put({}, Key(i), Value(i, 0)).ok());
  }
  const std::shared_ptr<const Snapshot> snapshot = db->GetSnapshot();
  ASSERT_TRUE(db->Put({}, Key(0), Value(0, 1)).ok());  // after the snapshot, before the iterator
  const std::unique_ptr<Iterator> it = db->NewIterator({.snapshot = snapshot});
  for (int i = 0; i < 1000; ++i) {
    ASSERT_TRUE(db->Put({}, Key(i), Value(i, 2)).ok());
  }
  int count = 0;
  for (it->SeekToFirst(); it->Valid(); it->Next(), ++count) {
    ASSERT_EQ(it->value(), Value(count, 0)) << count;
  }
  EXPECT_EQ(count, 100);
  EXPECT_TRUE(it->status().ok());
}

TEST_F(SnapshotTest, ASnapshotOfAnEmptyDatabase) {
  const std::unique_ptr<DB> db = Open();
  const std::shared_ptr<const Snapshot> snapshot = db->GetSnapshot();
  for (int i = 0; i < 200; ++i) {
    ASSERT_TRUE(db->Put({}, Key(i), Value(i, 0)).ok());
  }
  EXPECT_EQ(Get(*db, Key(5), snapshot), "NOT_FOUND");
  EXPECT_EQ(Scan(*db, snapshot), "");
}

TEST_F(SnapshotTest, TheOldestLiveSnapshotIsTracked) {
  const std::unique_ptr<DB> db = Open();
  const DBImpl& impl = Impl(*db);
  EXPECT_EQ(impl.OldestSnapshotForTesting(), std::nullopt);
  ASSERT_TRUE(db->Put({}, "a", "1").ok());
  std::shared_ptr<const Snapshot> s1 = db->GetSnapshot();  // sequence 1
  ASSERT_TRUE(db->Put({}, "b", "2").ok());
  std::shared_ptr<const Snapshot> s2 = db->GetSnapshot();  // sequence 2
  std::shared_ptr<const Snapshot> s2_copy = s2;            // the same snapshot
  std::shared_ptr<const Snapshot> s3 = db->GetSnapshot();  // also sequence 2
  EXPECT_EQ(impl.OldestSnapshotForTesting(), 1U);
  s1.reset();
  EXPECT_EQ(impl.OldestSnapshotForTesting(), 2U);
  s2.reset();
  EXPECT_EQ(impl.OldestSnapshotForTesting(), 2U) << "a copy still holds it";
  s2_copy.reset();
  EXPECT_EQ(impl.OldestSnapshotForTesting(), 2U) << "s3 is at the same sequence";
  s3.reset();
  EXPECT_EQ(impl.OldestSnapshotForTesting(), std::nullopt);
}

TEST_F(SnapshotTest, ASnapshotMayOutliveItsDatabase) {
  std::shared_ptr<const Snapshot> snapshot;
  {
    const std::unique_ptr<DB> db = Open();
    ASSERT_TRUE(db->Put({}, "a", "1").ok());
    snapshot = db->GetSnapshot();
  }
  snapshot.reset();  // the DB is gone; this must still be safe (ASan checks)
}

TEST_F(SnapshotTest, AnotherDatabasesSnapshotIsRejected) {
  const std::unique_ptr<DB> db = Open();
  ASSERT_TRUE(db->Put({}, "a", "1").ok());
  const Result<std::unique_ptr<DB>> other = OpenDB(Options{}, "other", fs_);
  ASSERT_TRUE(other.ok());
  const std::shared_ptr<const Snapshot> foreign = (*other)->GetSnapshot();

  EXPECT_EQ(db->Get({.snapshot = foreign}, "a").status().code(), Status::Code::kInvalidArgument);
  const std::unique_ptr<Iterator> it = db->NewIterator({.snapshot = foreign});
  it->SeekToFirst();
  EXPECT_FALSE(it->Valid());
  EXPECT_EQ(it->status().code(), Status::Code::kInvalidArgument);

  // Nor can a caller make one.
  class Homemade final : public Snapshot {};
  const auto homemade = std::make_shared<const Homemade>();
  EXPECT_EQ(db->Get({.snapshot = homemade}, "a").status().code(), Status::Code::kInvalidArgument);
}

TEST_F(SnapshotTest, SnapshotsEndWhenTheDatabaseCloses) {
  std::shared_ptr<const Snapshot> old;
  {
    const std::unique_ptr<DB> db = Open();
    ASSERT_TRUE(db->Put({}, "a", "1").ok());
    old = db->GetSnapshot();
  }
  const std::unique_ptr<DB> db = Open();
  EXPECT_EQ(db->Get({.snapshot = old}, "a").status().code(), Status::Code::kInvalidArgument);
  EXPECT_EQ(Impl(*db).OldestSnapshotForTesting(), std::nullopt);
}

// Readers take snapshots while a writer updates two keys in one batch: at any
// snapshot, both keys hold the same number, read with two separate Gets.
TEST_F(SnapshotTest, ConcurrentSnapshotsSeeWholeBatches) {
  const std::unique_ptr<DB> db = Open(std::size_t{8} << 10);
  constexpr int kWrites = 3000;
  std::atomic<bool> done{false};
  std::atomic<int> wrong{0};
  std::atomic<int> reads{0};

  std::thread writer([&] {
    for (int i = 1; i <= kWrites; ++i) {
      WriteBatch batch;
      batch.Put("left", std::to_string(i));
      batch.Put("right", std::to_string(i));
      batch.Put(Key(i), std::string(100, 'p'));  // so memtables fill and flush
      if (!db->Write({}, batch).ok()) {
        wrong.fetch_add(1);
      }
    }
    done.store(true, std::memory_order_release);
  });
  std::vector<std::thread> readers;
  readers.reserve(4);
  for (int r = 0; r < 4; ++r) {
    readers.emplace_back([&] {
      int last = 0;
      while (!done.load(std::memory_order_acquire)) {
        const std::shared_ptr<const Snapshot> snapshot = db->GetSnapshot();
        const std::string left = Get(*db, "left", snapshot);
        std::this_thread::yield();
        const std::string right = Get(*db, "right", snapshot);
        if (left != right) {
          wrong.fetch_add(1);
        }
        if (left != "NOT_FOUND") {
          const int now = std::stoi(left);
          if (now < last) {
            wrong.fetch_add(1);  // a later snapshot saw an older write
          }
          last = now;
        }
        reads.fetch_add(1);
      }
    });
  }
  writer.join();
  for (std::thread& reader : readers) {
    reader.join();
  }
  EXPECT_EQ(wrong.load(), 0);
  EXPECT_GT(reads.load(), 0);
  EXPECT_EQ(Impl(*db).OldestSnapshotForTesting(), std::nullopt) << "every snapshot was released";
}

}  // namespace
}  // namespace forgedb
