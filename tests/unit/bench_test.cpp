// forgedb_bench (TODO 5.1; tools/bench/): the latency histogram, the JSON
// writer, option parsing, and whole runs on an in-memory file system,
// including that a wrong answer from an engine fails the run.
#include "tools/bench/bench.h"

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

  const Result<BenchOptions> all =
      Parse({"--engine=forgedb", "--workloads=readrandom,fillseq", "--num=5000", "--key_size=20",
             "--value_size=1024", "--sync=1", "--threads=4", "--cache_mb=64", "--runs=5",
             "--seed=7", "--db=somewhere", "--json=out.json"});
  ASSERT_TRUE(all.ok()) << all.status().ToString();
  EXPECT_EQ(all->workloads, (std::vector<std::string>{"readrandom", "fillseq"}));
  EXPECT_EQ(all->num, 5000U);
  EXPECT_EQ(all->key_size, 20U);
  EXPECT_EQ(all->value_size, 1024U);
  EXPECT_TRUE(all->sync);
  EXPECT_EQ(all->threads, 4U);
  EXPECT_EQ(all->cache_mb, 64U);
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
  EXPECT_NE(problem({"--num=100000000", "--key_size=8"}).find("too short"), std::string::npos);
}

// --- Whole runs on MemFileSystem -------------------------------------------------------------

BenchOptions Small() {
  BenchOptions options = std::move(Parse({"--num=3000", "--threads=2", "--db=bench"})).value();
  return options;
}

TEST(BenchRunTest, FillRandomCountsWhatItWrote) {
  const auto factory = NewForgeDBFactory(std::make_shared<MemFileSystem>(), false);
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
  const auto factory = NewForgeDBFactory(std::make_shared<MemFileSystem>(), false);
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
  std::unique_ptr<EngineFactory> real_ =
      NewForgeDBFactory(std::make_shared<MemFileSystem>(), false);
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
  const auto factory = NewForgeDBFactory(std::make_shared<MemFileSystem>(), false);
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
  EXPECT_NE(json.find(R"("latency_us": {)"), std::string::npos);
  EXPECT_NE(json.find(R"("write_amplification": null)"), std::string::npos) << "in memory";

  // Usage errors: exit code 2 and the usage text.
  std::ostringstream usage;
  EXPECT_EQ(RunBench(std::vector<std::string>{"--bogus=1"}, *factory, out, usage), 2);
  EXPECT_NE(usage.str().find("usage: forgedb_bench"), std::string::npos);
}

}  // namespace
}  // namespace forgedb::bench
