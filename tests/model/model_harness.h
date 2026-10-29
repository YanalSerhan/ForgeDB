// Model test harness: drives a DB with a seeded random stream of operations
// and checks every read against a reference model (std::map), which is
// obviously correct. Reused by every phase; only the DB setup changes (Phase 2
// adds reopening after a crash, Phase 4 snapshots, and so on).
//
// On a mismatch it reports the seed, the failing operation, and a minimal
// reproduction: it regenerates the operations from the seed, then shrinks
// them (drops reads, then delta-debugging over the writes, then individual
// operations inside batches) as long as the shorter sequence still fails.
//
//   model::RunOptions options;
//   options.seed = 7;
//   EXPECT_EQ(model::RunModelTest(model::DbSetup(open), options), "");  // "" = agreed throughout
#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <random>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "forgedb/db.h"
#include "forgedb/result.h"

namespace forgedb::model {

// One write inside an operation.
struct Mutation {
  enum class Kind : std::uint8_t { kPut, kDelete };
  Kind kind = Kind::kPut;
  std::string key;
  std::string value;  // kPut only
};

struct Op {
  enum class Kind : std::uint8_t {
    kPut,         // DB::Put; mutations has one kPut
    kDelete,      // DB::Delete; mutations has one kDelete
    kWriteBatch,  // DB::Write; mutations has zero or more
    kGet,         // DB::Get(key)
    kScan,        // a full scan with a new iterator
    kSeek,        // a new iterator, Seek(key), then next_count x Next
    kReopen,      // close the DB and open it again on the same storage
    // Snapshots (Phase 4.1). Each of a few slots holds a snapshot and a copy
    // of the model taken with it; reads at an empty slot do nothing, so
    // shrinking may drop any of these. A reopen empties every slot.
    kTakeSnapshot,     // slot = DB::GetSnapshot()
    kReleaseSnapshot,  // drop the slot's snapshot
    kGetAtSnapshot,    // DB::Get(key) at the slot's snapshot
    kSeekAtSnapshot,   // like kSeek, at the slot's snapshot
  };
  Kind kind = Kind::kGet;
  std::vector<Mutation> mutations;
  std::string key;
  std::size_t next_count = 0;
  std::size_t slot = 0;  // snapshot operations

  // Whether the operation can change what later reads see (a write, a
  // reopen, which can lose data if recovery is wrong, or a snapshot taken or
  // released). Shrinking keeps these.
  [[nodiscard]] bool ChangesState() const noexcept {
    return kind == Kind::kPut || kind == Kind::kDelete || kind == Kind::kWriteBatch ||
           kind == Kind::kReopen || kind == Kind::kTakeSnapshot || kind == Kind::kReleaseSnapshot;
  }
};

// Bytes as a C++ string literal: printable ASCII as is, others as \xHH.
[[nodiscard]] std::string Quote(std::string_view bytes);

// An operation as a line of pseudo-code, e.g. `Put("k07", "v12")`.
[[nodiscard]] std::string Describe(const Op& op);

struct GeneratorOptions {
  // Most keys come from a small fixed set, so keys are overwritten and
  // deleted over and over; key 0 of the set is the empty key.
  std::size_t small_key_space = 32;
  double small_key_fraction = 0.7;
  // The rest are random keys of 1..max_large_key_length bytes, about 10% of
  // them non-ASCII bytes, or a recently written one.
  std::size_t max_large_key_length = 16;
  std::size_t max_value_length = 48;
  std::size_t max_batch_size = 8;
  std::size_t max_next_count = 20;
  // A full scan costs O(DB size), so scans are rationed per run instead of
  // being a fixed share of operations.
  std::size_t expected_scans = 20;
  // Reopens per run (each replays the whole log, so they are rationed too).
  // 0 means none; set it only for a DbSetup with a reopen function.
  std::size_t expected_reopens = 0;
  // Snapshot slots; 0 means no snapshot operations (and the same operations
  // as before they existed, for a given seed). Taking a snapshot copies the
  // model, so snapshots taken (and released) per run are rationed too; with
  // slots, a third of Gets and Seeks read at a snapshot.
  std::size_t snapshot_slots = 0;
  std::size_t expected_snapshots = 0;
};

// A deterministic stream of operations: the same seed, operation count and
// options always produce the same operations.
class OpGenerator {
 public:
  OpGenerator(std::uint64_t seed, std::size_t total_operations, GeneratorOptions options = {});

  [[nodiscard]] Op Next();

 private:
  [[nodiscard]] std::string RandomKey();
  [[nodiscard]] std::string RandomValue();
  [[nodiscard]] Mutation RandomMutation();
  [[nodiscard]] bool Chance(double probability);

  GeneratorOptions options_;
  double scan_probability_;
  double reopen_probability_;
  double snapshot_probability_;
  std::mt19937_64 rng_;
  std::uint64_t counter_ = 0;              // numbers values, so reproductions are readable
  std::vector<std::string> recent_large_;  // so large keys get read and deleted too
  std::size_t recent_next_ = 0;
};

using Model = std::map<std::string, std::string>;
using DbOpener = std::function<Result<std::unique_ptr<DB>>()>;

// The snapshot slots: a snapshot and the model as it was when it was taken.
struct SavedSnapshot {
  std::shared_ptr<const Snapshot> snapshot;
  Model model;
};
using Snapshots = std::vector<std::optional<SavedSnapshot>>;

// How the harness gets databases. `open` starts over on empty storage (every
// run and every replay calls it first); `reopen`, if set, opens the storage
// the last `open` created again. The harness destroys the old DB first.
struct DbSetup {
  explicit DbSetup(DbOpener open_function, DbOpener reopen_function = nullptr)
      : open(std::move(open_function)), reopen(std::move(reopen_function)) {}
  DbOpener open;
  DbOpener reopen;
};

// Applies `op` to both `db` and `model` (a kReopen replaces `db` and empties
// `snapshots`). For reads, compares the DB's answer with the model's (or the
// snapshot's saved model). Returns a description of the problem: a mismatch,
// or a DB call that returned an error.
[[nodiscard]] std::optional<std::string> Apply(std::unique_ptr<DB>& db, const DbSetup& setup,
                                               Model& model, Snapshots& snapshots, const Op& op);

struct Failure {
  std::size_t op_index = 0;
  std::string message;
};

// Runs `ops` from the start on a new DB from setup.open and a new model.
// The first failure, if any.
[[nodiscard]] std::optional<Failure> Replay(const DbSetup& setup, std::span<const Op> ops);

struct ShrinkLimits {
  std::size_t max_replays = 2000;
  std::chrono::seconds time_limit{60};
};

struct ShrinkResult {
  std::vector<Op> ops;  // still failing; its last operation fails
  Failure failure;
  std::size_t replays = 0;
  bool stopped_early = false;  // a limit was hit; the result may not be minimal
};

// Shrinks `failing` (whose replay fails) to a short sequence that still fails.
[[nodiscard]] ShrinkResult Shrink(const DbSetup& setup, std::vector<Op> failing,
                                  const ShrinkLimits& limits = {});

struct RunOptions {
  std::uint64_t seed = 1;
  std::size_t operations = 100000;
  GeneratorOptions generator;
  ShrinkLimits shrink;
};

// Runs a model test. Returns "" if the DB agreed with the model on every
// read; otherwise a report with the seed, the failing operation and a
// minimal reproduction.
[[nodiscard]] std::string RunModelTest(const DbSetup& setup, const RunOptions& options);

}  // namespace forgedb::model
