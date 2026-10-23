#include "env/win_util.h"

#include <algorithm>
#include <array>
#include <climits>
#include <string>

namespace forgedb::win {

std::string WideToUtf8(std::wstring_view wide) {
  if (wide.empty()) {
    return {};
  }
  if (wide.size() > static_cast<std::size_t>(INT_MAX)) {
    return "<string too long to convert>";
  }
  // The API takes an explicit length (int), so `wide` needn't be NUL-terminated.
  const wchar_t* const chars = wide.data();
  const int wide_length = static_cast<int>(wide.size());
  // No WC_ERR_INVALID_CHARS: an unpaired surrogate becomes U+FFFD instead of
  // failing the whole conversion.
  const int length =
      ::WideCharToMultiByte(CP_UTF8, 0, chars, wide_length, nullptr, 0, nullptr, nullptr);
  if (length <= 0) {
    return "<invalid UTF-16>";
  }
  std::string utf8(static_cast<std::size_t>(length), '\0');
  ::WideCharToMultiByte(CP_UTF8, 0, chars, wide_length, utf8.data(), length, nullptr, nullptr);
  return utf8;
}

std::string PathToUtf8(const std::filesystem::path& path) { return WideToUtf8(path.native()); }

std::string ErrorMessage(DWORD error) {
  std::array<wchar_t, 512> buffer{};
  const DWORD length =
      ::FormatMessageW(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, nullptr, error,
                       0, buffer.data(), static_cast<DWORD>(buffer.size()), nullptr);
  if (length == 0) {
    return "unknown error";
  }
  std::wstring_view message(buffer.data(), length);
  // System messages end with "\r\n" (sometimes after a space).
  while (!message.empty() &&
         (message.back() == L'\r' || message.back() == L'\n' || message.back() == L' ')) {
    message.remove_suffix(1);
  }
  return WideToUtf8(message);
}

Status ErrorStatus(DWORD error, std::string_view operation, const std::filesystem::path& path) {
  std::string message;
  message.append(operation);
  message.append(" '");
  message.append(PathToUtf8(path));
  message.append("'");
  std::string detail = ErrorMessage(error);
  detail.append(" (Win32 error ");
  detail.append(std::to_string(error));
  detail.append(")");

  switch (error) {
    case ERROR_FILE_NOT_FOUND:
    case ERROR_PATH_NOT_FOUND:
      return Status::NotFound(message, detail);
    case ERROR_SHARING_VIOLATION:
    case ERROR_LOCK_VIOLATION:
      return Status::Busy(message, detail);
    default:
      return Status::IOError(message, detail);
  }
}

}  // namespace forgedb::win
