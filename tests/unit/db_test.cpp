#include "forgedb/db.h"

#include <atomic>
#include <cstddef>
#include <cstdint>

#include <gtest/gtest.h>

#include <latch>
#include <memory>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include "forgedb/iterator.h"
#include "forgedb/options.h"
#include "forgedb/status.h"
#include "forgedb/write_batch.h"

#include "db/db_iter.h"
#include "db/internal_key.h"
#include "db/write_batch_internal.h"
#include "memtable/memtable.h"

namespace forgedb {
namespace {

std::unique_ptr<DB> OpenDB() {
  Result<std::unique_ptr<DB>> db = DB::Open(Options{}, "test-db");
  EXPECT_TRUE(db.ok()) << db.status().ToString();
  return std::move(db).value();
}

// Get as a string: the value, or "NOT_FOUND", or "ERROR: <status>".
std::string Get(DB& db, std::string_view key) {
  Result<std::string> value = db.Get(ReadOptions{}, key);
  if (value.ok()) {
    return std::move(value).value();
  }
  if (value.status().code() == Status::Code::kNotFound) {
    return "NOT_FOUND";
  }
  return "ERROR: " + value.status().ToString();
}

// Every key=value an iterator yields from SeekToFirst, e.g. "a=1 b=2 ".
std::string Scan(Iterator& it) {
  std::string result;
  for (it.SeekToFirst(); it.Valid(); it.Next()) {
    result.append(it.key()).append("=").append(it.value()).append(" ");
  }
  EXPECT_TRUE(it.status().ok()) << it.status().ToString();
  return result;
}

std::string Scan(DB& db) { return Scan(*db.NewIterator(ReadOptions{})); }

// --- Open -------------------------------------------------------------------------

TEST(DBTest, OpenRejectsAnEmptyName) {
  const Result<std::unique_ptr<DB>> db = DB::Open(Options{}, "");
  EXPECT_EQ(db.status().code(), Status::Code::kInvalidArgument);
}

TEST(DBTest, NewDatabaseIsEmpty) {
  const std::unique_ptr<DB> db = OpenDB();
  EXPECT_EQ(Get(*db, "anything"), "NOT_FOUND");
  EXPECT_EQ(Scan(*db), "");
}

// --- Put, Delete, Get ---------------------------------------------------------------

TEST(DBTest, PutGetOverwriteDelete) {
  const std::unique_ptr<DB> db = OpenDB();
  ASSERT_TRUE(db->Put({}, "k", "v1").ok());
  EXPECT_EQ(Get(*db, "k"), "v1");
  ASSERT_TRUE(db->Put({}, "k", "v2").ok());
  EXPECT_EQ(Get(*db, "k"), "v2");
  ASSERT_TRUE(db->Delete({}, "k").ok());
  EXPECT_EQ(Get(*db, "k"), "NOT_FOUND");
  ASSERT_TRUE(db->Put({}, "k", "v3").ok());
  EXPECT_EQ(Get(*db, "k"), "v3") << "a key can come back after a delete";
}

TEST(DBTest, DeletingAMissingKeyIsNotAnError) {
  const std::unique_ptr<DB> db = OpenDB();
  EXPECT_TRUE(db->Delete({}, "never-written").ok());
  EXPECT_EQ(Get(*db, "never-written"), "NOT_FOUND");
}

TEST(DBTest, EmptyAndBinaryKeysAndValues) {
  const std::unique_ptr<DB> db = OpenDB();
  const std::string binary("\x00\xff\x00", 3);
  ASSERT_TRUE(db->Put({}, "", "empty key").ok());
  ASSERT_TRUE(db->Put({}, "empty value", "").ok());
  ASSERT_TRUE(db->Put({}, binary, binary).ok());
  EXPECT_EQ(Get(*db, ""), "empty key");
  EXPECT_EQ(Get(*db, "empty value"), "");
  EXPECT_EQ(Get(*db, binary), binary);
  EXPECT_EQ(Get(*db, std::string("\x00", 1)), "NOT_FOUND") << "a prefix of a stored key";
}

TEST(DBTest, SizeLimits) {
  const std::unique_ptr<DB> db = OpenDB();
  const std::string max_key(kMaxKeySize, 'k');
  const std::string long_key(kMaxKeySize + 1, 'k');
  ASSERT_TRUE(db->Put({}, max_key, "ok").ok()) << "the limit itself is allowed";
  EXPECT_EQ(Get(*db, max_key), "ok");

  EXPECT_EQ(db->Put({}, long_key, "v").code(), Status::Code::kInvalidArgument);
  EXPECT_EQ(db->Delete({}, long_key).code(), Status::Code::kInvalidArgument);
  EXPECT_EQ(db->Get({}, long_key).status().code(), Status::Code::kInvalidArgument);
  WriteBatch batch;
  batch.Put("fine", "v");
  batch.Delete(long_key);
  EXPECT_EQ(db->Write({}, batch).code(), Status::Code::kInvalidArgument);
  EXPECT_EQ(Get(*db, "fine"), "NOT_FOUND") << "a rejected batch applies nothing";

  // Put checks sizes before copying anything, so this costs one allocation.
  const std::string long_value(kMaxValueSize + 1, 'v');
  EXPECT_EQ(db->Put({}, "k", long_value).code(), Status::Code::kInvalidArgument);
}

// --- Write ------------------------------------------------------------------------------

TEST(DBTest, WriteAppliesABatchInOrder) {
  const std::unique_ptr<DB> db = OpenDB();
  ASSERT_TRUE(db->Put({}, "gone", "x").ok());
  WriteBatch batch;
  batch.Put("a", "1");
  batch.Put("a", "2");  // later wins
  batch.Delete("gone");
  batch.Put("b", "3");
  batch.Delete("b");  // put then delete: deleted
  ASSERT_TRUE(db->Write({}, batch).ok());
  EXPECT_EQ(Scan(*db), "a=2 ");
}

TEST(DBTest, WriteLeavesTheCallersBatchUnchanged) {
  const std::unique_ptr<DB> db = OpenDB();
  WriteBatch batch;
  batch.Put("a", "1");
  const std::string before(WriteBatchInternal::Contents(batch));
  ASSERT_TRUE(db->Write({}, batch).ok());
  ASSERT_TRUE(db->Write({}, batch).ok());  // and it can be written again
  EXPECT_EQ(WriteBatchInternal::Contents(batch), before);
  EXPECT_EQ(Get(*db, "a"), "1");
}

TEST(DBTest, EmptyBatchIsANoOp) {
  const std::unique_ptr<DB> db = OpenDB();
  EXPECT_TRUE(db->Write({}, WriteBatch{}).ok());
  EXPECT_EQ(Scan(*db), "");
}

// --- Iterator -----------------------------------------------------------------------------

TEST(DBTest, IteratorShowsLatestValuesInOrderAndHidesDeletions) {
  const std::unique_ptr<DB> db = OpenDB();
  for (const char* key : {"d", "b", "a", "c", "e"}) {
    ASSERT_TRUE(db->Put({}, key, std::string("old-") + key).ok());
  }
  ASSERT_TRUE(db->Put({}, "b", "new-b").ok());
  ASSERT_TRUE(db->Delete({}, "c").ok());
  ASSERT_TRUE(db->Delete({}, "e").ok());  // the last key
  ASSERT_TRUE(db->Delete({}, "a").ok());  // the first key
  EXPECT_EQ(Scan(*db), "b=new-b d=old-d ");
}

TEST(DBTest, IteratorSeek) {
  const std::unique_ptr<DB> db = OpenDB();
  for (const char* key : {"b", "d", "f"}) {
    ASSERT_TRUE(db->Put({}, key, key).ok());
  }
  ASSERT_TRUE(db->Delete({}, "d").ok());
  const std::unique_ptr<Iterator> it = db->NewIterator({});

  it->Seek("");
  ASSERT_TRUE(it->Valid());
  EXPECT_EQ(it->key(), "b");
  it->Seek("b");
  ASSERT_TRUE(it->Valid());
  EXPECT_EQ(it->key(), "b");
  it->Seek("c");  // d is deleted, so the next key is f
  ASSERT_TRUE(it->Valid());
  EXPECT_EQ(it->key(), "f");
  it->Next();
  EXPECT_FALSE(it->Valid());
  it->Seek("g");
  EXPECT_FALSE(it->Valid());
  it->SeekToFirst();  // usable again after running off the end
  ASSERT_TRUE(it->Valid());
  EXPECT_EQ(it->key(), "b");
}

TEST(DBTest, IteratorIsASnapshot) {
  const std::unique_ptr<DB> db = OpenDB();
  ASSERT_TRUE(db->Put({}, "a", "1").ok());
  ASSERT_TRUE(db->Put({}, "b", "1").ok());
  const std::unique_ptr<Iterator> it = db->NewIterator({});

  ASSERT_TRUE(db->Put({}, "a", "2").ok());
  ASSERT_TRUE(db->Delete({}, "b").ok());
  ASSERT_TRUE(db->Put({}, "c", "new").ok());

  EXPECT_EQ(Scan(*it), "a=1 b=1 ") << "writes after creation are invisible";
  it->Seek("c");
  EXPECT_FALSE(it->Valid());
  EXPECT_EQ(Scan(*db), "a=2 c=new ") << "a new iterator sees them";
}

TEST(DBTest, IteratorKeepsWorkingAfterMoreWrites) {
  // The views it returned stay valid: entries live in the memtable's arena,
  // which the iterator keeps alive and which never moves data.
  const std::unique_ptr<DB> db = OpenDB();
  ASSERT_TRUE(db->Put({}, "key", "value").ok());
  const std::unique_ptr<Iterator> it = db->NewIterator({});
  it->SeekToFirst();
  ASSERT_TRUE(it->Valid());
  const std::string_view key = it->key();
  for (int i = 0; i < 10000; ++i) {  // many arena blocks' worth
    ASSERT_TRUE(db->Put({}, "filler" + std::to_string(i), std::string(100, 'x')).ok());
  }
  EXPECT_EQ(key, "key");
  EXPECT_EQ(it->value(), "value");
}

// A key with far more versions than kMaxSequentialSkip, between other keys,
// so Next and Seek go through the re-seek path, for both kinds of skipping:
// older versions of a key, and versions newer than the iterator's snapshot.
TEST(DBTest, IteratorSkipsManyVersionsByReseeking) {
  const std::unique_ptr<DB> db = OpenDB();
  ASSERT_TRUE(db->Put({}, "a", "a").ok());
  ASSERT_TRUE(db->Put({}, "z", "z").ok());
  for (int i = 0; i < 100; ++i) {
    ASSERT_TRUE(db->Put({}, "hot", "v" + std::to_string(i)).ok());
  }
  EXPECT_EQ(Scan(*db), "a=a hot=v99 z=z ");

  const std::unique_ptr<Iterator> snapshot = db->NewIterator({});
  for (int i = 100; i < 200; ++i) {  // 100 versions newer than `snapshot`
    ASSERT_TRUE(db->Put({}, "hot", "v" + std::to_string(i)).ok());
  }
  EXPECT_EQ(Scan(*snapshot), "a=a hot=v99 z=z ");
  snapshot->Seek("hot");
  ASSERT_TRUE(snapshot->Valid());
  EXPECT_EQ(snapshot->value(), "v99");

  ASSERT_TRUE(db->Delete({}, "hot").ok());  // a deletion on top of 200 versions
  EXPECT_EQ(Scan(*db), "a=a z=z ");
  const std::unique_ptr<Iterator> it = db->NewIterator({});
  it->Seek("b");
  ASSERT_TRUE(it->Valid());
  EXPECT_EQ(it->key(), "z");
}

// Built directly on a memtable to reach cases the DB never creates: here an
// entry with sequence 0 right where the iterator decides to re-seek.
TEST(DBIterTest, ReseekNextToSequenceZeroStillMakesProgress) {
  MemTableRef table = MemTable::Create();
  for (SequenceNumber sequence = 0; sequence <= kMaxSequentialSkip + 1; ++sequence) {
    table->Add(sequence, ValueType::kValue, "a", "a" + std::to_string(sequence));
  }
  table->Add(1, ValueType::kValue, "b", "b");
  const std::unique_ptr<Iterator> it = NewDBIterator(std::move(table), 100);
  EXPECT_EQ(Scan(*it), "a=a9 b=b ");
}

TEST(DBIterTest, ReadsAtTheGivenSequence) {
  const MemTableRef table = MemTable::Create();
  table->Add(1, ValueType::kValue, "a", "a1");
  table->Add(2, ValueType::kDeletion, "a", "");
  table->Add(3, ValueType::kValue, "b", "b3");
  // Each iterator takes its own reference (a copy of `table`).
  EXPECT_EQ(Scan(*NewDBIterator(table, 0)), "");
  EXPECT_EQ(Scan(*NewDBIterator(table, 1)), "a=a1 ");
  EXPECT_EQ(Scan(*NewDBIterator(table, 2)), "");
  EXPECT_EQ(Scan(*NewDBIterator(table, 3)), "b=b3 ");
}

// --- Concurrency --------------------------------------------------------------------------
//
// Two writers each repeatedly write a batch setting "x" and "y" to the same
// value, while readers check through iterators (each a snapshot) that x and y
// are always equal: a batch must never be seen half applied.

TEST(DBConcurrencyTest, ReadersNeverSeeHalfABatch) {
  constexpr int kWriters = 2;
  constexpr int kReaders = 3;
  constexpr int kBatchesPerWriter = 3000;
  const std::unique_ptr<DB> db = OpenDB();
  std::atomic<int> writers_done{0};
  std::latch start(kWriters + kReaders);
  std::vector<std::string> failures(kReaders);
  std::vector<std::thread> threads;
  threads.reserve(kWriters + kReaders);

  for (int w = 0; w < kWriters; ++w) {
    threads.emplace_back([&, w] {
      start.arrive_and_wait();
      for (int i = 0; i < kBatchesPerWriter; ++i) {
        const std::string value = std::to_string(w) + ":" + std::to_string(i);
        WriteBatch batch;
        batch.Put("x", value);
        batch.Put("y", value);
        EXPECT_TRUE(db->Write({}, batch).ok());
      }
      writers_done.fetch_add(1, std::memory_order_release);
    });
  }
  for (int r = 0; r < kReaders; ++r) {
    threads.emplace_back([&, r] {
      std::string& failure = failures[static_cast<std::size_t>(r)];
      start.arrive_and_wait();
      while (failure.empty() && writers_done.load(std::memory_order_acquire) < kWriters) {
        const std::unique_ptr<Iterator> it = db->NewIterator({});
        it->Seek("x");
        if (!it->Valid()) {
          continue;  // nothing written yet
        }
        const std::string x(it->value());
        it->Next();
        if (!it->Valid() || it->key() != "y" || it->value() != x) {
          failure = "x=" + x + " but y=" + (it->Valid() ? std::string(it->value()) : "missing");
        }
      }
    });
  }
  for (std::thread& thread : threads) {
    thread.join();
  }
  for (int r = 0; r < kReaders; ++r) {
    EXPECT_EQ(failures[static_cast<std::size_t>(r)], "") << "reader " << r;
  }
  const std::string x = Get(*db, "x");
  EXPECT_EQ(Get(*db, "y"), x);
}

}  // namespace
}  // namespace forgedb
