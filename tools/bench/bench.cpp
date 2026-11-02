#include "tools/bench/bench.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <functional>
#include <latch>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <ostream>
#include <random>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

#include "forgedb/iterator.h"
#include "forgedb/result.h"
#include "forgedb/status.h"
#include "forgedb/version.h"

#include "env/process_io.h"
#include "tools/bench/bench_options.h"
#include "tools/bench/engine.h"
#include "tools/bench/histogram.h"
#include "tools/bench/json_writer.h"

namespace forgedb::bench {
namespace {

using Clock = std::chrono::steady_clock;

constexpr std::uint64_t kSeekScanLength = 10;  // entries read after each seek

// Keys are the even numbers 2i (i < num), zero-padded to key_size digits;
// the odd number 2i + 1 between two of them is a missing key.
class Keys {
 public:
  explicit Keys(std::uint64_t width) : width_(width) {}
  [[nodiscard]] std::string Existing(std::uint64_t index) const { return Format(2 * index); }
  [[nodiscard]] std::string Missing(std::uint64_t index) const { return Format((2 * index) + 1); }

 private:
  [[nodiscard]] std::string Format(std::uint64_t n) const {
    std::string digits = std::to_string(n);
    digits.insert(0, width_ - digits.size(), '0');
    return digits;
  }
  std::uint64_t width_;
};

// Values are slices of a block of random letters (no engine compresses).
class Values {
 public:
  Values(std::uint64_t size, std::uint64_t seed) : size_(size) {
    std::mt19937_64 rng(seed ^ 0x5eedULL);
    data_.resize(std::max<std::uint64_t>(1 << 20, size * 4) + size);
    for (char& c : data_) {
      c = static_cast<char>('a' + (rng() % 26));
    }
  }
  [[nodiscard]] std::string_view Next(std::mt19937_64& rng) const {
    const std::uint64_t offset = rng() % (data_.size() - size_ + 1);
    return std::string_view(data_).substr(offset, size_);
  }

 private:
  std::uint64_t size_;
  std::string data_;
};

// What one thread did.
struct ThreadTally {
  std::uint64_t ops = 0;
  std::uint64_t bytes = 0;          // keys and values written or read
  std::uint64_t bytes_written = 0;  // keys and values written
  LatencyHistogram latency;
  Clock::time_point start;
  Clock::time_point end;
};

// What a thread runs: its index, its share of the operations [begin, end)
// of options.num, its tally. A wrong answer or an error is a Status.
using ThreadBody = std::function<Status(std::uint64_t thread, std::uint64_t begin,
                                        std::uint64_t end, ThreadTally& tally)>;

struct PhaseResult {
  ThreadTally total;
  double seconds = 0;
};

// Runs `body` on options.threads threads, which start together.
Result<PhaseResult> RunThreads(const BenchOptions& options, const ThreadBody& body) {
  const std::uint64_t threads = options.threads;
  std::vector<ThreadTally> tallies(threads);
  std::vector<Status> statuses(threads);
  std::latch ready(static_cast<std::ptrdiff_t>(threads));
  std::vector<std::thread> workers;
  workers.reserve(threads);
  for (std::uint64_t t = 0; t < threads; ++t) {
    workers.emplace_back([&, t] {
      const std::uint64_t begin = options.num * t / threads;
      const std::uint64_t end = options.num * (t + 1) / threads;
      ready.arrive_and_wait();
      tallies[t].start = Clock::now();
      statuses[t] = body(t, begin, end, tallies[t]);
      tallies[t].end = Clock::now();
    });
  }
  for (std::thread& worker : workers) {
    worker.join();
  }
  PhaseResult result;
  Clock::time_point start = tallies.front().start;
  Clock::time_point end = tallies.front().end;
  for (std::uint64_t t = 0; t < threads; ++t) {
    FORGEDB_RETURN_IF_ERROR(statuses[t]);
    const ThreadTally& tally = tallies[t];
    result.total.ops += tally.ops;
    result.total.bytes += tally.bytes;
    result.total.bytes_written += tally.bytes_written;
    result.total.latency.Merge(tally.latency);
    start = std::min(start, tally.start);
    end = std::max(end, tally.end);
  }
  result.seconds = std::chrono::duration<double>(end - start).count();
  return result;
}

std::uint64_t Nanoseconds(Clock::time_point start) {
  return static_cast<std::uint64_t>(
      std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - start).count());
}

Status Wrong(std::string_view workload, const std::string& what) {
  return Status::Corruption(std::string(workload) + ": wrong answer", what);
}

// Runs the operations of one workload for one thread. `permutation` is
// fillrandom's key order.
class WorkloadOps {
 public:
  WorkloadOps(std::string workload, Engine& engine, const BenchOptions& options, const Keys& keys,
              const Values& values, const std::vector<std::uint64_t>& permutation)
      : workload_(std::move(workload)),
        engine_(&engine),
        options_(&options),
        keys_(&keys),
        values_(&values),
        permutation_(&permutation),
        entry_bytes_(options.key_size + options.value_size) {}

