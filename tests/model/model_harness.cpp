#include "model/model_harness.h"

#include <algorithm>
#include <array>
#include <utility>

#include "forgedb/iterator.h"
#include "forgedb/options.h"
#include "forgedb/status.h"
#include "forgedb/write_batch.h"

namespace forgedb::model {

// --- Describing operations ----------------------------------------------------------

std::string Quote(std::string_view bytes) {
  static constexpr std::array<char, 16> kHex = {'0', '1', '2', '3', '4', '5', '6', '7',
                                                '8', '9', 'a', 'b', 'c', 'd', 'e', 'f'};
  const auto is_hex_digit = [](char c) {
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
  };
  std::string quoted = "\"";
  for (std::size_t i = 0; i < bytes.size(); ++i) {
    const char c = bytes[i];
    const auto byte = static_cast<unsigned char>(c);
    if (c == '"' || c == '\\') {
      quoted.push_back('\\');
      quoted.push_back(c);
    } else if (byte >= 0x20 && byte < 0x7F) {
      quoted.push_back(c);
    } else {
      quoted.append("\\x");
      quoted.push_back(kHex[byte >> 4U]);
      quoted.push_back(kHex[byte & 0xFU]);
      // A hex escape swallows every hex digit after it ("\x012" is one
      // byte), so split the literal if one follows: "\x01" "2".
      if (i + 1 < bytes.size() && is_hex_digit(bytes[i + 1])) {
        quoted.append("\" \"");
      }
    }
  }
  quoted.push_back('"');
  return quoted;
}

namespace {

std::string DescribeMutation(const Mutation& mutation) {
  return mutation.kind == Mutation::Kind::kPut
             ? "Put(" + Quote(mutation.key) + ", " + Quote(mutation.value) + ")"
             : "Delete(" + Quote(mutation.key) + ")";
}

}  // namespace

std::string Describe(const Op& op) {
  switch (op.kind) {
    case Op::Kind::kPut:
    case Op::Kind::kDelete:
      return DescribeMutation(op.mutations.front());
    case Op::Kind::kWriteBatch: {
      std::string text = "Write({";
      for (std::size_t i = 0; i < op.mutations.size(); ++i) {
        text += (i == 0 ? "" : ", ") + DescribeMutation(op.mutations[i]);
      }
      return text + "})";
    }
    case Op::Kind::kGet:
      return "Get(" + Quote(op.key) + ")";
    case Op::Kind::kScan:
      return "Scan()";
    case Op::Kind::kSeek:
      return "Seek(" + Quote(op.key) + ") + " + std::to_string(op.next_count) + " x Next()";
    case Op::Kind::kReopen:
      return "Reopen()";
  }
  return "?";
}

// --- Generating operations ----------------------------------------------------------

OpGenerator::OpGenerator(std::uint64_t seed, std::size_t total_operations, GeneratorOptions options)
    : options_(options),
      scan_probability_(
          std::min(0.01, static_cast<double>(options.expected_scans) /
                             static_cast<double>(std::max<std::size_t>(total_operations, 1)))),
      reopen_probability_(
          std::min(0.01, static_cast<double>(options.expected_reopens) /
                             static_cast<double>(std::max<std::size_t>(total_operations, 1)))),
      rng_(seed) {}

bool OpGenerator::Chance(double probability) {
  // Not std::uniform_real_distribution: its algorithm differs between
  // standard libraries, and a seed must mean the same operations everywhere.
  // mt19937_64 itself is fully specified; its top 53 bits make a double in [0, 1).
  const double uniform = static_cast<double>(rng_() >> 11U) * 0x1.0p-53;
  return uniform < probability;
}

std::string OpGenerator::RandomKey() {
  if (Chance(options_.small_key_fraction)) {
    const std::size_t index = rng_() % options_.small_key_space;
    if (index == 0) {
      return "";
    }
    return (index < 10 ? "k0" : "k") + std::to_string(index);
  }
  if (!recent_large_.empty() && Chance(0.5)) {
    return recent_large_[rng_() % recent_large_.size()];
  }
  std::string key((rng_() % options_.max_large_key_length) + 1, '\0');
  for (char& c : key) {
    c = Chance(0.1) ? static_cast<char>(rng_()) : static_cast<char>('a' + (rng_() % 26));
  }
  constexpr std::size_t kRecentLargeKeys = 64;
  if (recent_large_.size() < kRecentLargeKeys) {
    recent_large_.push_back(key);
  } else {
    recent_large_[recent_next_] = key;
    recent_next_ = (recent_next_ + 1) % kRecentLargeKeys;
  }
  return key;
}

std::string OpGenerator::RandomValue() {
  // "v<number>" identifies the write in a reproduction; padding varies sizes.
  std::string value = "v" + std::to_string(++counter_);
  const std::size_t length = rng_() % (options_.max_value_length + 1);
  while (value.size() < length) {
    value.push_back(Chance(0.05) ? static_cast<char>(rng_()) : '.');
  }
  return value;
}

Mutation OpGenerator::RandomMutation() {
  if (Chance(0.7)) {
    std::string key = RandomKey();
    return {.kind = Mutation::Kind::kPut, .key = std::move(key), .value = RandomValue()};
  }
  return {.kind = Mutation::Kind::kDelete, .key = RandomKey(), .value = {}};
}

Op OpGenerator::Next() {
  Op op;
  if (Chance(scan_probability_)) {
    op.kind = Op::Kind::kScan;
    return op;
  }
  if (reopen_probability_ > 0 && Chance(reopen_probability_)) {
    op.kind = Op::Kind::kReopen;
    return op;
  }
  // Put 35%, Delete 15%, WriteBatch 10%, Get 30%, Seek 10%.
  const std::uint64_t roll = rng_() % 100;
  if (roll < 35) {
    op.kind = Op::Kind::kPut;
    std::string key = RandomKey();
    op.mutations.push_back(
        {.kind = Mutation::Kind::kPut, .key = std::move(key), .value = RandomValue()});
  } else if (roll < 50) {
    op.kind = Op::Kind::kDelete;
    op.mutations.push_back({.kind = Mutation::Kind::kDelete, .key = RandomKey(), .value = {}});
  } else if (roll < 60) {
    op.kind = Op::Kind::kWriteBatch;
    const std::size_t size = rng_() % (options_.max_batch_size + 1);  // may be empty
    for (std::size_t i = 0; i < size; ++i) {
      op.mutations.push_back(RandomMutation());
    }
  } else if (roll < 90) {
    op.kind = Op::Kind::kGet;
    op.key = RandomKey();
  } else {
    op.kind = Op::Kind::kSeek;
    op.key = RandomKey();
    if (Chance(0.3)) {
      op.key.push_back('~');  // between stored keys
    }
    op.next_count = rng_() % (options_.max_next_count + 1);
  }
  return op;
}

// --- Applying and checking -----------------------------------------------------------

namespace {

std::string Show(const std::optional<std::pair<std::string_view, std::string_view>>& entry) {
  return entry ? Quote(entry->first) + "=" + Quote(entry->second) : "end";
}

// Steps an iterator and the model side by side for `steps` positions (the
// current one plus steps - 1 Nexts), or to the end of both.
// `expected` is taken by value because it is advanced; `model_end` by
// reference: with MSVC's checked iterators, copying one is not free.
std::optional<std::string> CompareRange(Iterator& it, Model::const_iterator expected,
                                        const Model::const_iterator& model_end, std::size_t steps,
                                        const std::string& what) {
  for (std::size_t position = 0; position < steps; ++position) {
    std::optional<std::pair<std::string_view, std::string_view>> got;
    std::optional<std::pair<std::string_view, std::string_view>> want;
    if (it.Valid()) {
      got.emplace(it.key(), it.value());
    }
    if (expected != model_end) {
      want.emplace(expected->first, expected->second);
    }
    if (got != want) {
      return what + ": at position " + std::to_string(position) + " expected " + Show(want) +
             ", got " + Show(got);
    }
    if (!got) {
      break;  // both at the end
    }
    it.Next();
    ++expected;
  }
  if (!it.status().ok()) {
    return what + ": iterator status " + it.status().ToString();
  }
  return std::nullopt;
}

std::optional<std::string> CheckWrite(const Status& status, const Op& op) {
  if (!status.ok()) {
    return Describe(op) + " returned " + status.ToString();
  }
  return std::nullopt;
}

void ApplyToModel(Model& model, const Mutation& mutation) {
  if (mutation.kind == Mutation::Kind::kPut) {
    model[mutation.key] = mutation.value;
  } else {
    model.erase(mutation.key);
  }
}

}  // namespace

std::optional<std::string> Apply(std::unique_ptr<DB>& db_ref, const DbSetup& setup, Model& model,
                                 const Op& op) {
  if (op.kind == Op::Kind::kReopen) {
    if (!setup.reopen) {
      return std::string("Reopen() but the DbSetup has no reopen function");
    }
    db_ref.reset();  // closes the database (and releases its LOCK)
    Result<std::unique_ptr<DB>> reopened = setup.reopen();
    if (!reopened.ok()) {
      return "Reopen() failed: " + reopened.status().ToString();
    }
    db_ref = std::move(reopened).value();
    return std::nullopt;  // whether recovery kept everything shows in later reads
  }
  DB& db = *db_ref;
  switch (op.kind) {
    case Op::Kind::kPut: {
      const Mutation& put = op.mutations.front();
      ApplyToModel(model, put);
      return CheckWrite(db.Put(WriteOptions{}, put.key, put.value), op);
    }
    case Op::Kind::kDelete: {
      const Mutation& deletion = op.mutations.front();
      ApplyToModel(model, deletion);
      return CheckWrite(db.Delete(WriteOptions{}, deletion.key), op);
    }
    case Op::Kind::kWriteBatch: {
      WriteBatch batch;
      for (const Mutation& mutation : op.mutations) {
        if (mutation.kind == Mutation::Kind::kPut) {
          batch.Put(mutation.key, mutation.value);
        } else {
          batch.Delete(mutation.key);
        }
        ApplyToModel(model, mutation);
      }
      return CheckWrite(db.Write(WriteOptions{}, batch), op);
    }
    case Op::Kind::kGet: {
      const Result<std::string> got = db.Get(ReadOptions{}, op.key);
      const auto expected = model.find(op.key);
      const std::string want = expected == model.end() ? "NotFound" : Quote(expected->second);
      std::string have;
      if (got.ok()) {
        have = Quote(*got);
      } else if (got.status().code() == Status::Code::kNotFound) {
        have = "NotFound";
      } else {
        have = "error " + got.status().ToString();
      }
      if (have != want) {
        return Describe(op) + ": expected " + want + ", got " + have;
      }
      return std::nullopt;
    }
    case Op::Kind::kScan: {
      const std::unique_ptr<Iterator> it = db.NewIterator(ReadOptions{});
      it->SeekToFirst();
      return CompareRange(*it, model.cbegin(), model.cend(), model.size() + 1, Describe(op));
    }
    case Op::Kind::kSeek: {
      const std::unique_ptr<Iterator> it = db.NewIterator(ReadOptions{});
      it->Seek(op.key);
      return CompareRange(*it, model.lower_bound(op.key), model.cend(), op.next_count + 1,
                          Describe(op));
    }
    case Op::Kind::kReopen:
      break;  // handled above
  }
  return "unknown operation kind";
}

std::optional<Failure> Replay(const DbSetup& setup, std::span<const Op> ops) {
  Result<std::unique_ptr<DB>> opened = setup.open();
  if (!opened.ok()) {
    return Failure{.op_index = 0,
                   .message = "opening the DB failed: " + opened.status().ToString()};
  }
  std::unique_ptr<DB> db = std::move(opened).value();
  Model model;
  for (std::size_t i = 0; i < ops.size(); ++i) {
    if (std::optional<std::string> problem = Apply(db, setup, model, ops[i])) {
      return Failure{.op_index = i, .message = std::move(*problem)};
    }
  }
  return std::nullopt;
}

// --- Shrinking ------------------------------------------------------------------------

namespace {

class Shrinker {
 public:
  Shrinker(const DbSetup& setup, const ShrinkLimits& limits)
      : setup_(&setup), limits_(limits), start_(std::chrono::steady_clock::now()) {}

