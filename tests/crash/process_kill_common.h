// Shared by the process-kill test (TODO 2.6) and its child process: the keys
// the child writes and the value each key must have.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace forgedb::process_kill {

// The most keys the child writes in one batch (one acknowledged write).
inline constexpr std::size_t kMaxBatchKeys = 5;

// Key number `n` of run `run`: "r0007-000123". Sorted by run, then number.
inline std::string KeyFor(std::uint64_t run, std::uint64_t n) {
  const std::string run_text = std::to_string(run);
  const std::string n_text = std::to_string(n);
  return "r" + std::string(run_text.size() < 4 ? 4 - run_text.size() : 0, '0') + run_text + "-" +
         std::string(n_text.size() < 6 ? 6 - n_text.size() : 0, '0') + n_text;
}

// The value stored under `key`: 0 to 2999 bytes derived from the key, so the
// parent can tell a correct value from a torn or stale one.
inline std::string ValueFor(std::string_view key) {
  std::uint64_t hash = 14695981039346656037ULL;  // FNV-1a
  for (const char c : key) {
    hash = (hash ^ static_cast<unsigned char>(c)) * 1099511628211ULL;
  }
  std::string value(static_cast<std::size_t>(hash % 3000), '\0');
  for (std::size_t i = 0; i < value.size(); ++i) {
    value[i] = static_cast<char>(key[i % key.size()] + static_cast<char>(i % 7));
  }
  return value;
}

}  // namespace forgedb::process_kill
