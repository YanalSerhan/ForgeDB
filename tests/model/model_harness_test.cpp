// Tests of the harness itself: that it is deterministic, that its operation
// mix covers what it claims to, and above all that it catches a buggy DB and
// shrinks the failure to a short reproduction.
#include "model/model_harness.h"

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <functional>

#include <gtest/gtest.h>

#include <memory>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "forgedb/db.h"
#include "forgedb/write_batch.h"

#include "db/db_impl.h"
#include "db/filename.h"
#include "env/mem_file_system.h"

namespace forgedb::model {
namespace {

// A new database on its own in-memory file system.
Result<std::unique_ptr<DB>> OpenMemoryDB() {
  return OpenDB(Options{}, "model-test", std::make_shared<MemFileSystem>());
}

RunOptions SeedAndOps(std::uint64_t seed, std::size_t operations,
                      std::size_t expected_reopens = 0) {
  RunOptions options;
  options.seed = seed;
  options.operations = operations;
  options.generator.expected_reopens = expected_reopens;
  return options;
}

constexpr std::string_view kDbName = "model-test";

// A DbSetup on in-memory storage that `reopen` opens again. `before_reopen`,
// if set, runs on the storage between the close and the open, and
// `reopen_count`, if set, counts the reopens.
DbSetup ReopenableMemoryDB(std::function<void(MemFileSystem&)> before_reopen = nullptr,
                           std::size_t* reopen_count = nullptr) {
  const auto storage = std::make_shared<std::shared_ptr<MemFileSystem>>();
  return DbSetup(
      [storage] {
        *storage = std::make_shared<MemFileSystem>();  // fresh storage for every run/replay
        return OpenDB(Options{}, std::filesystem::path(kDbName), *storage);
      },
      [storage, before_reopen = std::move(before_reopen), reopen_count] {
        if (before_reopen) {
          before_reopen(**storage);
        }
        if (reopen_count != nullptr) {
          ++*reopen_count;
        }
        return OpenDB(Options{}, std::filesystem::path(kDbName), *storage);
      });
}

// A planted recovery bug: the newest log loses its last byte at every reopen,
// so its last record is torn and recovery (correctly) drops it. A closed
// database must keep every write, so the harness must notice.
void DropTheLastByteOfTheNewestLog(MemFileSystem& fs) {
  const std::filesystem::path db(kDbName);
  std::optional<std::uint64_t> newest;
  for (const std::string& name : std::move(fs.GetChildren(db)).value()) {
    const std::optional<ParsedFileName> parsed = ParseFileName(name);
    if (parsed && parsed->type == FileType::kLog && (!newest || parsed->number > *newest)) {
      newest = parsed->number;
    }
  }
  if (!newest) {
    return;
  }
  const std::filesystem::path path = LogFileName(db, *newest);
  std::string contents;
  {
    const std::unique_ptr<SequentialFile> file = std::move(fs.NewSequentialFile(path)).value();
    std::string buffer(4096, '\0');
    std::size_t read = 0;
    while ((read = std::move(file->Read(std::span<char>(buffer))).value()) > 0) {
      contents.append(buffer, 0, read);
    }
  }
  if (contents.empty()) {
    return;
  }
  contents.pop_back();
  const std::unique_ptr<WritableFile> file = std::move(fs.NewWritableFile(path)).value();
  if (!file->Append(contents).ok() || !file->Close().ok()) {
    std::abort();  // MemFileSystem does not fail these
  }
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
  std::shared_ptr<const Snapshot> GetSnapshot() override { return real_->GetSnapshot(); }
  DBStats GetStats() override { return real_->GetStats(); }

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
  EXPECT_FALSE(kinds.contains(Op::Kind::kReopen)) << "no reopens unless asked for";
  EXPECT_GT(empty_batches, 0U);
  EXPECT_TRUE(empty_key);
  EXPECT_GT(writes_to_small_keys, 5000U) << "most writes hit the small key set: overwrites";
  EXPECT_GT(non_ascii_keys, 0U);
}

TEST(ModelHarnessTest, GeneratorReopensAboutAsOftenAsAsked) {
  GeneratorOptions options;
  options.expected_reopens = 40;
  OpGenerator generator(3, 20000, options);
  std::size_t reopens = 0;
  for (int i = 0; i < 20000; ++i) {
    reopens += generator.Next().kind == Op::Kind::kReopen ? 1U : 0U;
  }
  EXPECT_GE(reopens, 20U);
  EXPECT_LE(reopens, 60U);
}

TEST(ModelHarnessTest, GeneratorMakesSnapshotOperationsOnlyWhenAsked) {
  GeneratorOptions options;
  options.snapshot_slots = 3;
  options.expected_snapshots = 100;
  OpGenerator with(4, 20000, options);
  OpGenerator without(4, 20000);
  std::set<Op::Kind> kinds;
  std::size_t takes = 0;
  for (int i = 0; i < 20000; ++i) {
    const Op op = with.Next();
    kinds.insert(op.kind);
    takes += op.kind == Op::Kind::kTakeSnapshot ? 1U : 0U;
    EXPECT_LT(op.slot, 3U);
    const Op::Kind plain = without.Next().kind;
    EXPECT_NE(plain, Op::Kind::kTakeSnapshot);
    EXPECT_NE(plain, Op::Kind::kGetAtSnapshot);
  }
  EXPECT_TRUE(kinds.contains(Op::Kind::kTakeSnapshot));
  EXPECT_TRUE(kinds.contains(Op::Kind::kReleaseSnapshot));
  EXPECT_TRUE(kinds.contains(Op::Kind::kGetAtSnapshot));
  EXPECT_TRUE(kinds.contains(Op::Kind::kSeekAtSnapshot));
  EXPECT_GE(takes, 60U) << "about 100";
  EXPECT_LE(takes, 140U);
}

TEST(ModelHarnessTest, AgreesWithACorrectDBAtSnapshots) {
  RunOptions options = SeedAndOps(6, 20000, 10);
  options.generator.snapshot_slots = 3;
  options.generator.expected_snapshots = 100;
  EXPECT_EQ(RunModelTest(ReopenableMemoryDB(), options), "");
}

// A planted snapshot bug: reads at a snapshot ignore it and read the latest
// data. The harness must notice and shrink it to: take, write, read at it.
class DBThatIgnoresSnapshots final : public DB {
 public:
  explicit DBThatIgnoresSnapshots(std::unique_ptr<DB> real) : real_(std::move(real)) {}
  Status Write(const WriteOptions& options, const WriteBatch& batch) override {
    return real_->Write(options, batch);
  }
  Result<std::string> Get(const ReadOptions& /*options*/, std::string_view key) override {
    return real_->Get({}, key);
  }
  std::unique_ptr<Iterator> NewIterator(const ReadOptions& /*options*/) override {
    return real_->NewIterator({});
  }
  std::shared_ptr<const Snapshot> GetSnapshot() override { return real_->GetSnapshot(); }
  DBStats GetStats() override { return real_->GetStats(); }

