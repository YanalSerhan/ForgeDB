// Crash torture test (TODO 2.5): crash the database over and over at random
// points on the fault-injection file system, and check after every recovery
// that it kept its promises (docs/adr/0002):
//
//   - every write acknowledged with sync = true survives a power loss;
//   - every acknowledged write survives a process crash or a clean close;
//   - the recovered state is exactly the state after some prefix of the
//     writes, never a mix (no holes), compared byte for byte with std::map;
//   - recovery never fails because of what a crash leaves behind.
//
// One run is a database that lives through many cycles. Each cycle opens it,
// checks the recovered state, applies random writes (random sync flags), and
// ends with a power loss, a process crash or a clean close, either between
// writes or in the middle of a file operation (CrashAtOperation). A run
// starts a fresh database every kCyclesPerDatabase cycles, because Phase 2
// never deletes logs and every open replays all of them.
//
// Run-time overrides:
//   FORGEDB_CRASH_CYCLES=5000   cycles per test (default: see CMakeLists.txt)
//   FORGEDB_CRASH_SEED=17       run only this seed (e.g. to reproduce a failure)
#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <filesystem>

#include <gtest/gtest.h>

#include <map>
#include <memory>
#include <random>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "forgedb/db.h"
#include "forgedb/iterator.h"
#include "forgedb/options.h"
#include "forgedb/write_batch.h"

#include "db/db_impl.h"
#include "env/mem_file_system.h"
#include "log/log_format.h"
#include "support/fault_injection_file_system.h"
#include "support/test_env.h"
#include "util/coding.h"
#include "util/crc32c.h"

#ifndef FORGEDB_CRASH_TEST_CYCLES
#define FORGEDB_CRASH_TEST_CYCLES 200
#endif

namespace forgedb {
namespace {

using CrashKind = FaultInjectionFileSystem::CrashKind;
using Op = FaultInjectionFileSystem::Op;
using UnsyncedTail = FaultInjectionFileSystem::UnsyncedTail;
using State = std::map<std::string, std::string>;

constexpr std::size_t kCyclesPerDatabase = 50;
constexpr std::size_t kMaxWritesPerCycle = 100;
constexpr std::size_t kKeySpace = 24;

// --- Writes and their expected effect ----------------------------------------------------------

struct Mutation {
  bool is_put = true;
  std::string key;
  std::string value;
};
using Batch = std::vector<Mutation>;

void ApplyTo(State& state, const Batch& batch) {
  for (const Mutation& mutation : batch) {
    if (mutation.is_put) {
      state[mutation.key] = mutation.value;
    } else {
      state.erase(mutation.key);
    }
  }
}

std::string Describe(const Batch& batch) {
  std::string text = "{";
  for (const Mutation& mutation : batch) {
    text += mutation.is_put
                ? " Put(" + mutation.key + ", " + std::to_string(mutation.value.size()) + " bytes)"
                : " Delete(" + mutation.key + ")";
  }
  return text + " }";
}

class Random {
 public:
  explicit Random(std::uint64_t seed) : rng_(seed) {}
  std::uint64_t Below(std::uint64_t bound) { return rng_() % bound; }  // bound > 0
  bool Percent(std::uint64_t percent) { return Below(100) < percent; }

