// How many bytes this process has read and written (TODO 5.1: the benchmark
// measures write amplification with it, the same way for every engine).
//
// Windows: GetProcessIoCounters, which counts the bytes passed to every read
// and write call of the process (files, and also pipes and the console),
// whether or not they reached the disk yet. A benchmark's database writes are
// almost all of it; what the benchmark prints is a few kilobytes.
#pragma once

#include <cstdint>

#include "forgedb/result.h"

namespace forgedb {

struct ProcessIo {
  std::uint64_t bytes_read = 0;
  std::uint64_t bytes_written = 0;
};

// The counts since the process started.
[[nodiscard]] Result<ProcessIo> CurrentProcessIo();

}  // namespace forgedb
