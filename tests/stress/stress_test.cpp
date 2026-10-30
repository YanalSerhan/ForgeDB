// Stress test (TODO 4.5): 8 reader threads (Gets, scans, snapshot reads) and
// 1 to 4 writer threads on one database while the background thread
// flushes and compacts, checked as it runs and against a model when it
// pauses (see stress/stress_workload.h for how reads are judged).
//
//   - The number of active writers cycles 4, 1, 2, 3 (every 15 s, or an
//     eighth of the run if that is shorter); paused writers wait.
//   - Every 2 minutes (or halfway through a short run) every thread stops;
//     the database is checked key by key and by a full scan against the
//     model (each group's last acknowledged version), closed (a compaction
//     may be running), reopened and checked again.
//   - Readers hold snapshots for up to a thousand operations, so
//     compactions must keep the versions they see.
//   - Reader 0 also checks that GetStats() counters never go down.
//
// The first problem stops every thread and fails the test with the seed.
// Thread interleavings can't be replayed, so the seed only fixes the random
// choices.
//
//   FORGEDB_STRESS_SECONDS   run length (default: 10 in optimized builds, 5
//                            in debug and sanitizer builds)
//   FORGEDB_STRESS_SEED      the seed (default: random, printed)
//
// StressTest.WindowsFileSystem runs on real files in %TEMP% (the exit-gate
// run: scripts/stress.ps1); StressTest.MemoryFileSystem on MemFileSystem.
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <functional>

#include <gtest/gtest.h>

#include <memory>
#include <mutex>
#include <optional>
#include <random>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

#include "forgedb/db.h"
#include "forgedb/iterator.h"
#include "forgedb/options.h"
#include "forgedb/result.h"
#include "forgedb/snapshot.h"
#include "forgedb/stats.h"
#include "forgedb/status.h"
#include "forgedb/write_batch.h"

#include "db/db_impl.h"
#include "env/mem_file_system.h"
#include "stress/stress_workload.h"
#include "support/test_env.h"

#ifndef FORGEDB_STRESS_TEST_SECONDS
#define FORGEDB_STRESS_TEST_SECONDS 5
#endif

namespace forgedb::stress {
namespace {

using Clock = std::chrono::steady_clock;

constexpr int kWriters = 4;
constexpr int kReaders = 8;
constexpr int kGroupsPerWriter = 2000;
constexpr int kHotGroups = kGroupsPerWriter / 20;  // half of all writes go to these
constexpr std::array<int, 4> kWriterSchedule = {4, 1, 2, 3};

// Small everything, so flushes, compactions, stalls and cache evictions
// happen all the time.
Options StressOptions() {
  Options options;
  options.memtable_bytes = std::size_t{64} << 10;
  options.max_bytes_for_level_base = std::size_t{512} << 10;
  options.target_file_size = std::size_t{128} << 10;
  options.block_cache_bytes = std::size_t{256} << 10;
  options.max_open_tables = 32;
  return options;
}

// Starts, pauses and stops the worker threads.
class Control {
 public:
  explicit Control(int threads) : threads_(threads) {}

  // Worker side: waits while this worker must not run. False: the run is
  // over (or failed).
  bool Proceed(bool writer, int index) {
    if (!MustWait(writer, index)) {
      return !stop_.load();
    }
    std::unique_lock lock(mutex_);
    ++parked_;
    cv_.notify_all();
    cv_.wait(lock, [&] { return stop_.load() || !MustWait(writer, index); });
    --parked_;
    return !stop_.load();
  }

  // The first failure wins; it stops every thread.
  void Fail(const std::string& problem) {
    {
      const std::scoped_lock lock(mutex_);
      if (failure_.empty()) {
        failure_ = problem;
      }
      stop_.store(true);
    }
    cv_.notify_all();
  }
  [[nodiscard]] bool Stopped() const { return stop_.load(); }
  [[nodiscard]] std::string Failure() {
    const std::scoped_lock lock(mutex_);
    return failure_;
  }