  Status Run(std::uint64_t thread, std::uint64_t begin, std::uint64_t end, ThreadTally& tally) {
    if (workload_ == "fillseq" || workload_ == "fillrandom") {
      return Fill(thread, begin, end, tally);
    }
    if (workload_ == "readseq") {
      return Scan(tally);
    }
    if (workload_ == "seekrandom") {
      return SeekAndScan(thread, begin, end, tally);
    }
    return RandomKeys(thread, begin, end, tally);  // overwrite, readrandom, readmissing, mixed
  }

 private:
  Status Put(const std::string& key, std::mt19937_64& rng, ThreadTally& tally) {
    const auto start = Clock::now();
    FORGEDB_RETURN_IF_ERROR(engine_->Put(key, values_->Next(rng)));
    tally.latency.Add(Nanoseconds(start));
    ++tally.ops;
    tally.bytes += entry_bytes_;
    tally.bytes_written += entry_bytes_;
    return Status::OK();
  }

  Status Get(const std::string& key, bool should_exist, std::string& value, ThreadTally& tally) {
    const auto start = Clock::now();
    FORGEDB_ASSIGN_OR_RETURN(const bool found, engine_->Get(key, value));
    tally.latency.Add(Nanoseconds(start));
    ++tally.ops;
    if (found != should_exist) {
      return Wrong(workload_, "Get " + key + (found ? " found a value" : " found nothing"));
    }
    if (found && value.size() != options_->value_size) {
      return Wrong(workload_, "Get " + key + " found " + std::to_string(value.size()) + " bytes");
    }
    tally.bytes += found ? key.size() + value.size() : 0;
    return Status::OK();
  }

  Status Fill(std::uint64_t thread, std::uint64_t begin, std::uint64_t end, ThreadTally& tally) {
    const bool random = workload_ == "fillrandom";
    std::mt19937_64 rng(options_->seed + thread);
    for (std::uint64_t i = begin; i < end; ++i) {
      FORGEDB_RETURN_IF_ERROR(Put(keys_->Existing(random ? (*permutation_)[i] : i), rng, tally));
    }
    return Status::OK();
  }

  Status RandomKeys(std::uint64_t thread, std::uint64_t begin, std::uint64_t end,
                    ThreadTally& tally) {
    std::mt19937_64 rng(options_->seed + 1000 + thread);
    std::string value;
    for (std::uint64_t i = begin; i < end; ++i) {
      const std::uint64_t index = rng() % options_->num;
      if (workload_ == "overwrite" || (workload_ == "mixed" && rng() % 10 == 0)) {
        FORGEDB_RETURN_IF_ERROR(Put(keys_->Existing(index), rng, tally));
      } else if (workload_ == "readmissing") {
        FORGEDB_RETURN_IF_ERROR(Get(keys_->Missing(index), false, value, tally));
      } else {
        FORGEDB_RETURN_IF_ERROR(Get(keys_->Existing(index), true, value, tally));
      }
    }
    return Status::OK();
  }

