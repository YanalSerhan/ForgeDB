// forgedb_bench (TODO 5.1; tools/bench/): the latency histogram, the JSON
// writer, option parsing, reading LevelDB's properties, the disk engine
// factory, and whole runs on an in-memory file system, including that a wrong
// answer from an engine fails the run. The RocksDB, LevelDB and SQLite
// adapters themselves are tested in tests/bench/engines_test.cpp (msvc-bench).
#include "tools/bench/bench.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>

#include <gtest/gtest.h>

#include <iterator>
#include <limits>
#include <map>
#include <memory>
#include <random>
#include <span>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "forgedb/iterator.h"
#include "forgedb/result.h"
#include "forgedb/status.h"

#include "env/mem_file_system.h"
#include "tools/bench/bench_options.h"
#include "tools/bench/engine.h"
#include "tools/bench/forgedb_engine.h"
#include "tools/bench/histogram.h"
#include "tools/bench/json_writer.h"
#include "tools/bench/leveldb_properties.h"

namespace forgedb::bench {
namespace {

// --- LatencyHistogram ---------------------------------------------------------------

TEST(LatencyHistogramTest, BucketsAreContiguousAndWithinOneSixteenth) {
  std::mt19937_64 rng(1);
  for (int i = 0; i < 100000; ++i) {
    const std::uint64_t value = rng() >> (rng() % 64);
    const std::size_t bucket = LatencyHistogram::BucketOf(value);
    ASSERT_LT(bucket, LatencyHistogram::kBuckets);
    const std::uint64_t upper = LatencyHistogram::UpperEdge(bucket);
    ASSERT_GE(upper, value);
    if (bucket > 0) {
      ASSERT_LT(LatencyHistogram::UpperEdge(bucket - 1), value) << "no gap or overlap";
    }
    if (value >= 16) {
      ASSERT_LE(static_cast<double>(upper - value), static_cast<double>(value) / 16) << value;
    } else {
      ASSERT_EQ(upper, value) << "exact below 16";
    }
  }
  EXPECT_EQ(LatencyHistogram::UpperEdge(LatencyHistogram::BucketOf(UINT64_MAX)), UINT64_MAX);
}

TEST(LatencyHistogramTest, PercentilesMinMaxMeanAndMerge) {
  const LatencyHistogram empty;
  EXPECT_EQ(empty.Percentile(0.5), 0U);
  EXPECT_EQ(empty.Max(), 0U);

  LatencyHistogram low;
  LatencyHistogram high;
  for (std::uint64_t v = 1; v <= 1000; ++v) {
    (v <= 500 ? low : high).Add(v * 1000);  // 1 to 1000 microseconds
  }
  low.Merge(high);
  EXPECT_EQ(low.Count(), 1000U);
  EXPECT_EQ(low.Min(), 1000U);
  EXPECT_EQ(low.Max(), 1000000U);
  EXPECT_DOUBLE_EQ(low.Mean(), 500500.0);
  const auto p50 = static_cast<double>(low.Percentile(0.5));
  const auto p99 = static_cast<double>(low.Percentile(0.99));
  EXPECT_GE(p50, 500000.0);
  EXPECT_LE(p50, 500000.0 * 17 / 16);
  EXPECT_GE(p99, 990000.0);
  EXPECT_LE(p99, 1000000.0) << "never above the maximum";
  EXPECT_EQ(low.Percentile(1.0), 1000000U);
}

// --- JsonWriter -----------------------------------------------------------------------

TEST(JsonWriterTest, WritesNestedValuesWithCommasAndIndentation) {
  JsonWriter json;
  json.BeginObject();
  json.Key("name");
  json.String("fill\"random\"\n\x01");
  json.Key("list");
  json.BeginArray();
  json.Number(std::uint64_t{1});
  json.Number(2.5);
  json.Bool(false);
  json.Null();
  json.EndArray();
  json.Key("empty");
  json.BeginObject();
  json.EndObject();
  json.Key("nan");
  json.Number(std::numeric_limits<double>::quiet_NaN());
  json.Key("negative");
  json.Number(std::int64_t{-3});
  json.EndObject();
  EXPECT_EQ(json.Take(),
            "{\n"
            "  \"name\": \"fill\\\"random\\\"\\n\\u0001\",\n"
            "  \"list\": [\n"
            "    1,\n"
            "    2.5,\n"
            "    false,\n"
            "    null\n"
            "  ],\n"
            "  \"empty\": {},\n"
            "  \"nan\": null,\n"
            "  \"negative\": -3\n"
            "}");
}

// --- Options -------------------------------------------------------------------------

const std::vector<std::string> kEngines = {"forgedb"};

Result<BenchOptions> Parse(std::vector<std::string> args) {
  return ParseBenchOptions(args, kEngines);
}

TEST(BenchOptionsTest, DefaultsAndEveryOption) {
  const Result<BenchOptions> defaults = Parse({});
  ASSERT_TRUE(defaults.ok());
  EXPECT_EQ(defaults->num, 1000000U);
  EXPECT_EQ(defaults->key_size, 16U);
  EXPECT_EQ(defaults->value_size, 100U);
  EXPECT_EQ(defaults->workloads.size(), kWorkloads.size());
  EXPECT_FALSE(defaults->sync);
  EXPECT_EQ(defaults->cache_mb, 8U);
  EXPECT_EQ(defaults->memtable_mb, 4U);

  const Result<BenchOptions> all =
      Parse({"--engine=forgedb", "--workloads=readrandom,fillseq", "--num=5000", "--key_size=20",
             "--value_size=1024", "--sync=1", "--threads=4", "--cache_mb=64", "--memtable_mb=16",
             "--runs=5", "--seed=7", "--db=somewhere", "--json=out.json"});
  ASSERT_TRUE(all.ok()) << all.status().ToString();
  EXPECT_EQ(all->workloads, (std::vector<std::string>{"readrandom", "fillseq"}));
  EXPECT_EQ(all->num, 5000U);
  EXPECT_EQ(all->key_size, 20U);
  EXPECT_EQ(all->value_size, 1024U);
  EXPECT_TRUE(all->sync);
  EXPECT_EQ(all->threads, 4U);
  EXPECT_EQ(all->cache_mb, 64U);
  EXPECT_EQ(all->memtable_mb, 16U);
  EXPECT_EQ(all->runs, 5U);
  EXPECT_EQ(all->seed, 7U);
  EXPECT_EQ(all->db, std::filesystem::path("somewhere"));
  EXPECT_EQ(all->json, std::filesystem::path("out.json"));
}

TEST(BenchOptionsTest, ProblemsAreNamed) {
  const auto problem = [](std::vector<std::string> args) {
    const Result<BenchOptions> result = Parse(std::move(args));
    EXPECT_FALSE(result.ok());
    return result.status().ToString();
  };
  EXPECT_NE(problem({"--frobnicate=1"}).find("unknown option --frobnicate"), std::string::npos);
  EXPECT_NE(problem({"num=5"}).find("expected --name=value"), std::string::npos);
  EXPECT_NE(problem({"--num=0"}).find("--num must be a number from 1"), std::string::npos);
  EXPECT_NE(problem({"--threads=many"}).find("--threads"), std::string::npos);
  EXPECT_NE(problem({"--workloads=fillseq,teleport"}).find("unknown workload 'teleport'"),
            std::string::npos);
  EXPECT_NE(problem({"--engine=rocksdb"}).find("unknown engine 'rocksdb'"), std::string::npos);
  EXPECT_NE(problem({"--sync=2"}).find("--sync"), std::string::npos);
  EXPECT_NE(problem({"--memtable_mb=0"}).find("--memtable_mb must be a number from 1 to 1024"),
            std::string::npos);
  EXPECT_NE(problem({"--num=100000000", "--key_size=8"}).find("too short"), std::string::npos);
}

// --- LevelDB's properties ----------------------------------------------------------

// "leveldb.sstables" as LevelDB prints it (Version::DebugString).
constexpr std::string_view kSstables =
    "--- level 0 ---\n"
    " 12:1048600['00000000000000000002' @ 9 : 1 .. '00000000000000000998' @ 400 : 1]\n"
    " 14:2000['k' @ 401 : 1 .. 'k\\x00\\xff' @ 402 : 0]\n"
    "--- level 1 ---\n"
    " 9:5242880['a' @ 1 : 1 .. 'b' @ 2 : 1]\n"
    "--- level 2 ---\n"
    "--- level 3 ---\n"
    " 7:52428800['c' @ 3 : 1 .. 'd' @ 4 : 1]\n"
    "--- level 4 ---\n"
    "--- level 5 ---\n"
    "--- level 6 ---\n"
    " 3:99['e' @ 5 : 1 .. 'f' @ 6 : 1]\n";

TEST(LevelDBPropertiesTest, TablesPerLevel) {
  const Result<LevelDBTables> tables = ParseLevelDBTables(kSstables);
  ASSERT_TRUE(tables.ok()) << tables.status().ToString();
  EXPECT_EQ(tables->files, (std::array<std::uint64_t, kLevelDBLevels>{2, 1, 0, 1, 0, 0, 1}));
  EXPECT_EQ(tables->bytes,
            (std::array<std::uint64_t, kLevelDBLevels>{1050600, 5242880, 0, 52428800, 0, 0, 99}));

  const Result<LevelDBTables> empty = ParseLevelDBTables(
      "--- level 0 ---\n--- level 1 ---\n--- level 2 ---\n--- level 3 ---\n"
      "--- level 4 ---\n--- level 5 ---\n--- level 6 ---\n");
  ASSERT_TRUE(empty.ok()) << empty.status().ToString();
  EXPECT_EQ(empty->files, (std::array<std::uint64_t, kLevelDBLevels>{}));
}

TEST(LevelDBPropertiesTest, MalformedTablesAreCorruption) {
  for (const std::string_view text :
       {std::string_view(" 12:100['a' .. 'b']\n"),            // a file before any level
        std::string_view("--- level 7 ---\n"),                // no such level
        std::string_view("--- level 0 ---\n 12:big['a']\n"),  // size not a number
        std::string_view("--- level 0 ---\nsomething else\n")}) {
    const Result<LevelDBTables> tables = ParseLevelDBTables(text);
    ASSERT_FALSE(tables.ok()) << text;
    EXPECT_EQ(tables.status().code(), Status::Code::kCorruption) << text;
  }
}

TEST(LevelDBPropertiesTest, CompactionScoreIsLevelDBs) {
  LevelDBTables tables;
  EXPECT_DOUBLE_EQ(LevelDBCompactionScore(tables), 0.0);
  tables.files[0] = 3;
  EXPECT_DOUBLE_EQ(LevelDBCompactionScore(tables), 0.75);
  tables.files[0] = 4;  // the level-0 trigger
  EXPECT_DOUBLE_EQ(LevelDBCompactionScore(tables), 1.0);
  tables.files[0] = 0;
  tables.bytes[1] = std::uint64_t{5} << 20U;  // half of level 1's 10 MiB
  EXPECT_DOUBLE_EQ(LevelDBCompactionScore(tables), 0.5);
  tables.bytes[3] = std::uint64_t{1500} << 20U;  // level 3 holds 1,000 MiB
  EXPECT_DOUBLE_EQ(LevelDBCompactionScore(tables), 1.5);
  tables.bytes[3] = 0;
  tables.bytes[6] = std::uint64_t{1} << 40U;  // the last level never scores
  EXPECT_DOUBLE_EQ(LevelDBCompactionScore(tables), 0.5);
  EXPECT_DOUBLE_EQ(LevelDBCompactionScore(*ParseLevelDBTables(kSstables)), 0.5);
}

TEST(LevelDBPropertiesTest, CompactionTotals) {
  // "leveldb.stats" as LevelDB prints it (DBImpl::GetProperty).
  const Result<LevelDBCompactionStats> stats = ParseLevelDBStats(
      "                               Compactions\n"
      "Level  Files Size(MB) Time(sec) Read(MB) Write(MB)\n"
      "--------------------------------------------------\n"
      "  0        2        4         1        0        36\n"
      "  1        5       10         3       60        58\n"
      "  2       40       80         2       30        29\n");
  ASSERT_TRUE(stats.ok()) << stats.status().ToString();
  EXPECT_DOUBLE_EQ(stats->seconds, 6.0);
  EXPECT_DOUBLE_EQ(stats->read_mb, 90.0);
  EXPECT_DOUBLE_EQ(stats->write_mb, 123.0);

  const Result<LevelDBCompactionStats> none = ParseLevelDBStats(
      "                               Compactions\n"
      "Level  Files Size(MB) Time(sec) Read(MB) Write(MB)\n"
      "--------------------------------------------------\n");
  ASSERT_TRUE(none.ok()) << none.status().ToString();
  EXPECT_DOUBLE_EQ(none->write_mb, 0.0);

  EXPECT_FALSE(ParseLevelDBStats("no table here").ok());
  EXPECT_FALSE(ParseLevelDBStats("--------------------------------------------------\n"
                                 "  0        2        4         1\n")
                   .ok());
}

// --- The disk engine factory --------------------------------------------------------

Result<std::unique_ptr<Engine>> OpenNothing(const std::filesystem::path& dir,
                                            const EngineOptions& /*options*/) {
  return Status::NotSupported("not an engine", dir.filename().generic_string());
}

std::filesystem::path FreshFolder(std::string_view name) {
  const std::filesystem::path dir = std::filesystem::temp_directory_path() / name;
  std::filesystem::remove_all(dir);
  return dir;
}

void WriteFile(const std::filesystem::path& path, std::string_view contents) {
  std::ofstream file(path, std::ios::binary);
  file << contents;
}

TEST(DiskEngineFactoryTest, OpensByNameAndManagesFolders) {
  const auto factory = NewDiskEngineFactory(
      {{.name = "first", .open = OpenNothing}, {.name = "second", .open = OpenNothing}});
  EXPECT_EQ(factory->Engines(), (std::vector<std::string>{"first", "second"}));
  EXPECT_TRUE(factory->RealFiles());

  const std::filesystem::path root = FreshFolder("forgedb_disk_factory_test") / "a" / "b";
  ASSERT_TRUE(factory->CreateRoot(root).ok());
  EXPECT_TRUE(std::filesystem::is_directory(root));

  const Result<std::unique_ptr<Engine>> second = factory->Open("second", root / "db", {});
  ASSERT_FALSE(second.ok());
  EXPECT_EQ(second.status().code(), Status::Code::kNotSupported) << "the opener was called";
  const Result<std::unique_ptr<Engine>> unknown = factory->Open("third", root / "db", {});
  ASSERT_FALSE(unknown.ok());
  EXPECT_EQ(unknown.status().code(), Status::Code::kInvalidArgument);

  WriteFile(root / "file", "data");
  ASSERT_TRUE(factory->Remove(root).ok());
  EXPECT_FALSE(std::filesystem::exists(root));
  EXPECT_TRUE(factory->Remove(root).ok()) << "removing what is not there is fine";
  std::filesystem::remove_all(root.parent_path().parent_path());
}

TEST(DiskEngineFactoryTest, FolderBytesCountsTheFilesInTheFolder) {
  const std::filesystem::path dir = FreshFolder("forgedb_folder_bytes_test");
  EXPECT_FALSE(FolderBytes(dir).ok()) << "no folder";
  std::filesystem::create_directories(dir / "sub");
  const Result<std::uint64_t> none = FolderBytes(dir);
  ASSERT_TRUE(none.ok()) << none.status().ToString();
  EXPECT_EQ(*none, 0U);
  WriteFile(dir / "one", "abc");
  WriteFile(dir / "two", "defgh");
  WriteFile(dir / "sub" / "three", "not counted");
  const Result<std::uint64_t> eight = FolderBytes(dir);
  ASSERT_TRUE(eight.ok()) << eight.status().ToString();
  EXPECT_EQ(*eight, 8U);
  std::filesystem::remove_all(dir);
}

// --- Whole runs on MemFileSystem -------------------------------------------------------------

BenchOptions Small() {
  BenchOptions options = std::move(Parse({"--num=3000", "--threads=2", "--db=bench"})).value();
  return options;
}

TEST(BenchRunTest, FillRandomCountsWhatItWrote) {
  const auto factory = NewForgeDBFactory(std::make_shared<MemFileSystem>());
  const BenchOptions options = Small();
  ASSERT_TRUE(factory->CreateRoot("bench").ok());
  const Result<RunResult> result =
      RunWorkload(*factory, "forgedb", "fillrandom", 1, options, "bench");
  ASSERT_TRUE(result.ok()) << result.status().ToString();
  EXPECT_EQ(result->ops, 3000U);
  EXPECT_EQ(result->bytes, 3000U * (16 + 100));
  EXPECT_TRUE(result->has_latency);
  EXPECT_EQ(result->latency.Count(), 3000U);
  EXPECT_GT(result->seconds, 0.0);
  EXPECT_FALSE(result->write_amplification) << "in memory: no process I/O to count";
  ASSERT_TRUE(result->space_amplification);
  EXPECT_GT(result->space_amplification.value_or(0), 0.9) << "every key and value is on disk";
  EXPECT_DOUBLE_EQ(result->counters.at("user_bytes"), 3000.0 * (16 + 100));
}

TEST(BenchRunTest, ReadsWriteNothingAndCheckTheirAnswers) {
  const auto factory = NewForgeDBFactory(std::make_shared<MemFileSystem>());
  const BenchOptions options = Small();
  ASSERT_TRUE(factory->CreateRoot("bench").ok());
  for (const std::string_view workload : {"readrandom", "readmissing", "seekrandom"}) {
    const Result<RunResult> result =
        RunWorkload(*factory, "forgedb", workload, 1, options, "bench");
    ASSERT_TRUE(result.ok()) << workload << ": " << result.status().ToString();
    EXPECT_EQ(result->ops, 3000U) << workload;
    EXPECT_DOUBLE_EQ(result->counters.at("user_bytes"), 0.0) << workload;
  }
  const Result<RunResult> scan = RunWorkload(*factory, "forgedb", "readseq", 1, options, "bench");
  ASSERT_TRUE(scan.ok()) << scan.status().ToString();
  EXPECT_EQ(scan->ops, 2U * 3000) << "each of the 2 threads scans everything";
  EXPECT_FALSE(scan->has_latency);
}

// An engine that loses the writes of every tenth key (key 2i with i a
// multiple of 10: its last digit is 0 and its tens digit is even). The
// benchmark must notice; with one key in ten lost, 1,000 random reads miss
// all of them with a probability of 0.9^1000.
class LosesSomeKeys final : public Engine {
 public:
  explicit LosesSomeKeys(std::unique_ptr<Engine> real) : real_(std::move(real)) {}
  Status Put(std::string_view key, std::string_view value) override {
    const bool lost = key.size() >= 2 && key.back() == '0' && (key[key.size() - 2] - '0') % 2 == 0;
    return lost ? Status::OK() : real_->Put(key, value);
  }
  Result<bool> Get(std::string_view key, std::string& value) override {
    return real_->Get(key, value);
  }
  std::unique_ptr<Iterator> NewIterator() override { return real_->NewIterator(); }
  Status WaitForBackgroundWork() override { return real_->WaitForBackgroundWork(); }
  Result<std::uint64_t> DiskBytes() override { return real_->DiskBytes(); }
  std::map<std::string, double> Counters() override { return real_->Counters(); }

