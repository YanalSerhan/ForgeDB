// Reading environment variables in tests (run-time overrides such as
// FORGEDB_MODEL_OPS or FORGEDB_CRASH_CYCLES).
#pragma once

#include <cstdint>
#include <optional>
#include <string>

namespace forgedb {

// The variable's value, or nullopt if it is not set.
[[nodiscard]] std::optional<std::string> GetEnv(const char* name);

// The variable as an unsigned integer, or `fallback` if it is not set.
// Aborts with a message if it is set but not a number.
[[nodiscard]] std::uint64_t GetEnvNumber(const char* name, std::uint64_t fallback);

}  // namespace forgedb
