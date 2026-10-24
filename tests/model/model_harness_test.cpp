// Tests of the harness itself: that it is deterministic, that its operation
// mix covers what it claims to, and above all that it catches a buggy DB and
// shrinks the failure to a short reproduction.
#include "model/model_harness.h"

#include <cstddef>
#include <cstdint>

#include <gtest/gtest.h>

#include <memory>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "forgedb/db.h"
#include "forgedb/write_batch.h"

namespace forgedb::model {
namespace {

Result<std::unique_ptr<DB>> OpenMemoryDB() { return DB::Open(Options{}, "model-test"); }

RunOptions SeedAndOps(std::uint64_t seed, std::size_t operations) {
  RunOptions options;
  options.seed = seed;
  options.operations = operations;
  return options;
}

// A DB with a planted bug: any deletion of `ignored_key` is silently dropped.
// Every other operation is forwarded to a real DB.
class DBThatIgnoresDeletesOf final : public DB {
 public:
  DBThatIgnoresDeletesOf(std::unique_ptr<DB> real, std::string ignored_key)
      : real_(std::move(real)), ignored_key_(std::move(ignored_key)) {}

  Status Write(const WriteOptions& options, const WriteBatch& batch) override {
    // Rebuild the batch without the ignored deletions.
    class Filter final : public WriteBatch::Handler {
     public:
      explicit Filter(std::string_view ignored) : ignored_(ignored) {}
      void Put(std::string_view key, std::string_view value) override { out.Put(key, value); }
      void Delete(std::string_view key) override {
        if (key != ignored_) {
          out.Delete(key);
        }
      }
      WriteBatch out;  // NOLINT(misc-non-private-member-variables-in-classes)

     private:
      std::string_view ignored_;
    } filter(ignored_key_);
    FORGEDB_RETURN_IF_ERROR(batch.Iterate(filter));
    return real_->Write(options, filter.out);
  }
  Result<std::string> Get(const ReadOptions& options, std::string_view key) override {
    return real_->Get(options, key);
  }
  std::unique_ptr<Iterator> NewIterator(const ReadOptions& options) override {
    return real_->NewIterator(options);
  }

