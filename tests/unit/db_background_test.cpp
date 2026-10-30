// The background thread (TODO 4.2): flushes run while writes go on; writers
// wait only when the memtable fills again before the flush is done; errors
// and shutdown.
//
// A gate in front of the file system (support/gated_file_system.h) holds
// every table's Sync until the test opens it, so a flush can be kept running
// for as long as a test needs.
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <filesystem>

#include <gtest/gtest.h>

#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include "forgedb/db.h"
#include "forgedb/options.h"
#include "forgedb/result.h"
#include "forgedb/status.h"

#include "db/db_impl.h"
#include "env/mem_file_system.h"
#include "support/gated_file_system.h"

namespace forgedb {
namespace {

std::string Key(int i) {
  const std::string digits = std::to_string(i);
  return "key" + std::string(digits.size() < 6 ? 6 - digits.size() : 0, '0') + digits;
}

std::string Value(int i) { return "value-" + std::to_string(i) + std::string(200, 'x'); }

std::string Get(DB& db, std::string_view key) {
  Result<std::string> value = db.Get({}, key);
  if (value.ok()) {
    return std::move(value).value();
  }
  return value.status().code() == Status::Code::kNotFound ? "NOT_FOUND"
                                                          : "ERROR: " + value.status().ToString();
}

DBImpl& Impl(DB& db) { return dynamic_cast<DBImpl&>(db); }

// A writer thread putting keys [first, first + count) until one fails.
class Writer {
 public:
  Writer(DB* db, int first, int count)
      : thread_([this, db, first, count] {
          for (int i = first; i < first + count; ++i) {
            const Status status = db->Put({}, Key(i), Value(i));
            if (!status.ok()) {
              const std::scoped_lock lock(mutex_);
              failure_ = status;
              break;
            }
            written_.fetch_add(1);
          }
          done_.store(true);
        }) {}
  ~Writer() { Join(); }
  Writer(const Writer&) = delete;
  Writer& operator=(const Writer&) = delete;
  Writer(Writer&&) = delete;
  Writer& operator=(Writer&&) = delete;

  // Waits until the writer has made no progress for 300 ms (it is waiting
  // for the flush); false if it finished instead.
  bool WaitUntilStuck() {
    int last = -1;
    while (!done_.load()) {
      const int now = written_.load();
      if (now == last) {
        return true;
      }
      last = now;
      std::this_thread::sleep_for(std::chrono::milliseconds(300));
    }
    return false;
  }
  void Join() {
    if (thread_.joinable()) {
      thread_.join();
    }
  }
  [[nodiscard]] int Written() const { return written_.load(); }
  [[nodiscard]] Status Failure() {
    const std::scoped_lock lock(mutex_);
    return failure_;
  }

 private:
  std::atomic<int> written_{0};
  std::atomic<bool> done_{false};
  std::mutex mutex_;
  Status failure_;
  std::thread thread_;  // last: starts after the members above exist
};

class BackgroundTest : public testing::Test {
 protected:
  std::unique_ptr<DB> Open() {
    Options options;
    options.memtable_bytes = std::size_t{16} << 10;  // about 70 of these values
    Result<std::unique_ptr<DB>> db = OpenDB(options, "db", gated_);
    EXPECT_TRUE(db.ok()) << db.status().ToString();
    return db.ok() ? std::move(db).value() : nullptr;
  }

  // Writes keys from `next_` until a write switches memtables, then waits
  // until that flush is held at the gate.
  void WriteUntilAFlushIsHeld(DB& db) {
    gate_.Close();
    const std::uint64_t flushes = Impl(db).FlushesForTesting();
    while (!Impl(db).FlushPendingForTesting()) {
      ASSERT_TRUE(db.Put({}, Key(next_), Value(next_)).ok());
      ++next_;
    }
    gate_.WaitForArrival();
    EXPECT_EQ(Impl(db).FlushesForTesting(), flushes) << "the flush is held";
  }

