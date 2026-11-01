#include "tools/bench/bench_options.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "forgedb/db.h"
#include "forgedb/result.h"
#include "forgedb/status.h"

#include "tools/tool_util.h"

namespace forgedb::bench {
namespace {

std::vector<std::string> SplitList(std::string_view text) {
  std::vector<std::string> items;
  while (!text.empty()) {
    const std::size_t comma = text.find(',');
    const std::string_view item = text.substr(0, comma);
    if (!item.empty()) {
      items.emplace_back(item);
    }
    text.remove_prefix(comma == std::string_view::npos ? text.size() : comma + 1);
  }
  return items;
}

Status Bad(std::string_view message) { return Status::InvalidArgument(message); }

// A number in [low, high] for option `name`.
Result<std::uint64_t> Number(std::string_view name, std::string_view text, std::uint64_t low,
                             std::uint64_t high) {
  const std::optional<std::uint64_t> value = tools::ParseUnsigned(text);
  if (!value || *value < low || *value > high) {
    return Bad("--" + std::string(name) + " must be a number from " + std::to_string(low) + " to " +
               std::to_string(high) + ", not '" + std::string(text) + "'");
  }
  return *value;
}

std::size_t Digits(std::uint64_t n) { return std::to_string(n).size(); }

// A comma-separated list whose items must all be in `known`.
template <typename Known>
Result<std::vector<std::string>> KnownList(std::string_view name, std::string_view value,
                                           const Known& known) {
  std::vector<std::string> items = SplitList(value);
  if (items.empty()) {
    return Bad("--" + std::string(name) + " needs at least one item");
  }
  for (const std::string& item : items) {
    if (std::ranges::find(known, item) == std::ranges::end(known)) {
      return Bad("unknown " + std::string(name == "engine" ? "engine" : "workload") + " '" + item +
                 "'");
    }
  }
  return items;
}

// The numeric options: their name, field and range.
struct NumericOption {
  std::string_view name;
  std::uint64_t BenchOptions::* field;
  std::uint64_t low;
  std::uint64_t high;
};

constexpr std::array<NumericOption, 7> kNumericOptions = {{
    {.name = "num", .field = &BenchOptions::num, .low = 1, .high = std::uint64_t{1} << 40U},
    {.name = "key_size", .field = &BenchOptions::key_size, .low = 8, .high = kMaxKeySize},
    {.name = "value_size",
     .field = &BenchOptions::value_size,
     .low = 0,
     .high = std::uint64_t{16} << 20U},
    {.name = "threads", .field = &BenchOptions::threads, .low = 1, .high = 256},
    {.name = "cache_mb",
     .field = &BenchOptions::cache_mb,
     .low = 0,
     .high = std::uint64_t{1} << 20U},
    {.name = "runs", .field = &BenchOptions::runs, .low = 1, .high = 1000},
    {.name = "seed", .field = &BenchOptions::seed, .low = 0, .high = UINT64_MAX},
}};

Status SetOption(BenchOptions& options, std::string_view name, std::string_view value,
                 std::span<const std::string> known_engines) {
  for (const NumericOption& numeric : kNumericOptions) {
    if (name == numeric.name) {
      FORGEDB_ASSIGN_OR_RETURN(options.*numeric.field,
                               Number(name, value, numeric.low, numeric.high));
      return Status::OK();
    }
  }
  if (name == "engine") {
    FORGEDB_ASSIGN_OR_RETURN(options.engines, KnownList(name, value, known_engines));
  } else if (name == "workloads") {
    FORGEDB_ASSIGN_OR_RETURN(options.workloads, KnownList(name, value, kWorkloads));
  } else if (name == "sync") {
    FORGEDB_ASSIGN_OR_RETURN(const std::uint64_t sync, Number(name, value, 0, 1));
    options.sync = sync == 1;
  } else if (name == "db") {
    options.db = tools::PathFromUtf8(value);
  } else if (name == "json") {
    options.json = tools::PathFromUtf8(value);
  } else {
    return Bad("unknown option --" + std::string(name));
  }
  return Status::OK();
}

}  // namespace

std::string BenchUsage(std::span<const std::string> known_engines) {
  std::string engines;
  for (const std::string& engine : known_engines) {
    engines += (engines.empty() ? "" : ",") + engine;
  }
  std::string workloads;
  for (const std::string_view workload : kWorkloads) {
    workloads += (workloads.empty() ? "" : ",") + std::string(workload);
  }
  return "usage: forgedb_bench [--name=value]...\n"
         "  --engine=" +
         engines +
         "    engines, comma-separated (built in: those listed)\n"
         "  --workloads=" +
         workloads +
         "\n"
         "  --num=1000000  --key_size=16  --value_size=100  --sync=0  --threads=1\n"
         "  --cache_mb=8  --runs=1  --seed=1  --db=<dir>  --json=<file>\n";
}

Result<BenchOptions> ParseBenchOptions(std::span<const std::string> args,
                                       std::span<const std::string> known_engines) {
  BenchOptions options;
  for (const std::string& arg : args) {
    const std::string_view text = arg;
    const std::size_t equals = text.find('=');
    if (!text.starts_with("--") || equals == std::string_view::npos) {
      return Bad("expected --name=value, got '" + arg + "'");
    }
    FORGEDB_RETURN_IF_ERROR(
        SetOption(options, text.substr(2, equals - 2), text.substr(equals + 1), known_engines));
  }
  // Keys are 2i (present) and 2i + 1 (missing), zero-padded to key_size.
  if (Digits((2 * options.num) + 1) > options.key_size) {
    return Bad("--key_size=" + std::to_string(options.key_size) +
               " is too short for --num=" + std::to_string(options.num) + " (keys up to " +
               std::to_string((2 * options.num) + 1) + ")");
  }
  return options;
}

}  // namespace forgedb::bench
