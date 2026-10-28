// forgedb_cli: see tools/cli.h.
#include <cstddef>
#include <iostream>
#include <memory>
#include <span>
#include <string>
#include <vector>

#include "env/win_file_system.h"
#include "env/win_util.h"
#include "tools/cli.h"

int wmain(int argc, wchar_t** argv) {  // NOLINT(misc-use-internal-linkage): the entry point
  std::vector<std::string> args;
  for (const wchar_t* arg : std::span<wchar_t*>(argv, static_cast<std::size_t>(argc)).subspan(1)) {
    args.push_back(forgedb::win::WideToUtf8(arg));
  }
  return forgedb::tools::RunCli(args, std::make_shared<forgedb::WinFileSystem>(), std::cout,
                                std::cerr);
}
