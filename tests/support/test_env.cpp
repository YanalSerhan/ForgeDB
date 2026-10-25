#include "support/test_env.h"

#include <charconv>
#include <cstdio>
#include <cstdlib>
#include <system_error>

namespace forgedb {

std::optional<std::string> GetEnv(const char* name) {
#ifdef _MSC_VER  // MSVC and clang-cl
  // _dupenv_s, not std::getenv: the MSVC runtime deprecates getenv (C4996).
  char* value = nullptr;
  std::size_t length = 0;
  if (_dupenv_s(&value, &length, name) != 0 || value == nullptr) {
    return std::nullopt;
  }
  std::string result(value);
  std::free(value);  // NOLINT(cppcoreguidelines-no-malloc,cppcoreguidelines-owning-memory)
  return result;
#else
  const char* value = std::getenv(name);  // NOLINT(concurrency-mt-unsafe): read before threads
  return value == nullptr ? std::nullopt : std::optional<std::string>(value);
#endif
}

std::uint64_t GetEnvNumber(const char* name, std::uint64_t fallback) {
  const std::optional<std::string> value = GetEnv(name);
  if (!value) {
    return fallback;
  }
  // std::from_chars, not std::stoull: no exceptions (clang-tidy's clang-cl
  // mode compiles tests without them).
  std::uint64_t number = 0;
  const char* const end = value->data() + value->size();
  const auto [parsed_to, error] = std::from_chars(value->data(), end, number);
  if (error == std::errc() && parsed_to == end && !value->empty()) {
    return number;
  }
  std::fprintf(stderr, "%s=%s is not a number\n", name, value->c_str());  // NOLINT
  std::abort();
}

}  // namespace forgedb