 private:
  std::unique_ptr<Engine> real_;
};

class LossyFactory final : public EngineFactory {
 public:
  [[nodiscard]] std::vector<std::string> Engines() const override { return {"forgedb"}; }
  Result<std::unique_ptr<Engine>> Open(std::string_view engine, const std::filesystem::path& dir,
                                       const EngineOptions& options) override {
    FORGEDB_ASSIGN_OR_RETURN(std::unique_ptr<Engine> real, real_->Open(engine, dir, options));
    return std::unique_ptr<Engine>(std::make_unique<LosesSomeKeys>(std::move(real)));
  }
  Status Remove(const std::filesystem::path& dir) override { return real_->Remove(dir); }
  Status CreateRoot(const std::filesystem::path& dir) override { return real_->CreateRoot(dir); }
  [[nodiscard]] bool RealFiles() const override { return false; }

 private:
  std::unique_ptr<EngineFactory> real_ = NewForgeDBFactory(std::make_shared<MemFileSystem>());
};

TEST(BenchRunTest, AWrongAnswerFailsTheRun) {
  LossyFactory factory;
  BenchOptions options = Small();
  options.num = 1000;
  ASSERT_TRUE(factory.CreateRoot("bench").ok());
  for (const std::string_view workload : {"readseq", "seekrandom", "readrandom"}) {
    const Result<RunResult> result = RunWorkload(factory, "forgedb", workload, 1, options, "bench");
    ASSERT_FALSE(result.ok()) << workload;
    EXPECT_EQ(result.status().code(), Status::Code::kCorruption) << workload;
    EXPECT_NE(result.status().ToString().find("wrong answer"), std::string::npos) << workload;
  }
}

TEST(BenchRunTest, TheWholeToolWithJson) {
  const auto factory = NewForgeDBFactory(std::make_shared<MemFileSystem>());
  const std::filesystem::path json_path =
      std::filesystem::temp_directory_path() / "forgedb_bench_test.json";
  std::ostringstream out;
  std::ostringstream err;
  const std::vector<std::string> args = {"--num=2000", "--threads=2", "--runs=2", "--db=bench",
                                         "--json=" + json_path.string()};
  ASSERT_EQ(RunBench(args, *factory, out, err), 0) << err.str();
  for (const std::string_view workload : kWorkloads) {
    EXPECT_NE(out.str().find(std::string(workload) + "  forgedb  run 2: "), std::string::npos)
        << workload;
    EXPECT_NE(out.str().find(std::string(workload) + "  forgedb: ops/s median "), std::string::npos)
        << workload;
  }
  std::string json;
  {
    std::ifstream file(json_path, std::ios::binary);
    json.assign(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
  }
  std::filesystem::remove(json_path);
  std::size_t runs = 0;
  for (std::size_t at = json.find(R"("engine": "forgedb")"); at != std::string::npos;
       at = json.find(R"("engine": "forgedb")", at + 1)) {
    ++runs;
  }
  EXPECT_EQ(runs, 2 * kWorkloads.size());
  EXPECT_NE(json.find(R"("num": 2000)"), std::string::npos);
  EXPECT_NE(json.find(R"("memtable_mb": 4)"), std::string::npos);
  EXPECT_NE(json.find(R"("compression": "none")"), std::string::npos);
  EXPECT_NE(json.find(R"("latency_us": {)"), std::string::npos);
  EXPECT_NE(json.find(R"("write_amplification": null)"), std::string::npos) << "in memory";

  // Usage errors: exit code 2 and the usage text.
  std::ostringstream usage;
  EXPECT_EQ(RunBench(std::vector<std::string>{"--bogus=1"}, *factory, out, usage), 2);
  EXPECT_NE(usage.str().find("usage: forgedb_bench"), std::string::npos);
}

}  // namespace
}  // namespace forgedb::bench