  // Controller side.
  void SetActiveWriters(int writers) {
    {
      const std::scoped_lock lock(mutex_);
      active_writers_.store(writers);
    }
    cv_.notify_all();
  }
  // Returns once every worker is parked (or the run stopped).
  void PauseAll() {
    std::unique_lock lock(mutex_);
    pause_.store(true);
    cv_.wait(lock, [&] { return stop_.load() || parked_ == threads_; });
  }
  void ResumeAll() {
    {
      const std::scoped_lock lock(mutex_);
      pause_.store(false);
    }
    cv_.notify_all();
  }
  void Stop() {
    {
      const std::scoped_lock lock(mutex_);
      stop_.store(true);
    }
    cv_.notify_all();
  }

 private:
  [[nodiscard]] bool MustWait(bool writer, int index) const {
    return !stop_.load() && (pause_.load() || (writer && index >= active_writers_.load()));
  }

  const int threads_;
  std::mutex mutex_;
  std::condition_variable cv_;
  int parked_ = 0;       // guarded by mutex_
  std::string failure_;  // guarded by mutex_
  // Written under mutex_ (so waiters wake), read without it.
  std::atomic<bool> stop_{false};
  std::atomic<bool> pause_{false};
  std::atomic<int> active_writers_{kWriters};
};

struct Counters {
  std::atomic<std::uint64_t> writes{0};
  std::atomic<std::uint64_t> gets{0};
  std::atomic<std::uint64_t> scans{0};
  std::atomic<std::uint64_t> scanned_entries{0};
  std::atomic<std::uint64_t> snapshot_reads{0};
  std::atomic<std::uint64_t> held_snapshot_checks{0};
  std::atomic<std::uint64_t> widest_window{0};
};

// Statistics summed over every open of the run.
struct Totals {
  std::uint64_t opens = 0;
  std::uint64_t user_bytes = 0;
  std::uint64_t table_bytes = 0;
  std::uint64_t flushes = 0;
  std::uint64_t compactions = 0;  // trivial moves included
  std::uint64_t stalls = 0;
  std::uint64_t stall_microseconds = 0;

  void Add(const DBStats& stats) {
    ++opens;
    user_bytes += stats.user_bytes;
    table_bytes += stats.TableBytesWritten();
    flushes += stats.flushes;
    for (const LevelStats& level : stats.levels) {
      compactions += level.compactions.count + level.compactions.trivial_moves;
    }
    for (const StallStats& stall :
         {stats.memtable_full, stats.level0_slowdown, stats.level0_stop}) {
      stalls += stall.count;
      stall_microseconds += stall.microseconds;
    }
  }
};

using Opener = std::function<Result<std::unique_ptr<DB>>()>;

class StressRun {
 public:
  StressRun(Opener open, std::uint64_t seed) : open_(std::move(open)), seed_(seed) {
    for (int w = 0; w < kWriters; ++w) {
      started_.emplace_back(kGroupsPerWriter);
      acked_.emplace_back(kGroupsPerWriter);
    }
  }

  // Runs for `duration`; returns the first problem, or "".
  std::string Run(std::chrono::milliseconds duration);

 private:
  struct HeldSnapshot {
    std::uint64_t generation = 0;
    std::shared_ptr<const Snapshot> snapshot;
    int writer = 0;
    int group = 0;
    GroupView view;
    std::uint64_t operations_left = 0;
  };

  void Writer(int writer);
  void Reader(int reader);
  // Reader operations; each returns a problem or "".
  std::string ReadOne(std::mt19937_64& rng);
  std::string Scan(std::mt19937_64& rng);
  std::string SnapshotRead(std::mt19937_64& rng, std::optional<HeldSnapshot>& held);
  std::string CheckHeld(const HeldSnapshot& held);
  std::string CheckStats(std::optional<DBStats>& last, std::uint64_t& last_generation);

