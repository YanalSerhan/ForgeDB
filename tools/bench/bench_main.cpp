// forgedb_bench: see tools/bench/bench.h and tools/bench/bench_options.h.
#include <cstddef>
#include <iostream>
#include <memory>
#include <span>
#include <string>
#include <vector>

#include "env/win_util.h"
#include "tools/bench/bench.h"
#include "tools/bench/engine.h"

int wmain(int argc, wchar_t** argv) {  // NOLINT(misc-use-internal-linkage): the entry point
  std::vector<std::string> args;
  for (const wchar_t* arg : std::span<wchar_t*>(argv, static_cast<std::size_t>(argc)).subspan(1)) {
    args.push_back(forgedb::win::WideToUtf8(arg));
  }
  const std::unique_ptr<forgedb::bench::EngineFactory> factory =
      forgedb::bench::NewDiskEngineFactory();
  return forgedb::bench::RunBench(args, *factory, std::cout, std::cerr);
}