 private:
  std::mt19937_64 rng_;
};

// A valid, complete log fragment (FULL, holding `payload`), as a value may
// contain one: a log file stored as a value, or bytes chosen to look like one.
std::string LogFragment(std::string_view payload) {
  std::string fragment;
  const char type = static_cast<char>(log::RecordType::kFull);
  PutFixed32(fragment,
             crc32c::Mask(crc32c::Extend(crc32c::Value(std::string_view(&type, 1)), payload)));
  fragment.push_back(static_cast<char>(payload.size() & 0xFFU));
  fragment.push_back(static_cast<char>(payload.size() >> 8U));
  fragment.push_back(type);
  fragment.append(payload);
  return fragment;
}

// Mostly small values; some large enough to cross 32 KiB log blocks, so torn
// tails and crashes land inside multi-fragment records too. With
// `log_fragments`, many values also contain valid log fragments: recovery
// must not mistake them for log data after a torn record (Phase 2.7).
std::string RandomValue(Random& random, std::uint64_t tag, bool log_fragments) {
  std::size_t size = 0;
  const std::uint64_t roll = random.Below(100);
  if (roll < 60) {
    size = random.Below(32);
  } else if (roll < 90) {
    size = 100 + random.Below(2000);
  } else {
    size = 5000 + random.Below(40000);
  }
  const char filler = static_cast<char>('a' + (tag % 26));
  std::string value = "v" + std::to_string(tag) + ":";
  if (log_fragments && random.Percent(50)) {
    for (std::uint64_t i = 1 + random.Below(4); i > 0; --i) {
      value.append(random.Below(40), filler);
      value += LogFragment(std::string(random.Below(20), filler));
    }
  }
  value.resize(std::max(size, value.size()), filler);
  return value;
}

Batch RandomBatch(Random& random, std::uint64_t tag, bool log_fragments) {
  std::size_t size = 1;
  const std::uint64_t roll = random.Below(100);
  if (roll < 5) {
    size = 0;  // an empty batch: logged, but uses no sequence numbers
  } else if (roll >= 75) {
    size = 2 + random.Below(7);
  }
  Batch batch;
  for (std::size_t i = 0; i < size; ++i) {
    Mutation mutation;
    mutation.key = "k" + std::to_string(random.Below(kKeySpace));
    mutation.is_put = random.Percent(75);
    if (mutation.is_put) {
      mutation.value = RandomValue(random, tag, log_fragments);
    }
    batch.push_back(std::move(mutation));
  }
  return batch;
}

WriteBatch ToWriteBatch(const Batch& batch) {
  WriteBatch write_batch;
  for (const Mutation& mutation : batch) {
    if (mutation.is_put) {
      write_batch.Put(mutation.key, mutation.value);
    } else {
      write_batch.Delete(mutation.key);
    }
  }
  return write_batch;
}

State ReadAll(DB& db) {
  State state;
  const std::unique_ptr<Iterator> it = db.NewIterator(ReadOptions{});
  for (it->SeekToFirst(); it->Valid(); it->Next()) {
    state.emplace(std::string(it->key()), std::string(it->value()));
  }
  EXPECT_TRUE(it->status().ok()) << it->status().ToString();
  return state;
}

// --- The torture loop
// -----------------------------------------------------------------------------

struct TortureOptions {
  FaultInjectionFileSystem::Options fs;
  std::size_t cycles = FORGEDB_CRASH_TEST_CYCLES;
  bool inject_errors = false;            // fail a random Append, Flush or Sync in most cycles
  bool log_fragments_in_values = false;  // see RandomValue
  // How the DB is opened: a tiny memtable_bytes flushes on almost every write,
  // so crashes land in flushes, MANIFEST writes and CURRENT switches.
  Options db;
};

// What the runs actually exercised: a test that passes without crashing
// anything proves nothing, so the tests check these too.
struct Stats {
  std::size_t cycles = 0;
  std::size_t power_losses = 0;
  std::size_t process_crashes = 0;
  std::size_t clean_closes = 0;
  std::size_t died_mid_operation = 0;
  std::size_t failed_writes = 0;
  std::size_t lost_unsynced_writes = 0;  // recoveries that kept fewer than all acknowledged writes
  std::size_t kept_failed_write = 0;     // recoveries that kept a write that had failed
  std::size_t torn_tails = 0;            // recoveries that ignored a torn tail in the newest log
  std::size_t dropped_valid_data = 0;    // point-in-time: valid records after a problem
  std::size_t flushes = 0;               // memtable flushes while writing

