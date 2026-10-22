#include "forgedb/check.h"

#include <array>
#include <charconv>
#include <cstdio>
#include <cstdlib>
#include <system_error>

namespace forgedb::detail {

void CheckFailed(const char* expression, std::source_location location) noexcept {
  // Plain fputs instead of printf-style formatting: nothing here may allocate or
  // fail in interesting ways, because the process is already in a bad state.
  std::array<char, 16> line{};
  const auto [end, ec] = std::to_chars(line.data(), line.data() + line.size() - 1, location.line());
  if (ec != std::errc{}) {
    line[0] = '?';
  }

  std::fputs(location.file_name(), stderr);
  std::fputs("(", stderr);
  std::fputs(line.data(), stderr);
  std::fputs("): ", stderr);
  std::fputs(location.function_name(), stderr);
  std::fputs(": FORGEDB_CHECK failed: ", stderr);
  std::fputs(expression, stderr);
  std::fputs("\n", stderr);
  std::fflush(stderr);
  std::_Exit(3);
}

}  // namespace forgedb::detail
