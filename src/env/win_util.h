// Win32 helpers for src/env/: UTF-16 to UTF-8 conversion and turning Win32
// error codes into Status values.
#pragma once

#include <windows.h>

#include <filesystem>
#include <string>
#include <string_view>

#include "forgedb/status.h"

namespace forgedb::win {

// UTF-16 to UTF-8. Never fails: invalid UTF-16 (an unpaired surrogate, which
// NTFS file names may contain) becomes U+FFFD.
[[nodiscard]] std::string WideToUtf8(std::wstring_view wide);

[[nodiscard]] std::string PathToUtf8(const std::filesystem::path& path);

// The system's description of a Win32 error code, in UTF-8 without the
// trailing newline, e.g. "The system cannot find the file specified."
[[nodiscard]] std::string ErrorMessage(DWORD error);

// The Status for a failed Win32 call on `path`:
//   ERROR_FILE_NOT_FOUND, ERROR_PATH_NOT_FOUND          -> NotFound
//   ERROR_SHARING_VIOLATION, ERROR_LOCK_VIOLATION       -> Busy
//   anything else                                       -> IOError
// with the message "<operation> '<path>': <system message> (Win32 error <n>)".
[[nodiscard]] Status ErrorStatus(DWORD error, std::string_view operation,
                                 const std::filesystem::path& path);

}  // namespace forgedb::win