 private:
  std::unique_ptr<DB> real_;
};

TEST(ModelHarnessTest, CatchesASnapshotBugAndShrinksIt) {
  RunOptions options = SeedAndOps(5, 20000);
  options.generator.snapshot_slots = 3;
  options.generator.expected_snapshots = 100;
  const std::string report = RunModelTest(
      DbSetup([]() -> Result<std::unique_ptr<DB>> {
        FORGEDB_ASSIGN_OR_RETURN(std::unique_ptr<DB> real, OpenMemoryDB());
        return std::unique_ptr<DB>(std::make_unique<DBThatIgnoresSnapshots>(std::move(real)));
      }),
      options);
  ASSERT_NE(report, "") << "the snapshot bug must be found";
  SCOPED_TRACE(report);
  EXPECT_EQ(report.find("may not be minimal"), std::string::npos);
  const std::size_t start = report.find("Minimal reproduction (");
  ASSERT_NE(start, std::string::npos);
  const std::size_t count =
      std::stoul(report.substr(start + std::string("Minimal reproduction (").size()));
  EXPECT_GE(count, 3U);
  EXPECT_LE(count, 4U);
  EXPECT_NE(report.find("GetSnapshot()", start), std::string::npos);
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
  EXPECT_EQ(RunModelTest(DbSetup(OpenMemoryDB), SeedAndOps(5, 20000)), "");
}

TEST(ModelHarnessTest, AgreesWithACorrectDBAcrossReopens) {
  std::size_t reopens = 0;
  EXPECT_EQ(RunModelTest(ReopenableMemoryDB(nullptr, &reopens), SeedAndOps(5, 20000, 40)), "");
  EXPECT_GE(reopens, 20U) << "the run did reopen";
}

TEST(ModelHarnessTest, AReopenWithoutAReopenFunctionIsReported) {
  Op reopen;
  reopen.kind = Op::Kind::kReopen;
  EXPECT_EQ(Describe(reopen), "Reopen()");
  const std::vector<Op> ops = {reopen};
  const std::optional<Failure> failure = Replay(DbSetup(OpenMemoryDB), ops);
  if (!failure.has_value()) {
    FAIL() << "a reopen needs a reopen function";
  }
  EXPECT_EQ(failure->op_index, 0U);
  EXPECT_NE(failure->message.find("no reopen function"), std::string::npos) << failure->message;
}

TEST(ModelHarnessTest, CatchesABugAndShrinksItToAMinimalReproduction) {
  const std::string report = RunModelTest(DbSetup(OpenBuggyDB), SeedAndOps(5, 20000));
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

TEST(ModelHarnessTest, CatchesWritesLostByAReopenAndShrinksThem) {
  const std::string report =
      RunModelTest(ReopenableMemoryDB(DropTheLastByteOfTheNewestLog), SeedAndOps(5, 20000, 40));
  ASSERT_NE(report, "") << "the lost write must be found";
  SCOPED_TRACE(report);
  EXPECT_EQ(report.find("may not be minimal"), std::string::npos);

  // The shortest failure is: write, reopen (the write is lost), read it.
  const std::size_t start = report.find("Minimal reproduction (");
  ASSERT_NE(start, std::string::npos);
  const std::size_t count =
      std::stoul(report.substr(start + std::string("Minimal reproduction (").size()));
  EXPECT_GE(count, 3U);
  EXPECT_LE(count, 4U);
  EXPECT_NE(report.find("Reopen()", start), std::string::npos);
}

TEST(ModelHarnessTest, ShrinkResultStillFailsAndIsOneMinimal) {
  // Find a failing prefix directly, then check Shrink's two promises.
  OpGenerator generator(9, 20000);
  std::vector<Op> ops;
  std::optional<Failure> failure;
  const DbSetup buggy(OpenBuggyDB);
  {
    std::unique_ptr<DB> db = std::move(OpenBuggyDB()).value();
    Model model;
    Snapshots snapshots;
    while (!failure && ops.size() < 20000) {
      ops.push_back(generator.Next());
      if (std::optional<std::string> problem = Apply(db, buggy, model, snapshots, ops.back())) {
        failure = Failure{.op_index = ops.size() - 1, .message = *problem};
      }
    }
  }
  ASSERT_TRUE(failure.has_value());

  const ShrinkResult shrunk = Shrink(buggy, ops);
  ASSERT_FALSE(shrunk.ops.empty());
  EXPECT_FALSE(shrunk.stopped_early);
  const std::optional<Failure> replayed = Replay(buggy, shrunk.ops);
  if (!replayed.has_value()) {
    FAIL() << "the reproduction must still fail";
  }
  EXPECT_EQ(replayed->op_index, shrunk.ops.size() - 1) << "at its last operation";
  EXPECT_FALSE(Replay(DbSetup(OpenMemoryDB), shrunk.ops).has_value())
      << "and a correct DB passes it";
  for (std::size_t i = 0; i + 1 < shrunk.ops.size(); ++i) {  // 1-minimal
    std::vector<Op> without = shrunk.ops;
    without.erase(without.begin() + static_cast<std::ptrdiff_t>(i));
    EXPECT_FALSE(Replay(buggy, without).has_value()) << "operation " << i << " is needed";
  }
}

}  // namespace
}  // namespace forgedb::model
