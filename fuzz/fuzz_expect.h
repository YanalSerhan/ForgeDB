// Shared by the fuzz targets: checking a promise so that libFuzzer saves the
// input that broke it.
#pragma once

#include <cstdio>
#include <cstdlib>
#include <source_location>

namespace forgedb::fuzz {

// Like FORGEDB_CHECK, but aborts: FORGEDB_CHECK ends the process with
// _Exit, which bypasses libFuzzer's handler, so the input would not be saved.
inline void Expect(bool condition,
                   std::source_location location = std::source_location::current()) {
  if (!condition) {
    std::fprintf(stderr, "%s(%u): broken promise\n",  // NOLINT(cppcoreguidelines-pro-type-vararg)
                 location.file_name(), static_cast<unsigned>(location.line()));
    std::abort();
  }
}

}  // namespace forgedb::fuzz