  // Every thread scans everything: one operation per entry.
  Status Scan(ThreadTally& tally) {
    const std::unique_ptr<Iterator> it = engine_->NewIterator();
    std::uint64_t index = 0;
    for (it->SeekToFirst(); it->Valid(); it->Next(), ++index) {
      if (index >= options_->num || it->key() != keys_->Existing(index)) {
        return Wrong(workload_, "entry " + std::to_string(index) + " is " + std::string(it->key()) +
                                    ", expected " +
                                    (index < options_->num ? keys_->Existing(index) : "the end"));
      }
      ++tally.ops;
      tally.bytes += it->key().size() + it->value().size();
    }
    FORGEDB_RETURN_IF_ERROR(it->status());
    if (index != options_->num) {
      return Wrong(workload_, "the scan ended after " + std::to_string(index) + " entries");
    }
    return Status::OK();
  }

  Status SeekAndScan(std::uint64_t thread, std::uint64_t begin, std::uint64_t end,
                     ThreadTally& tally) {
    std::mt19937_64 rng(options_->seed + 2000 + thread);
    const std::unique_ptr<Iterator> it = engine_->NewIterator();
    for (std::uint64_t i = begin; i < end; ++i) {
      const std::uint64_t index = rng() % options_->num;
      const std::string target = keys_->Existing(index);
      const auto start = Clock::now();
      it->Seek(target);
      std::uint64_t read = 0;
      for (; it->Valid() && read < kSeekScanLength; it->Next(), ++read) {
        if (it->key() != keys_->Existing(index + read)) {
          return Wrong(workload_, "after seeking " + target + ", entry " + std::to_string(read) +
                                      " is " + std::string(it->key()));
        }
        tally.bytes += it->key().size() + it->value().size();
      }
      tally.latency.Add(Nanoseconds(start));
      ++tally.ops;
      FORGEDB_RETURN_IF_ERROR(it->status());
      if (read != std::min(kSeekScanLength, options_->num - index)) {
        return Wrong(workload_,
                     "after seeking " + target + ", " + std::to_string(read) + " entries");
      }
    }
    return Status::OK();
  }

  std::string workload_;
  Engine* engine_;
  const BenchOptions* options_;
  const Keys* keys_;
  const Values* values_;
  const std::vector<std::uint64_t>* permutation_;
  std::uint64_t entry_bytes_;
};

ThreadBody Workload(std::string_view workload, Engine& engine, const BenchOptions& options,
                    const Keys& keys, const Values& values,
                    const std::vector<std::uint64_t>& permutation) {
  auto ops = std::make_shared<WorkloadOps>(std::string(workload), engine, options, keys, values,
                                           permutation);
  return [ops](std::uint64_t thread, std::uint64_t begin, std::uint64_t end, ThreadTally& tally) {
    return ops->Run(thread, begin, end, tally);
  };
}

void NumberOrNull(JsonWriter& json, const std::optional<double>& value) {
  if (value.has_value()) {
    json.Number(value.value_or(0));
  } else {
    json.Null();
  }
}

bool Writes(std::string_view workload) {
  return workload == "fillseq" || workload == "fillrandom" || workload == "overwrite" ||
         workload == "mixed";
}

std::map<std::string, double> Growth(const std::map<std::string, double>& before,
                                     const std::map<std::string, double>& after) {
  std::map<std::string, double> growth;
  for (const auto& [name, value] : after) {
    const auto it = before.find(name);
    growth[name] = value - (it == before.end() ? 0 : it->second);
  }
  return growth;
}

// The current process's bytes written, if this factory writes real files.
std::optional<std::uint64_t> BytesWritten(const EngineFactory& factory) {
  if (!factory.RealFiles()) {
    return std::nullopt;
  }
  const Result<ProcessIo> io = CurrentProcessIo();
  return io.ok() ? std::optional<std::uint64_t>(io->bytes_written) : std::nullopt;
}

std::string BuildType() {
#ifdef NDEBUG
  std::string type = "optimized";
#else
  std::string type = "debug";
#endif
#ifdef FORGEDB_ENABLE_ASSERTS
  type += ", assertions";
#endif
#ifdef __SANITIZE_ADDRESS__
  type += ", AddressSanitizer";
#elif defined(__has_feature)
#if __has_feature(address_sanitizer)
  type += ", AddressSanitizer";
#endif
#endif
  return type;
}

std::string Compiler() {
#ifdef __clang__
  return "clang " __clang_version__;
#elif defined(_MSC_VER)
  return "MSVC " + std::to_string(_MSC_FULL_VER);
#elif defined(__GNUC__)
  return "gcc " __VERSION__;
#else
  return "unknown";
#endif
}

std::string Fixed(double value, int decimals) {
  std::array<char, 64> buffer{};
  const int n = std::snprintf(buffer.data(), buffer.size(), "%.*f", decimals, value);  // NOLINT
  return {buffer.data(), static_cast<std::size_t>(std::max(n, 0))};
}

double Median(std::vector<double> values) {
  std::ranges::sort(values);
  const std::size_t n = values.size();
  return n % 2 == 1 ? values[n / 2] : (values[(n / 2) - 1] + values[n / 2]) / 2;
}

}  // namespace