  Gate gate_;
  std::shared_ptr<MemFileSystem> base_ = std::make_shared<MemFileSystem>();
  std::shared_ptr<GatedFileSystem> gated_ = std::make_shared<GatedFileSystem>(base_, &gate_);
  int next_ = 0;  // the next key to write
};

TEST_F(BackgroundTest, WritesGoOnWhileAFlushRunsAndWaitOnlyWhenTheMemtableFillsAgain) {
  const std::unique_ptr<DB> db = Open();
  WriteUntilAFlushIsHeld(*db);
  EXPECT_EQ(Impl(*db).GetStats().memtable_full.count, 0U);

  // A writer fills the new memtable, then has to wait for the held flush.
  Writer writer(db.get(), next_, 500);
  ASSERT_TRUE(writer.WaitUntilStuck()) << "it cannot finish: 500 writes need two more flushes";
  EXPECT_GT(writer.Written(), 0) << "writes went on while the flush was held";
  EXPECT_LT(writer.Written(), 500);
  for (int i = 0; i < next_ + writer.Written(); ++i) {
    ASSERT_EQ(Get(*db, Key(i)), Value(i)) << "readers see the flushing memtable too";
  }

  gate_.Open();
  writer.Join();
  ASSERT_TRUE(writer.Failure().ok()) << writer.Failure().ToString();
  next_ += 500;
  EXPECT_GE(Impl(*db).GetStats().memtable_full.count, 1U);
  Impl(*db).WaitForFlushForTesting();
  EXPECT_GE(Impl(*db).FlushesForTesting(), 2U);
  for (int i = 0; i < next_; ++i) {
    ASSERT_EQ(Get(*db, Key(i)), Value(i));
  }
}

TEST_F(BackgroundTest, AFailedFlushWakesTheWaitingWriterWithTheError) {
  int acknowledged = 0;
  {
    const std::unique_ptr<DB> db = Open();
    WriteUntilAFlushIsHeld(*db);
    Writer writer(db.get(), next_, 500);
    ASSERT_TRUE(writer.WaitUntilStuck());
    gate_.Open(Status::IOError("injected table sync error"));
    writer.Join();
    EXPECT_EQ(writer.Failure().code(), Status::Code::kIOError) << writer.Failure().ToString();
    EXPECT_FALSE(db->Put({}, "later", "x").ok()) << "the error is sticky";
    acknowledged = next_ + writer.Written();
    for (int i = 0; i < acknowledged; ++i) {
      ASSERT_EQ(Get(*db, Key(i)), Value(i)) << "reads go on";
    }
  }
  gate_.Open();
  const std::unique_ptr<DB> db = Open();
  for (int i = 0; i < acknowledged; ++i) {
    ASSERT_EQ(Get(*db, Key(i)), Value(i)) << "nothing acknowledged was lost";
  }
}

TEST_F(BackgroundTest, ShutdownFinishesTheRunningFlush) {
  std::unique_ptr<DB> db = Open();
  WriteUntilAFlushIsHeld(*db);
  std::atomic<bool> closed{false};
  std::thread closer([&] {
    db.reset();
    closed.store(true);
  });
  std::this_thread::sleep_for(std::chrono::milliseconds(100));
  EXPECT_FALSE(closed.load()) << "the destructor waits for the running flush";
  gate_.Open();
  closer.join();

  db = Open();
  // The flush finished, so its MANIFEST record names the new log: only that
  // log is replayed.
  EXPECT_EQ(Impl(*db).GetRecoveryInfo().logs_replayed.size(), 1U);
  for (int i = 0; i < next_; ++i) {
    ASSERT_EQ(Get(*db, Key(i)), Value(i));
  }
}

TEST_F(BackgroundTest, SynchronousFlushHookWaitsForTheFlush) {
  const std::unique_ptr<DB> db = Open();
  Impl(*db).SetSynchronousFlushForTesting(true);
  std::uint64_t flushes = 0;
  for (int i = 0; i < 300; ++i) {
    ASSERT_TRUE(db->Put({}, Key(i), Value(i)).ok());
    // A write that switched memtables returns only after the flush is done:
    // no flush is ever left pending.
    flushes = Impl(*db).FlushesForTesting();
    Impl(*db).WaitForFlushForTesting();
    ASSERT_EQ(Impl(*db).FlushesForTesting(), flushes) << i;
  }
  EXPECT_GE(flushes, 3U);
  EXPECT_EQ(Impl(*db).GetStats().memtable_full.count, 0U);
}

}  // namespace
}  // namespace forgedb