  void Print(const char* name) const {
    std::printf(  // NOLINT(cppcoreguidelines-pro-type-vararg)
        "[ stats    ] %s: %zu cycles: %zu power losses, %zu process crashes, %zu clean closes, "
        "%zu deaths mid-operation, %zu failed writes; recoveries: %zu lost unsynced writes, "
        "%zu kept a failed write, %zu ignored a torn tail, %zu dropped valid records after a "
        "problem (point-in-time); %zu flushes\n",
        name, cycles, power_losses, process_crashes, clean_closes, died_mid_operation,
        failed_writes, lost_unsynced_writes, kept_failed_write, torn_tails, dropped_valid_data,
        flushes);
  }
};

// What the next recovery must produce: the state after `start` plus the first
// j of `batches`, for some j in [lowest, highest].
struct Expectation {
  State start;
  SequenceNumber start_sequence = 0;
  std::vector<Batch> batches;
  std::size_t acknowledged = 0;  // batches[0, acknowledged) were acknowledged
  bool last_failed = false;      // batches[acknowledged] failed (and is the last one)
  std::size_t lowest = 0;
  std::size_t highest = 0;
  std::string how_it_ended;
};

class Torture {
 public:
  // `stats` (not null) collects what the run exercised.
  Torture(std::uint64_t seed, const TortureOptions& options, Stats* stats)
      : seed_(seed), options_(options), stats_(stats), random_(seed) {}

  // Runs every cycle; returns "" or a description of the first broken promise.
  std::string Run() {
    for (std::size_t cycle = 0; cycle < options_.cycles; ++cycle) {
      if (cycle % kCyclesPerDatabase == 0) {
        NewDatabase(cycle);
      }
      cycle_ = cycle;
      ++stats_->cycles;
      if (const std::string problem = OneCycle(); !problem.empty()) {
        return Report(problem);
      }
    }
    return "";
  }

 private:
  void NewDatabase(std::size_t cycle) {
    db_.reset();
    FaultInjectionFileSystem::Options fs_options = options_.fs;
    fs_options.seed = (seed_ * 1000003) + cycle;
    fs_ = std::make_shared<FaultInjectionFileSystem>(std::make_shared<MemFileSystem>(), fs_options);
    expected_ = Expectation{};
    recovered_.clear();
    recovered_sequence_ = 0;
  }