Result<RunResult> RunWorkload(EngineFactory& factory, std::string_view engine,
                              std::string_view workload, std::uint64_t run,
                              const BenchOptions& options, const std::filesystem::path& root) {
  const std::filesystem::path dir =
      root / (std::string(engine) + "-" + std::string(workload) + "-" + std::to_string(run));
  FORGEDB_RETURN_IF_ERROR(factory.Remove(dir));
  const EngineOptions engine_options{.sync = options.sync,
                                     .cache_bytes = options.cache_mb << 20U,
                                     .memtable_bytes = options.memtable_mb << 20U,
                                     .bloom_bits_per_key = 10};
  RunResult result;
  result.engine = std::string(engine);
  result.workload = std::string(workload);
  result.run = run;
  {
    FORGEDB_ASSIGN_OR_RETURN(const std::unique_ptr<Engine> db,
                             factory.Open(engine, dir, engine_options));
    const Keys keys(options.key_size);
    const Values values(options.value_size, options.seed + run);
    std::vector<std::uint64_t> permutation(options.num);
    for (std::uint64_t i = 0; i < options.num; ++i) {
      permutation[i] = i;
    }
    std::ranges::shuffle(permutation, std::mt19937_64(options.seed + run));

    if (workload != "fillseq" && workload != "fillrandom") {
      // Not measured: a full database, compactions finished.
      FORGEDB_RETURN_IF_ERROR(
          RunThreads(options, Workload("fillrandom", *db, options, keys, values, permutation))
              .status());
      FORGEDB_RETURN_IF_ERROR(db->WaitForBackgroundWork());
    }

    const std::optional<std::uint64_t> written_before = BytesWritten(factory);
    const std::map<std::string, double> counters_before = db->Counters();
    FORGEDB_ASSIGN_OR_RETURN(
        const PhaseResult phase,
        RunThreads(options, Workload(workload, *db, options, keys, values, permutation)));
    const auto settle_start = Clock::now();
    FORGEDB_RETURN_IF_ERROR(db->WaitForBackgroundWork());
    result.settle_seconds = std::chrono::duration<double>(Clock::now() - settle_start).count();
    const std::optional<std::uint64_t> written_after = BytesWritten(factory);

    result.ops = phase.total.ops;
    result.seconds = phase.seconds;
    result.bytes = phase.total.bytes;
    result.has_latency = phase.total.latency.Count() > 0;
    result.latency = phase.total.latency;
    result.counters = Growth(counters_before, db->Counters());
    // A workload that writes always writes its log: if the counters did not
    // move, they do not work here (Wine, for one), and the value is unknown.
    if (Writes(workload) && written_before && written_after && *written_after > *written_before &&
        phase.total.bytes_written > 0) {
      result.write_amplification = static_cast<double>(*written_after - *written_before) /
                                   static_cast<double>(phase.total.bytes_written);
    }
    FORGEDB_ASSIGN_OR_RETURN(const std::uint64_t disk_bytes, db->DiskBytes());
    result.space_amplification =
        static_cast<double>(disk_bytes) /
        static_cast<double>(options.num * (options.key_size + options.value_size));
  }  // closes the engine before its files go
  FORGEDB_RETURN_IF_ERROR(factory.Remove(dir));
  return result;
}