 private:
  std::unique_ptr<DB> real_;
  std::string ignored_key_;
};

Result<std::unique_ptr<DB>> OpenBuggyDB() {
  FORGEDB_ASSIGN_OR_RETURN(std::unique_ptr<DB> real, OpenMemoryDB());
  return std::unique_ptr<DB>(std::make_unique<DBThatIgnoresDeletesOf>(std::move(real), "k07"));
}

TEST(ModelHarnessTest, GeneratorIsDeterministic) {
  OpGenerator a(42, 1000);
  OpGenerator b(42, 1000);
  OpGenerator other(43, 1000);
  bool differs = false;
  for (int i = 0; i < 1000; ++i) {
    const Op op_a = a.Next();
    const Op op_b = b.Next();
    ASSERT_EQ(Describe(op_a), Describe(op_b)) << "operation " << i;
    differs = differs || Describe(op_a) != Describe(other.Next());
  }
  EXPECT_TRUE(differs) << "a different seed gives different operations";
}

TEST(ModelHarnessTest, GeneratorCoversEveryOperationAndReusesKeys) {
  OpGenerator generator(1, 20000);
  std::set<Op::Kind> kinds;
  std::size_t empty_batches = 0;
  std::size_t writes_to_small_keys = 0;
  std::size_t non_ascii_keys = 0;
  bool empty_key = false;
  for (int i = 0; i < 20000; ++i) {
    const Op op = generator.Next();
    kinds.insert(op.kind);
    empty_batches += (op.kind == Op::Kind::kWriteBatch && op.mutations.empty()) ? 1U : 0U;
    for (const Mutation& mutation : op.mutations) {
      empty_key = empty_key || mutation.key.empty();
      writes_to_small_keys += (mutation.key.size() == 3 && mutation.key[0] == 'k') ? 1U : 0U;
      for (const char c : mutation.key) {
        if (static_cast<unsigned char>(c) >= 0x80) {
          ++non_ascii_keys;
          break;
        }
      }
    }
  }
  EXPECT_EQ(kinds.size(), 6U) << "Put, Delete, WriteBatch, Get, Scan and Seek all appear";
  EXPECT_GT(empty_batches, 0U);
  EXPECT_TRUE(empty_key);
  EXPECT_GT(writes_to_small_keys, 5000U) << "most writes hit the small key set: overwrites";
  EXPECT_GT(non_ascii_keys, 0U);
}

TEST(ModelHarnessTest, QuoteEscapesBytes) {
  EXPECT_EQ(Quote("abc"), "\"abc\"");
  EXPECT_EQ(Quote(std::string("a\x00"
                              "1",
                              3)),
            "\"a\\x00\" \"1\"");
  EXPECT_EQ(Quote("q\"b\\"), "\"q\\\"b\\\\\"");
}

TEST(ModelHarnessTest, AgreesWithACorrectDB) {
  EXPECT_EQ(RunModelTest(OpenMemoryDB, SeedAndOps(5, 20000)), "");
}

TEST(ModelHarnessTest, CatchesABugAndShrinksItToAMinimalReproduction) {
  const std::string report = RunModelTest(OpenBuggyDB, SeedAndOps(5, 20000));
  ASSERT_NE(report, "") << "the planted bug must be found";
  SCOPED_TRACE(report);
  EXPECT_NE(report.find("seed 5"), std::string::npos);
  EXPECT_NE(report.find("FORGEDB_MODEL_SEED=5"), std::string::npos);
  EXPECT_EQ(report.find("may not be minimal"), std::string::npos);

  // The shortest failure is: write k07, delete it, read it. Exactly which
  // operations remain depends on the seed, but there are at most three.
  const std::size_t start = report.find("Minimal reproduction (");
  ASSERT_NE(start, std::string::npos);
  const std::size_t count =
      std::stoul(report.substr(start + std::string("Minimal reproduction (").size()));
  EXPECT_GE(count, 2U);
  EXPECT_LE(count, 3U);
  EXPECT_NE(report.find("k07", start), std::string::npos);
}

TEST(ModelHarnessTest, ShrinkResultStillFailsAndIsOneMinimal) {
  // Find a failing prefix directly, then check Shrink's two promises.
  OpGenerator generator(9, 20000);
  std::vector<Op> ops;
  std::optional<Failure> failure;
  {
    const std::unique_ptr<DB> db = std::move(OpenBuggyDB()).value();
    Model model;
    while (!failure && ops.size() < 20000) {
      ops.push_back(generator.Next());
      if (std::optional<std::string> problem = Apply(*db, model, ops.back())) {
        failure = Failure{.op_index = ops.size() - 1, .message = *problem};
      }
    }
  }
  ASSERT_TRUE(failure.has_value());

  const ShrinkResult shrunk = Shrink(OpenBuggyDB, ops);
  ASSERT_FALSE(shrunk.ops.empty());
  EXPECT_FALSE(shrunk.stopped_early);
  const std::optional<Failure> replayed = Replay(OpenBuggyDB, shrunk.ops);
  if (!replayed.has_value()) {
    FAIL() << "the reproduction must still fail";
  }
  EXPECT_EQ(replayed->op_index, shrunk.ops.size() - 1) << "at its last operation";
  EXPECT_FALSE(Replay(OpenMemoryDB, shrunk.ops).has_value()) << "and a correct DB passes it";
  for (std::size_t i = 0; i + 1 < shrunk.ops.size(); ++i) {  // 1-minimal
    std::vector<Op> without = shrunk.ops;
    without.erase(without.begin() + static_cast<std::ptrdiff_t>(i));
    EXPECT_FALSE(Replay(OpenBuggyDB, without).has_value()) << "operation " << i << " is needed";
  }
}

}  // namespace
}  // namespace forgedb::model