  std::string OneCycle() {
    // 1. Recover, and check the result against what the last cycle allows.
    Result<std::unique_ptr<DB>> opened = OpenDB(options_.db, "db", fs_);
    if (!opened.ok()) {
      return "open failed: " + opened.status().ToString();
    }
    db_ = std::move(opened).value();
    if (std::string problem = CheckRecovery(); !problem.empty()) {
      return problem;
    }

    // 2. Plan how this cycle ends.
    const std::size_t writes = 1 + random_.Below(kMaxWritesPerCycle);
    const std::uint64_t ending = random_.Below(100);
    const bool clean_close = ending < 10;
    const CrashKind kind = random_.Percent(75) ? CrashKind::kPowerLoss : CrashKind::kProcessCrash;
    if (ending >= 45) {
      fs_->CrashAtOperation(1 + random_.Below(4 * writes));  // ~4 file operations per write
    }
    if (options_.inject_errors && random_.Percent(70)) {
      constexpr std::array<Op, 3> kOps = {Op::kAppend, Op::kFlush, Op::kSync};
      // Two statements: the order of evaluation of function arguments is
      // unspecified, and a seed must give the same run with every compiler.
      const Op op = kOps.at(random_.Below(kOps.size()));
      fs_->FailNth(op, 1 + random_.Below(3 * writes));
    }

    // 3. Write.
    Expectation next;
    next.start = recovered_;
    next.start_sequence = recovered_sequence_;
    std::size_t acknowledged = 0;
    std::size_t synced = 0;
    bool failed = false;
    for (std::size_t i = 0; i < writes && !failed; ++i) {
      Batch batch = RandomBatch(random_, (cycle_ * 1000) + i, options_.log_fragments_in_values);
      const bool sync = random_.Percent(20);
      const Status status = db_->Write({.sync = sync}, ToWriteBatch(batch));
      next.batches.push_back(std::move(batch));
      if (status.ok()) {
        acknowledged = next.batches.size();
        synced = sync ? acknowledged : synced;
        continue;
      }
      failed = true;  // it may or may not have reached the log
      ++stats_->failed_writes;
      if (db_->Put({}, "k0", "after a failure").ok()) {
        return "a write succeeded after an earlier write failed (" + status.ToString() + ")";
      }
    }

    // 4. Before the crash, readers see exactly the acknowledged writes: not
    //    the failed one, even if part of it reached the log. Skipped if the
    //    simulated process died mid-operation: reads open table files
    //    (Phase 3), and a dead process can do nothing.
    const bool dead = fs_->ProcessIsDead();
    if (!dead) {
      State live = next.start;
      for (std::size_t i = 0; i < acknowledged; ++i) {
        ApplyTo(live, next.batches[i]);
      }
      if (ReadAll(*db_) != live) {
        return "before the crash, the DB does not show exactly the " +
               std::to_string(acknowledged) + " acknowledged writes";
      }
    }

    // 5. End the cycle.
    stats_->died_mid_operation += dead ? 1U : 0U;
    stats_->flushes += dynamic_cast<DBImpl&>(*db_).FlushesForTesting();
    next.acknowledged = acknowledged;
    next.last_failed = failed;
    next.highest = acknowledged + (failed ? 1U : 0U);
    if (clean_close && !dead) {
      ++stats_->clean_closes;
      db_.reset();
      next.lowest = acknowledged;
      next.how_it_ended = "clean close";
    } else {
      const bool crash_first = random_.Percent(50);  // with the DB still open, or after closing it
      if (!crash_first) {
        db_.reset();
      }
      if (const Status status = fs_->SimulateCrash(kind); !status.ok()) {
        return "SimulateCrash failed: " + status.ToString();
      }
      db_.reset();
      if (kind == CrashKind::kPowerLoss) {
        ++stats_->power_losses;
        next.lowest = synced;
        next.how_it_ended = "power loss";
      } else {
        ++stats_->process_crashes;
        next.lowest = acknowledged;
        next.how_it_ended = "process crash";
      }
      next.how_it_ended += dead ? " (process died mid-operation)" : " (between writes)";
    }
    fs_->ClearFaults();
    next.how_it_ended += "; " + std::to_string(acknowledged) + " of " +
                         std::to_string(next.batches.size()) + " writes acknowledged, " +
                         (synced == 0 ? std::string("none with sync")
                                      : "the last with sync is #" + std::to_string(synced));
    expected_ = std::move(next);
    return "";
  }

  std::string CheckRecovery() {
    auto& impl = dynamic_cast<DBImpl&>(*db_);
    const RecoveryInfo& info = impl.RecoveryInfoForTesting();
    const bool torn_newest = std::ranges::any_of(info.torn_tails, [&](const auto& tail) {
      return !info.logs_replayed.empty() && tail.log_number == info.logs_replayed.back();
    });
    stats_->torn_tails += torn_newest ? 1U : 0U;
    stats_->dropped_valid_data +=
        std::ranges::any_of(info.torn_tails,
                            [](const auto& tail) { return tail.valid_data_dropped; })
            ? 1U
            : 0U;
    const SequenceNumber sequence = impl.LastSequenceForTesting();
    const State actual = ReadAll(*db_);

    // Find the prefix: j writes replayed means the sequence advanced by the
    // number of operations in them (empty batches use none).
    State state = expected_.start;
    SequenceNumber at = expected_.start_sequence;
    for (std::size_t j = 0; j <= expected_.batches.size(); ++j) {
      if (j > 0) {
        ApplyTo(state, expected_.batches[j - 1]);
        at += expected_.batches[j - 1].size();
      }
      if (j < expected_.lowest || at != sequence || state != actual) {
        continue;
      }
      if (j > expected_.highest) {
        break;  // only matches beyond what was ever written: reported below
      }
      stats_->lost_unsynced_writes += j < expected_.acknowledged ? 1U : 0U;
      stats_->kept_failed_write += j > expected_.acknowledged ? 1U : 0U;
      recovered_ = actual;
      recovered_sequence_ = sequence;
      return "";
    }
    return "recovered state is not an allowed prefix: sequence " + std::to_string(sequence) + ", " +
           std::to_string(actual.size()) + " keys; allowed: writes #" +
           std::to_string(expected_.lowest) + " to #" + std::to_string(expected_.highest) +
           " after sequence " + std::to_string(expected_.start_sequence);
  }