std::string ResultsJson(const BenchOptions& options, std::span<const RunResult> results) {
  JsonWriter json;
  json.BeginObject();
  json.Key("tool");
  json.String("forgedb_bench");
  json.Key("forgedb_version");
  json.String(VersionString());
  json.Key("build");
  json.BeginObject();
  json.Key("compiler");
  json.String(Compiler());
  json.Key("type");
  json.String(BuildType());
  json.EndObject();

  json.Key("options");
  json.BeginObject();
  json.Key("engines");
  json.BeginArray();
  for (const std::string& engine : options.engines) {
    json.String(engine);
  }
  json.EndArray();
  json.Key("workloads");
  json.BeginArray();
  for (const std::string& workload : options.workloads) {
    json.String(workload);
  }
  json.EndArray();
  for (const auto& [name, value] :
       {std::pair{"num", options.num}, std::pair{"key_size", options.key_size},
        std::pair{"value_size", options.value_size}, std::pair{"threads", options.threads},
        std::pair{"cache_mb", options.cache_mb}, std::pair{"memtable_mb", options.memtable_mb},
        std::pair{"runs", options.runs}, std::pair{"seed", options.seed}}) {
    json.Key(name);
    json.Number(value);
  }
  json.Key("sync");
  json.Bool(options.sync);
  json.Key("bloom_bits_per_key");
  json.Number(std::uint64_t{10});
  json.Key("compression");
  json.String("none");
  json.EndObject();

  json.Key("results");
  json.BeginArray();
  for (const RunResult& r : results) {
    json.BeginObject();
    json.Key("engine");
    json.String(r.engine);
    json.Key("workload");
    json.String(r.workload);
    json.Key("run");
    json.Number(r.run);
    json.Key("ops");
    json.Number(r.ops);
    json.Key("seconds");
    json.Number(r.seconds);
    json.Key("ops_per_second");
    json.Number(static_cast<double>(r.ops) / r.seconds);
    json.Key("mb_per_second");
    json.Number(static_cast<double>(r.bytes) / 1e6 / r.seconds);
    json.Key("latency_us");
    if (r.has_latency) {
      json.BeginObject();
      for (const auto& [name, nanoseconds] :
           {std::pair{"p50", r.latency.Percentile(0.5)},
            std::pair{"p99", r.latency.Percentile(0.99)}, std::pair{"max", r.latency.Max()}}) {
        json.Key(name);
        json.Number(static_cast<double>(nanoseconds) / 1000);
      }
      json.Key("mean");
      json.Number(r.latency.Mean() / 1000);
      json.EndObject();
    } else {
      json.Null();
    }
    json.Key("write_amplification");
    NumberOrNull(json, r.write_amplification);
    json.Key("space_amplification");
    NumberOrNull(json, r.space_amplification);
    json.Key("settle_seconds");
    json.Number(r.settle_seconds);
    json.Key("counters");
    json.BeginObject();
    for (const auto& [name, value] : r.counters) {
      json.Key(name);
      json.Number(value);
    }
    json.EndObject();
    json.EndObject();
  }
  json.EndArray();
  json.EndObject();
  return json.Take() + "\n";
}

std::string FormatRun(const RunResult& r) {
  std::string line = r.workload + "  " + r.engine + "  run " + std::to_string(r.run) + ": " +
                     std::to_string(r.ops) + " ops in " + Fixed(r.seconds, 2) + " s, " +
                     Fixed(static_cast<double>(r.ops) / r.seconds, 0) + " ops/s, " +
                     Fixed(static_cast<double>(r.bytes) / 1e6 / r.seconds, 1) + " MB/s";
  if (r.has_latency) {
    line += "; latency us p50 " + Fixed(static_cast<double>(r.latency.Percentile(0.5)) / 1000, 1) +
            " p99 " + Fixed(static_cast<double>(r.latency.Percentile(0.99)) / 1000, 1) + " max " +
            Fixed(static_cast<double>(r.latency.Max()) / 1000, 0);
  }
  if (r.write_amplification.has_value()) {
    line += "; write amp " + Fixed(r.write_amplification.value_or(0), 2);
  }
  if (r.space_amplification.has_value()) {
    line += "; space amp " + Fixed(r.space_amplification.value_or(0), 2);
  }
  line += "; settled in " + Fixed(r.settle_seconds, 2) + " s";
  return line;
}

