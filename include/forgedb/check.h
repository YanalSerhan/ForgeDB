// Invariant checks.
//
//   FORGEDB_CHECK(cond)   always compiled in. For cheap conditions whose violation
//                         would corrupt memory or data if execution continued.
//   FORGEDB_DCHECK(cond)  compiled in only when FORGEDB_ENABLE_ASSERTS is defined
//                         (debug and ASan presets). For everything else, including
//                         expensive checks. When disabled the condition is still
//                         type-checked but never evaluated.
//
// Assertions are controlled by FORGEDB_ENABLE_ASSERTS rather than NDEBUG so that
// ASan builds (RelWithDebInfo, which defines NDEBUG) still check invariants.
#pragma once

#include <source_location>

namespace forgedb::detail {

// Prints "file(line): function: FORGEDB_CHECK failed: expression" to stderr and
// ends the process with exit code 3, the code std::abort() uses on Windows.
// It deliberately does not call std::abort(): in debug builds the CRT then shows
// a modal "abort() has been called" dialog, which would hang an unattended CI job.
[[noreturn]] void CheckFailed(const char* expression, std::source_location location =
                                                          std::source_location::current()) noexcept;

}  // namespace forgedb::detail

// Macros are needed here to capture the condition's source text (#condition).
// NOLINTNEXTLINE(cppcoreguidelines-macro-usage)
#define FORGEDB_CHECK(condition) \
  ((condition) ? static_cast<void>(0) : ::forgedb::detail::CheckFailed(#condition))

#ifdef FORGEDB_ENABLE_ASSERTS
// NOLINTNEXTLINE(cppcoreguidelines-macro-usage)
#define FORGEDB_DCHECK(condition) FORGEDB_CHECK(condition)
#else
// sizeof() keeps the expression compiled (so it can't rot) but never evaluates it.
// NOLINTNEXTLINE(cppcoreguidelines-macro-usage)
#define FORGEDB_DCHECK(condition) static_cast<void>(sizeof(!(condition)))
#endif