  // Replays `candidate`; if it fails, cuts it off just after the failing
  // operation, makes it the current best, and returns true.
  bool TryAdopt(std::vector<Op> candidate) {
    if (OutOfBudget()) {
      return false;
    }
    ++result_.replays;
    std::optional<Failure> failure = Replay(*setup_, candidate);
    if (!failure) {
      return false;
    }
    candidate.resize(failure->op_index + 1);
    result_.ops = std::move(candidate);
    result_.failure = std::move(*failure);
    return true;
  }

  bool OutOfBudget() {
    if (result_.replays >= limits_.max_replays ||
        std::chrono::steady_clock::now() - start_ >= limits_.time_limit) {
      result_.stopped_early = true;
    }
    return result_.stopped_early;
  }

  ShrinkResult& Best() { return result_; }

 private:
  const DbSetup* setup_;
  ShrinkLimits limits_;
  std::chrono::steady_clock::time_point start_;
  ShrinkResult result_;
};

// All operations except the half-open range [begin, end) of the leading
// (non-final) ones; the final, failing operation is always kept.
std::vector<Op> Without(const std::vector<Op>& ops, std::size_t begin, std::size_t end) {
  std::vector<Op> kept;
  kept.reserve(ops.size() - (end - begin));
  for (std::size_t i = 0; i < ops.size(); ++i) {
    if (i < begin || i >= end) {
      kept.push_back(ops[i]);
    }
  }
  return kept;
}

}  // namespace

ShrinkResult Shrink(const DbSetup& setup, std::vector<Op> failing, const ShrinkLimits& limits) {
  Shrinker shrinker(setup, limits);
  if (!shrinker.TryAdopt(std::move(failing))) {
    // Not reproducible (or no budget): report what we were given.
    return std::move(shrinker.Best());
  }
  ShrinkResult& best = shrinker.Best();

  // 1. Reads before the failing operation change nothing; drop them all.
  //    (Writes and reopens are kept here; ddmin below tries removing them.)
  {
    std::vector<Op> kept;
    for (std::size_t i = 0; i + 1 < best.ops.size(); ++i) {
      if (best.ops[i].ChangesState()) {
        kept.push_back(best.ops[i]);
      }
    }
    kept.push_back(best.ops.back());
    if (kept.size() < best.ops.size()) {
      static_cast<void>(shrinker.TryAdopt(std::move(kept)));
    }
  }

  // 2. Delta debugging (ddmin) over the operations before the failing one:
  //    try removing chunks, from halves down to single operations.
  std::size_t chunks = 2;
  while (best.ops.size() > 1 && !shrinker.OutOfBudget()) {
    const std::size_t leading = best.ops.size() - 1;
    chunks = std::min(chunks, leading);
    const std::size_t chunk_size = (leading + chunks - 1) / chunks;
    bool removed = false;
    for (std::size_t begin = 0; begin < leading && !removed; begin += chunk_size) {
      removed = shrinker.TryAdopt(Without(best.ops, begin, std::min(begin + chunk_size, leading)));
    }
    if (removed) {
      chunks = std::max<std::size_t>(chunks - 1, 2);
    } else if (chunk_size == 1) {
      break;  // no single operation can be removed: 1-minimal
    } else {
      chunks *= 2;
    }
  }

  // 3. Inside batches, remove individual operations. (A successful attempt
  //    can also shorten best.ops, hence the size checks on every step.)
  for (std::size_t i = 0; i < best.ops.size(); ++i) {
    std::size_t m = 0;
    while (i < best.ops.size() && best.ops[i].kind == Op::Kind::kWriteBatch &&
           m < best.ops[i].mutations.size() && !shrinker.OutOfBudget()) {
      std::vector<Op> candidate = best.ops;
      candidate[i].mutations.erase(candidate[i].mutations.begin() + static_cast<std::ptrdiff_t>(m));
      if (!shrinker.TryAdopt(std::move(candidate))) {
        ++m;  // needed; keep it and try the next one
      }
    }
  }
  return std::move(best);
}

// --- Running ----------------------------------------------------------------------------

std::string RunModelTest(const DbSetup& setup, const RunOptions& options) {
  std::optional<Failure> failure;
  {
    Result<std::unique_ptr<DB>> opened = setup.open();
    if (!opened.ok()) {
      return "opening the DB failed: " + opened.status().ToString();
    }
    std::unique_ptr<DB> db = std::move(opened).value();
    OpGenerator generator(options.seed, options.operations, options.generator);
    Model model;
    for (std::size_t i = 0; i < options.operations && !failure; ++i) {
      if (std::optional<std::string> problem = Apply(db, setup, model, generator.Next())) {
        failure = Failure{.op_index = i, .message = std::move(*problem)};
      }
    }
  }  // the DB is closed before shrinking opens new ones
  if (!failure) {
    return "";
  }

  // Regenerate the operations up to the failure (the generator is
  // deterministic) instead of keeping all of them in memory during the run.
  std::vector<Op> ops;
  ops.reserve(failure->op_index + 1);
  OpGenerator generator(options.seed, options.operations, options.generator);
  for (std::size_t i = 0; i <= failure->op_index; ++i) {
    ops.push_back(generator.Next());
  }
  const std::size_t original_length = ops.size();
  const ShrinkResult shrunk = Shrink(setup, std::move(ops), options.shrink);

  std::string report = "Model test failed: seed " + std::to_string(options.seed) + ", operation " +
                       std::to_string(failure->op_index) + " of " +
                       std::to_string(options.operations) + "\n  " + failure->message + "\n";
  if (shrunk.ops.empty()) {
    report += "The failure did not reproduce on replay (flaky?), so there is no reproduction.\n";
  } else {
    report += "Minimal reproduction (" + std::to_string(shrunk.ops.size()) + " of " +
              std::to_string(original_length) + " operations, " + std::to_string(shrunk.replays) +
              " replays" +
              (shrunk.stopped_early ? ", stopped at the shrink limit: may not be minimal" : "") +
              "):\n";
    for (const Op& op : shrunk.ops) {
      report += "  " + Describe(op) + "\n";
    }
    report += "  -> " + shrunk.failure.message + "\n";
  }
  report += "Rerun just this seed: FORGEDB_MODEL_SEED=" + std::to_string(options.seed) +
            " FORGEDB_MODEL_OPS=" + std::to_string(options.operations) + "\n";
  return report;
}

}  // namespace forgedb::model