int RunBench(std::span<const std::string> args, EngineFactory& factory, std::ostream& out,
             std::ostream& err) {
  const std::vector<std::string> engines = factory.Engines();
  Result<BenchOptions> parsed = ParseBenchOptions(args, engines);
  if (!parsed.ok()) {
    err << "forgedb_bench: " << parsed.status().ToString() << '\n' << BenchUsage(engines);
    return 2;
  }
  const BenchOptions& options = *parsed;

  std::filesystem::path root;
  if (options.db) {
    root = *options.db;
  } else {
    std::error_code error;
    std::random_device random;
    root = std::filesystem::temp_directory_path(error) /
           ("forgedb-bench-" + std::to_string(random()) + std::to_string(random()));
  }
  if (const Status status = factory.CreateRoot(root); !status.ok()) {
    err << "forgedb_bench: " << status.ToString() << '\n';
    return 1;
  }

  out << "forgedb_bench: ForgeDB " << VersionString() << ", " << Compiler() << ", " << BuildType()
      << '\n';
#ifndef NDEBUG
  out << "WARNING: not an optimized build; its numbers say nothing about performance\n";
#endif
  out << "num " << options.num << ", key " << options.key_size << " B, value " << options.value_size
      << " B, sync " << (options.sync ? "on" : "off") << ", threads " << options.threads
      << ", cache " << options.cache_mb << " MiB, runs " << options.runs << ", seed "
      << options.seed << "; databases in " << root.string() << '\n';

  // Runs are interleaved: run 1 of every engine, then run 2 of every engine, and
  // so on, so a slow spell of the machine (heat, power state, a background task)
  // falls on every engine alike instead of on one engine's runs.
  std::vector<RunResult> results;
  int exit_code = 0;
  for (const std::string& workload : options.workloads) {
    std::map<std::string, std::vector<double>> rates;
    std::set<std::string> failed;
    for (std::uint64_t run = 1; run <= options.runs; ++run) {
      for (const std::string& engine : options.engines) {
        if (failed.contains(engine)) {
          continue;
        }
        Result<RunResult> result = RunWorkload(factory, engine, workload, run, options, root);
        if (!result.ok()) {
          err << "forgedb_bench: " << workload << " on " << engine << ", run " << run << ": "
              << result.status().ToString() << '\n';
          exit_code = 1;
          failed.insert(engine);
          continue;
        }
        out << FormatRun(*result) << '\n';
        rates[engine].push_back(static_cast<double>(result->ops) / result->seconds);
        results.push_back(std::move(result).value());
      }
    }
    for (const std::string& engine : options.engines) {
      const std::vector<double>& engine_rates = rates[engine];
      if (engine_rates.size() > 1) {
        out << workload << "  " << engine << ": ops/s median " << Fixed(Median(engine_rates), 0)
            << ", min " << Fixed(*std::ranges::min_element(engine_rates), 0) << ", max "
            << Fixed(*std::ranges::max_element(engine_rates), 0) << " (" << engine_rates.size()
            << " runs)\n";
      }
    }
  }
  if (!options.db) {
    static_cast<void>(factory.Remove(root));  // the folder we made
  }
  if (options.json) {
    std::ofstream file(*options.json, std::ios::binary);
    file << ResultsJson(options, results);
    if (!file) {
      err << "forgedb_bench: could not write " << options.json->string() << '\n';
      return 1;
    }
    out << "results written to " << options.json->string() << '\n';
  }
  return exit_code;
}

}  // namespace forgedb::bench
