#include <windows.h>

#include <string>

#include "forgedb/result.h"
#include "forgedb/status.h"

#include "env/process_io.h"
#include "env/win_util.h"

namespace forgedb {

Result<ProcessIo> CurrentProcessIo() {
  IO_COUNTERS counters{};
  if (::GetProcessIoCounters(::GetCurrentProcess(), &counters) == 0) {
    const DWORD error = ::GetLastError();
    return Status::IOError("GetProcessIoCounters: " + win::ErrorMessage(error),
                           "Win32 error " + std::to_string(error));
  }
  return ProcessIo{.bytes_read = counters.ReadTransferCount,
                   .bytes_written = counters.WriteTransferCount};
}

}  // namespace forgedb