  // Every group's 4 keys as one snapshot (or the current state) shows them,
  // read by Get and by an iterator; a problem if the two differ.
  std::string ReadGroup(const std::shared_ptr<const Snapshot>& snapshot, int writer, int group,
                        GroupView& view);

  // Controller side, every worker parked.
  std::string CheckModel(const std::string& when);
  std::string Reopen();

  // A group's started and acknowledged versions (see stress_workload.h).
  std::atomic<std::uint64_t>& Started(int writer, int group) {
    return started_.at(static_cast<std::size_t>(writer)).at(static_cast<std::size_t>(group));
  }
  std::atomic<std::uint64_t>& Acked(int writer, int group) {
    return acked_.at(static_cast<std::size_t>(writer)).at(static_cast<std::size_t>(group));
  }

  static int PickGroup(std::mt19937_64& rng) {
    const auto pick = static_cast<int>(rng() % kGroupsPerWriter);
    return rng() % 2 == 0 ? pick % kHotGroups : pick;
  }

  Opener open_;
  const std::uint64_t seed_;
  std::unique_ptr<DB> db_;  // replaced only while every worker is parked
  std::atomic<std::uint64_t> generation_{0};
  std::vector<std::vector<std::atomic<std::uint64_t>>> started_;
  std::vector<std::vector<std::atomic<std::uint64_t>>> acked_;
  Control control_{kWriters + kReaders};
  Counters counters_;
  Totals totals_;
};

void StressRun::Writer(int writer) {
  std::mt19937_64 rng((seed_ * 1000) + static_cast<std::uint64_t>(writer));
  while (control_.Proceed(/*writer=*/true, writer)) {
    const int group = PickGroup(rng);
    std::atomic<std::uint64_t>& group_started = Started(writer, group);
    const std::uint64_t version = group_started.load() + 1;  // only this thread writes it
    WriteBatch batch;
    for (int member = 0; member < kKeysPerGroup; ++member) {
      const KeyId id{.writer = writer, .group = group, .member = member};
      if (IsDeletion(writer, group, version)) {
        batch.Delete(MakeKey(id));
      } else {
        batch.Put(MakeKey(id), MakeValue(id, version));
      }
    }
    group_started.store(version);
    const Status status = db_->Write({.sync = rng() % 64 == 0}, batch);
    if (!status.ok()) {
      control_.Fail("writer " + std::to_string(writer) + ": " + status.ToString());
      return;
    }
    Acked(writer, group).store(version);
    counters_.writes.fetch_add(1);
  }
}

std::string StressRun::ReadOne(std::mt19937_64& rng) {
  const KeyId id{.writer = static_cast<int>(rng() % kWriters),
                 .group = PickGroup(rng),
                 .member = static_cast<int>(rng() % kKeysPerGroup)};
  const std::uint64_t lo = Acked(id.writer, id.group).load();
  Result<std::string> value = db_->Get({}, MakeKey(id));
  const std::uint64_t hi = Started(id.writer, id.group).load();
  counters_.gets.fetch_add(1);
  std::uint64_t widest = counters_.widest_window.load();
  while (hi - lo > widest && !counters_.widest_window.compare_exchange_weak(widest, hi - lo)) {
  }
  std::optional<std::string> observed;
  if (value.ok()) {
    observed = std::move(value).value();
  } else if (value.status().code() != Status::Code::kNotFound) {
    return "Get " + MakeKey(id) + ": " + value.status().ToString();
  }
  return CheckRead(id, observed, lo, hi);
}

std::string StressRun::Scan(std::mt19937_64& rng) {
  const auto writer = static_cast<int>(rng() % kWriters);
  const int first = PickGroup(rng);
  const int count = std::min(1 + static_cast<int>(rng() % 50), kGroupsPerWriter - first);
  std::vector<std::uint64_t> lo(static_cast<std::size_t>(count));
  for (int i = 0; i < count; ++i) {
    lo[static_cast<std::size_t>(i)] = Acked(writer, first + i).load();
  }
  std::vector<GroupView> views(static_cast<std::size_t>(count));
  {
    const std::unique_ptr<Iterator> it = db_->NewIterator({});
    const std::string end = GroupStart(writer, first + count);
    std::string previous;
    for (it->Seek(GroupStart(writer, first)); it->Valid() && it->key() < end; it->Next()) {
      const std::optional<KeyId> id = ParseKey(it->key());
      if (!id || id->writer != writer || id->group < first || id->group >= first + count) {
        return "scan returned an unexpected key " + std::string(it->key());
      }
      if (!previous.empty() && std::string_view(previous) >= it->key()) {
        return "scan out of order: " + previous + " then " + std::string(it->key());
      }
      previous = it->key();
      views[static_cast<std::size_t>(id->group - first)][static_cast<std::size_t>(id->member)] =
          std::string(it->value());
      counters_.scanned_entries.fetch_add(1);
    }
    if (!it->status().ok()) {
      return "scan: " + it->status().ToString();
    }
  }
  counters_.scans.fetch_add(1);
  for (int i = 0; i < count; ++i) {
    const std::uint64_t hi = Started(writer, first + i).load();
    const std::string problem = CheckGroup(writer, first + i, views[static_cast<std::size_t>(i)],
                                           lo[static_cast<std::size_t>(i)], hi);
    if (!problem.empty()) {
      return "scan: " + problem;
    }
  }
  return "";
}

std::string StressRun::ReadGroup(const std::shared_ptr<const Snapshot>& snapshot, int writer,
                                 int group, GroupView& view) {
  const ReadOptions options{.snapshot = snapshot};
  for (int member = 0; member < kKeysPerGroup; ++member) {
    const std::string key = MakeKey({.writer = writer, .group = group, .member = member});
    Result<std::string> value = db_->Get(options, key);
    if (value.ok()) {
      view[static_cast<std::size_t>(member)] = std::move(value).value();
    } else if (value.status().code() == Status::Code::kNotFound) {
      view[static_cast<std::size_t>(member)].reset();
    } else {
      return "snapshot Get " + key + ": " + value.status().ToString();
    }
  }
  // The same through an iterator at the snapshot.
  GroupView scanned;
  const std::unique_ptr<Iterator> it = db_->NewIterator(options);
  const std::string end = GroupStart(writer, group + 1);
  for (it->Seek(GroupStart(writer, group)); it->Valid() && it->key() < end; it->Next()) {
    const std::optional<KeyId> id = ParseKey(it->key());
    if (!id) {
      return "snapshot scan returned an unexpected key " + std::string(it->key());
    }
    scanned[static_cast<std::size_t>(id->member)] = std::string(it->value());
  }
  if (!it->status().ok()) {
    return "snapshot scan: " + it->status().ToString();
  }
  if (scanned != view) {
    return "group " + GroupStart(writer, group) + ": a snapshot's Gets and its iterator disagree";
  }
  return "";
}

std::string StressRun::SnapshotRead(std::mt19937_64& rng, std::optional<HeldSnapshot>& held) {
  HeldSnapshot read;
  read.writer = static_cast<int>(rng() % kWriters);
  read.group = PickGroup(rng);
  const std::uint64_t lo = Acked(read.writer, read.group).load();
  read.snapshot = db_->GetSnapshot();
  const std::uint64_t hi = Started(read.writer, read.group).load();
  read.generation = generation_.load();
  std::string problem = ReadGroup(read.snapshot, read.writer, read.group, read.view);
  if (problem.empty()) {
    problem = CheckGroup(read.writer, read.group, read.view, lo, hi);
  }
  counters_.snapshot_reads.fetch_add(1);
  if (!problem.empty()) {
    return "snapshot: " + problem;
  }
  if (!held && rng() % 2 == 0) {
    // Keep it for a while: it must show exactly this when read again later,
    // whatever is written, flushed and compacted meanwhile.
    read.operations_left = 1 + (rng() % 1000);
    held = std::move(read);
  }
  return "";
}

std::string StressRun::CheckHeld(const HeldSnapshot& held) {
  GroupView again;
  std::string problem = ReadGroup(held.snapshot, held.writer, held.group, again);
  if (problem.empty() && again != held.view) {
    problem =
        "group " + GroupStart(held.writer, held.group) + ": a held snapshot now reads differently";
  }
  counters_.held_snapshot_checks.fetch_add(1);
  return problem.empty() ? "" : "held snapshot: " + problem;
}

std::string StressRun::CheckStats(std::optional<DBStats>& last, std::uint64_t& last_generation) {
  DBStats stats = db_->GetStats();
  if (stats.levels.size() != 7) {
    return "GetStats: " + std::to_string(stats.levels.size()) + " levels";
  }
  const std::uint64_t generation = generation_.load();
  if (last && last_generation == generation) {
    if (stats.user_bytes < last->user_bytes || stats.log_bytes < last->log_bytes ||
        stats.flushes < last->flushes || stats.TableBytesWritten() < last->TableBytesWritten()) {
      return "GetStats: a counter went down";
    }
  }
  last = std::move(stats);
  last_generation = generation;
  return "";
}

void StressRun::Reader(int reader) {
  std::mt19937_64 rng((seed_ * 1000) + 100 + static_cast<std::uint64_t>(reader));
  std::optional<HeldSnapshot> held;
  std::optional<DBStats> last_stats;
  std::uint64_t stats_generation = 0;
  std::uint64_t operations = 0;
  while (control_.Proceed(/*writer=*/false, reader)) {
    if (held && held->generation != generation_.load()) {
      held.reset();  // its DB was closed: snapshots end with it
    }
    std::string problem;
    const std::uint64_t pick = rng() % 100;
    if (pick < 50) {
      problem = ReadOne(rng);
    } else if (pick < 70) {
      problem = Scan(rng);
    } else {
      problem = SnapshotRead(rng, held);
    }
    if (problem.empty() && held && --held->operations_left == 0) {
      problem = CheckHeld(*held);
      held.reset();
    }
    if (problem.empty() && reader == 0 && ++operations % 256 == 0) {
      problem = CheckStats(last_stats, stats_generation);
    }
    if (!problem.empty()) {
      control_.Fail("reader " + std::to_string(reader) + ": " + problem);
      return;
    }
  }
}

std::string StressRun::CheckModel(const std::string& when) {
  std::size_t expected_keys = 0;
  for (int writer = 0; writer < kWriters; ++writer) {
    for (int group = 0; group < kGroupsPerWriter; ++group) {
      const std::uint64_t version = Acked(writer, group).load();
      if (Started(writer, group).load() != version) {
        return when + ": group " + GroupStart(writer, group) + " has a write in flight";
      }
      expected_keys += IsAbsent(writer, group, version) ? 0 : kKeysPerGroup;
      for (int member = 0; member < kKeysPerGroup; ++member) {
        const KeyId id{.writer = writer, .group = group, .member = member};
        Result<std::string> value = db_->Get({}, MakeKey(id));
        std::optional<std::string> observed;
        if (value.ok()) {
          observed = std::move(value).value();
        } else if (value.status().code() != Status::Code::kNotFound) {
          return when + ": Get " + MakeKey(id) + ": " + value.status().ToString();
        }
        // Nothing is in flight: the window is exactly the last version.
        const std::string problem = CheckRead(id, observed, version, version);
        if (!problem.empty()) {
          return std::string(when).append(": ").append(problem);
        }
      }
    }
  }
  // A full scan holds exactly those keys and nothing else.
  std::size_t scanned = 0;
  const std::unique_ptr<Iterator> it = db_->NewIterator({});
  for (it->SeekToFirst(); it->Valid(); it->Next()) {
    const std::optional<KeyId> id = ParseKey(it->key());
    if (!id || id->writer >= kWriters || id->group >= kGroupsPerWriter) {
      return when + ": full scan found an unexpected key " + std::string(it->key());
    }
    const std::uint64_t version = Acked(id->writer, id->group).load();
    const std::string problem = CheckRead(*id, std::string(it->value()), version, version);
    if (!problem.empty()) {
      return std::string(when).append(": full scan: ").append(problem);
    }
    ++scanned;
  }
  if (!it->status().ok()) {
    return when + ": full scan: " + it->status().ToString();
  }
  if (scanned != expected_keys) {
    return when + ": full scan found " + std::to_string(scanned) + " keys, the model has " +
           std::to_string(expected_keys);
  }
  return "";
}

std::string StressRun::Reopen() {
  totals_.Add(db_->GetStats());
  db_.reset();  // a compaction may be running: it is abandoned
  Result<std::unique_ptr<DB>> db = open_();
  if (!db.ok()) {
    return "reopen: " + db.status().ToString();
  }
  db_ = std::move(db).value();
  generation_.fetch_add(1);
  return "";
}

std::string StressRun::Run(std::chrono::milliseconds duration) {
  Result<std::unique_ptr<DB>> db = open_();
  if (!db.ok()) {
    return "open: " + db.status().ToString();
  }
  db_ = std::move(db).value();

  std::vector<std::thread> threads;
  threads.reserve(kWriters + kReaders);
  for (int w = 0; w < kWriters; ++w) {
    threads.emplace_back([this, w] { Writer(w); });
  }
  for (int r = 0; r < kReaders; ++r) {
    threads.emplace_back([this, r] { Reader(r); });
  }

  const auto start = Clock::now();
  const auto deadline = start + duration;
  const auto phase = std::clamp<Clock::duration>(duration / 8, std::chrono::milliseconds(500),
                                                 std::chrono::seconds(15));
  const auto reopen_every = std::min<Clock::duration>(std::chrono::minutes(2), duration / 2);
  auto next_phase = start + phase;
  auto next_reopen = start + reopen_every;
  std::size_t phase_index = 0;
  std::string problem;
  while (problem.empty() && !control_.Stopped() && Clock::now() < deadline) {
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    const auto now = Clock::now();
    if (now >= next_phase) {
      ++phase_index;
      control_.SetActiveWriters(kWriterSchedule.at(phase_index % kWriterSchedule.size()));
      next_phase += phase;
    }
    if (now >= next_reopen && now + reopen_every / 4 < deadline) {
      control_.PauseAll();
      if (!control_.Stopped()) {
        problem = CheckModel("before reopen");
        if (problem.empty()) {
          problem = Reopen();
        }
        if (problem.empty()) {
          problem = CheckModel("after reopen");
        }
      }
      if (!problem.empty()) {
        control_.Stop();  // the workers must not run on a database that failed to open
      }
      control_.ResumeAll();
      next_reopen += reopen_every;
    }
  }
  // The end: everything stops, then the model check, a last reopen and a
  // last check.
  if (problem.empty()) {
    control_.PauseAll();
    if (!control_.Stopped()) {
      problem = CheckModel("at the end");
      if (problem.empty()) {
        problem = Reopen();
      }
      if (problem.empty()) {
        problem = CheckModel("after the last reopen");
      }
    }
  }
  control_.Stop();
  for (std::thread& thread : threads) {
    thread.join();
  }
  if (problem.empty()) {
    problem = control_.Failure();
  }
  if (db_) {
    totals_.Add(db_->GetStats());
    db_.reset();
  }

  const double seconds = std::chrono::duration<double>(Clock::now() - start).count();
  const auto rate = [seconds](const std::atomic<std::uint64_t>& n) {
    return static_cast<double>(n.load()) / seconds;
  };
  std::printf(  // NOLINT(*-vararg)
      "[ stress   ] seed %llu, %.1f s: %.0f writes/s (batches of %d keys), %.0f Gets/s, %.0f "
      "scans/s (%.0f entries/s), %.0f snapshot reads/s, %llu held-snapshot checks; widest Get "
      "window %llu versions\n",
      static_cast<unsigned long long>(seed_), seconds, rate(counters_.writes), kKeysPerGroup,
      rate(counters_.gets), rate(counters_.scans), rate(counters_.scanned_entries),
      rate(counters_.snapshot_reads),
      static_cast<unsigned long long>(counters_.held_snapshot_checks.load()),
      static_cast<unsigned long long>(counters_.widest_window.load()));
  std::printf(  // NOLINT(*-vararg)
      "[ stress   ] %llu opens, %llu flushes, %llu compactions, write amplification %.2f, %llu "
      "stalls (%.1f s in total)\n",
      static_cast<unsigned long long>(totals_.opens),
      static_cast<unsigned long long>(totals_.flushes),
      static_cast<unsigned long long>(totals_.compactions),
      static_cast<double>(totals_.table_bytes) /
          static_cast<double>(std::max<std::uint64_t>(totals_.user_bytes, 1)),
      static_cast<unsigned long long>(totals_.stalls),
      static_cast<double>(totals_.stall_microseconds) / 1e6);
  if (problem.empty()) {
    if (counters_.writes.load() == 0 || counters_.gets.load() == 0 || counters_.scans.load() == 0 ||
        counters_.snapshot_reads.load() == 0) {
      problem = "some kind of operation never ran";
    } else if (totals_.compactions == 0) {
      problem = "no compaction ran";
    }
  }
  return problem;
}

std::chrono::milliseconds Duration() {
  return std::chrono::seconds(GetEnvNumber("FORGEDB_STRESS_SECONDS", FORGEDB_STRESS_TEST_SECONDS));
}

std::uint64_t Seed() {
  std::random_device random;
  return GetEnvNumber("FORGEDB_STRESS_SEED", (std::uint64_t{random()} << 32U) | random());
}

TEST(StressTest, MemoryFileSystem) {
  const std::uint64_t seed = Seed();
  const auto fs = std::make_shared<MemFileSystem>();
  StressRun run([fs] { return OpenDB(StressOptions(), "stress", fs); }, seed);
  const std::string problem = run.Run(Duration());
  EXPECT_EQ(problem, "") << "seed " << seed << " (FORGEDB_STRESS_SEED)";
}

TEST(StressTest, WindowsFileSystem) {
  const std::uint64_t seed = Seed();
  std::error_code error;
  const std::filesystem::path dir =
      std::filesystem::temp_directory_path(error) / ("forgedb-stress-" + std::to_string(seed));
  ASSERT_FALSE(error) << error.message();
  std::filesystem::remove_all(dir, error);
  StressRun run([dir] { return DB::Open(StressOptions(), dir); }, seed);
  const std::string problem = run.Run(Duration());
  EXPECT_EQ(problem, "") << "seed " << seed << " (FORGEDB_STRESS_SEED); files kept in "
                         << dir.string();
  if (problem.empty()) {
    std::filesystem::remove_all(dir, error);
  }
}

// --- The workload's own rules -------------------------------------------------------

TEST(StressWorkloadTest, KeysRoundTripAndSortByWriterGroupMember) {
  const KeyId id{.writer = 3, .group = 1234, .member = 2};
  EXPECT_EQ(MakeKey(id), "w3-g0001234-k2");
  EXPECT_EQ(ParseKey(MakeKey(id)), id);
  EXPECT_LT(MakeKey({.writer = 0, .group = 1999, .member = 3}),
            MakeKey({.writer = 1, .group = 0, .member = 0}));
  EXPECT_LT(MakeKey({.writer = 1, .group = 9, .member = 3}),
            MakeKey({.writer = 1, .group = 10, .member = 0}));
  EXPECT_LT(MakeKey({.writer = 1, .group = 1999, .member = 3}), GroupStart(1, 2000));
  EXPECT_FALSE(ParseKey("w3-g001234-k2"));
  EXPECT_FALSE(ParseKey("w3-g0001234-k4"));
  EXPECT_FALSE(ParseKey("w03-g0001234-k2"));
}

TEST(StressWorkloadTest, ValuesIdentifyTheirKeyAndVersionAndCatchDamage) {
  const KeyId id{.writer = 1, .group = 7, .member = 3};
  std::size_t longest = 0;
  for (std::uint64_t version = 1; version < 200; ++version) {
    const std::string value = MakeValue(id, version);
    longest = std::max(longest, value.size());
    ASSERT_EQ(ValueVersion(id, value), version);
    EXPECT_FALSE(ValueVersion({.writer = 1, .group = 7, .member = 2}, value)) << "another key";
    std::string damaged = value;
    damaged.back() = damaged.back() == 'a' ? 'b' : 'a';
    EXPECT_FALSE(ValueVersion(id, damaged));
    EXPECT_FALSE(ValueVersion(id, value.substr(0, value.size() - 1)));
  }
  EXPECT_GT(longest, 1000U) << "some values are long";
}

TEST(StressWorkloadTest, ReadsAreJudgedByTheirWindow) {
  const int writer = 0;
  const int group = 5;
  const KeyId id{.writer = writer, .group = group, .member = 1};
  std::uint64_t deletion = 0;  // the first deletion version, and a value version after it
  for (std::uint64_t v = 2; deletion == 0; ++v) {
    if (IsDeletion(writer, group, v) && !IsDeletion(writer, group, v + 1) &&
        !IsDeletion(writer, group, v - 1)) {
      deletion = v;
    }
  }
  const std::uint64_t value = deletion + 1;
  EXPECT_EQ(CheckRead(id, MakeValue(id, value), value, value), "");
  EXPECT_EQ(CheckRead(id, MakeValue(id, value), value - 1, value + 3), "");
  EXPECT_NE(CheckRead(id, MakeValue(id, value), value + 1, value + 3), "") << "too old";
  EXPECT_NE(CheckRead(id, MakeValue(id, value), 0, value - 1), "") << "not started yet";
  EXPECT_EQ(CheckRead(id, std::nullopt, deletion, deletion), "");
  EXPECT_EQ(CheckRead(id, std::nullopt, 0, 0), "") << "never written";
  EXPECT_NE(CheckRead(id, std::nullopt, value, value), "") << "nothing deleted it";
  EXPECT_NE(CheckRead(id, std::string("junk"), 0, 1000), "");
}

TEST(StressWorkloadTest, AGroupMustBeSeenAtOneVersion) {
  const int writer = 2;
  const int group = 9;
  std::uint64_t v = 1;
  while (IsDeletion(writer, group, v) || IsDeletion(writer, group, v + 1)) {
    ++v;
  }
  GroupView view;
  for (int member = 0; member < kKeysPerGroup; ++member) {
    view[static_cast<std::size_t>(member)] =
        MakeValue({.writer = writer, .group = group, .member = member}, v);
  }
  EXPECT_EQ(CheckGroup(writer, group, view, v, v + 1), "");
  GroupView torn = view;
  torn[2] = MakeValue({.writer = writer, .group = group, .member = 2}, v + 1);
  EXPECT_NE(CheckGroup(writer, group, torn, v, v + 1), "");
  GroupView missing = view;
  missing[3].reset();
  EXPECT_NE(CheckGroup(writer, group, missing, v, v + 1), "");
  EXPECT_EQ(CheckGroup(writer, group, GroupView{}, 0, 0), "") << "never written";
}

}  // namespace
}  // namespace forgedb::stress