  [[nodiscard]] std::string Report(const std::string& problem) const {
    std::string report = "Crash torture failed: seed " + std::to_string(seed_) + ", cycle " +
                         std::to_string(cycle_) + "\n  " + problem + "\n";
    report += "The previous cycle ended with: " + expected_.how_it_ended + "\n";
    report += "Its writes (#1 is the first):\n";
    for (std::size_t i = 0; i < expected_.batches.size(); ++i) {
      report += "  #" + std::to_string(i + 1) + " " + Describe(expected_.batches[i]) + "\n";
    }
    report += "Files after the crash:\n";
    for (const FaultInjectionFileSystem::CrashedFile& file : fs_->LastCrash()) {
      static constexpr std::array<const char*, 3> kOutcomes = {"unchanged", "rewritten", "removed"};
      report += "  " + file.path + ": " + kOutcomes.at(static_cast<std::size_t>(file.outcome)) +
                ", " + std::to_string(file.size) + " bytes\n";
    }
    report += "Rerun just this seed: FORGEDB_CRASH_SEED=" + std::to_string(seed_) +
              " FORGEDB_CRASH_CYCLES=" + std::to_string(options_.cycles) + "\n";
    return report;
  }

  std::uint64_t seed_;
  TortureOptions options_;
  Stats* stats_;
  Random random_;
  std::shared_ptr<FaultInjectionFileSystem> fs_;
  std::unique_ptr<DB> db_;
  Expectation expected_;
  State recovered_;
  SequenceNumber recovered_sequence_ = 0;
  std::size_t cycle_ = 0;
};

std::vector<std::uint64_t> Seeds() {
  if (GetEnv("FORGEDB_CRASH_SEED")) {
    return {GetEnvNumber("FORGEDB_CRASH_SEED", 0)};
  }
  return {1, 2, 3};
}

TortureOptions WithTail(UnsyncedTail tail) {
  TortureOptions options;
  options.fs.unsynced_tail = tail;
  options.cycles = GetEnvNumber("FORGEDB_CRASH_CYCLES", FORGEDB_CRASH_TEST_CYCLES);
  return options;
}

// Runs every seed; fails with the first report. Returns what was exercised.
Stats RunAll(const char* name, const TortureOptions& options) {
  Stats stats;
  for (const std::uint64_t seed : Seeds()) {
    Torture torture(seed, options, &stats);
    if (const std::string report = torture.Run(); !report.empty()) {
      ADD_FAILURE() << report;
      break;
    }
  }
  stats.Print(name);
  return stats;
}

// Every kind of ending happened, and recovery really had something to do.
void ExpectEverythingExercised(const Stats& stats) {
  if (stats.cycles < 100) {
    return;  // a short run proves too little to judge coverage
  }
  EXPECT_GT(stats.power_losses, 0U);
  EXPECT_GT(stats.process_crashes, 0U);
  EXPECT_GT(stats.clean_closes, 0U);
  EXPECT_GT(stats.died_mid_operation, 0U);
  EXPECT_GT(stats.lost_unsynced_writes, 0U) << "power losses lost unsynced writes";
}

TEST(CrashTortureTest, TornUnsyncedTails) {
  const Stats stats = RunAll("torn tails", WithTail(UnsyncedTail::kKeepPrefixTorn));
  ExpectEverythingExercised(stats);
  if (stats.cycles >= 100) {
    EXPECT_GT(stats.torn_tails, 0U);
  }
}

TEST(CrashTortureTest, PartlyKeptUnsyncedTails) {
  ExpectEverythingExercised(RunAll("partly kept tails", WithTail(UnsyncedTail::kKeepPrefix)));
}

TEST(CrashTortureTest, DroppedUnsyncedTails) {
  ExpectEverythingExercised(RunAll("dropped tails", WithTail(UnsyncedTail::kDrop)));
}

// Found by fuzzing (2.7): a torn record whose value contains a valid log
// fragment used to make recovery fail with Corruption. Many values here
// contain such fragments; every recovery must still succeed.
TEST(CrashTortureTest, ValuesContainingLogFragments) {
  TortureOptions options = WithTail(UnsyncedTail::kKeepPrefixTorn);
  options.log_fragments_in_values = true;
  const Stats stats = RunAll("log fragments in values", options);
  ExpectEverythingExercised(stats);
  if (stats.cycles >= 100) {
    EXPECT_GT(stats.torn_tails, 0U);
  }
}

// TODO 2.5: after a failed Append, Flush or Sync, the DB refuses every later
// write, and reopens with a correct prefix (the failed write may or may not
// be in it).
TEST(CrashTortureTest, InjectedWriteErrors) {
  TortureOptions options = WithTail(UnsyncedTail::kKeepPrefixTorn);
  options.inject_errors = true;
  const Stats stats = RunAll("injected errors", options);
  ExpectEverythingExercised(stats);
  if (stats.cycles >= 100) {
    EXPECT_GT(stats.failed_writes, 0U);
  }
}

// Pages of unsynced data written out of order (zero-filled holes followed by
// valid data). ADR 0004's strict rule refused to open about 3% of these
// (Phase 2.5); since 3.6 the newest log stops at its first problem
// (point-in-time, ADR 0004 rule 6), so every open must succeed with an
// allowed prefix.
TEST(CrashTortureTest, ZeroFilledPagesInUnsyncedTails) {
  TortureOptions options = WithTail(UnsyncedTail::kKeepPrefixTorn);
  options.fs.zero_pages_in_unsynced_tail = true;
  const Stats stats = RunAll("zero-filled pages", options);
  ExpectEverythingExercised(stats);
  if (stats.cycles >= 100) {
    EXPECT_GT(stats.dropped_valid_data, 0U) << "point-in-time recovery was exercised";
  }
}

// TODO 3.9: a small memtable flushes about every ten writes, so crashes
// (power losses undo renames and removals by default) land during flushes,
// MANIFEST writes and the CURRENT switch at every open.
TortureOptions WithFlushes() {
  TortureOptions options = WithTail(UnsyncedTail::kKeepPrefixTorn);
  options.db.memtable_bytes = std::size_t{32} << 10;
  return options;
}

TEST(CrashTortureTest, CrashesDuringFlushes) {
  const Stats stats = RunAll("flushes", WithFlushes());
  ExpectEverythingExercised(stats);
  if (stats.cycles >= 100) {
    EXPECT_GT(stats.flushes, stats.cycles) << "most cycles flushed at least once";
  }
}

TEST(CrashTortureTest, CrashesDuringFlushesWithInjectedErrors) {
  TortureOptions options = WithFlushes();
  options.inject_errors = true;
  const Stats stats = RunAll("flushes with injected errors", options);
  ExpectEverythingExercised(stats);
  if (stats.cycles >= 100) {
    EXPECT_GT(stats.failed_writes, 0U);
    EXPECT_GT(stats.flushes, 0U);
  }
}

}  // namespace
}  // namespace forgedb
