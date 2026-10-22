// Status: the outcome of an operation, either OK or an error code plus a message.
//
// Errors are values: ForgeDB never throws exceptions across its public API.
// The class is [[nodiscard]], so ignoring a returned Status is a compile warning
// (an error with /WX).
#pragma once

#include <cstdint>
#include <string>
#include <string_view>

namespace forgedb {

class [[nodiscard]] Status {
 public:
  enum class Code : std::uint8_t {
    kOk = 0,
    kNotFound,         // the key or file does not exist
    kCorruption,       // stored bytes failed a checksum, length or format check
    kNotSupported,     // the operation or format version is not supported
    kInvalidArgument,  // the caller passed something invalid
    kIOError,          // the operating system reported a failure
    kBusy,             // held by someone else (database LOCK, file sharing violation)
    kInternal,         // ForgeDB detected a bug in itself
  };

  // An OK status. It holds an empty std::string, so creating, copying and
  // testing an OK Status never allocates.
  Status() noexcept = default;

  static Status OK() noexcept { return {}; }

  // Each factory builds the message "msg" or, when detail is given, "msg: detail",
  // e.g. IOError("C:\\db\\000005.log", "The process cannot access the file...").
  static Status NotFound(std::string_view msg, std::string_view detail = {}) {
    return {Code::kNotFound, msg, detail};
  }
  static Status Corruption(std::string_view msg, std::string_view detail = {}) {
    return {Code::kCorruption, msg, detail};
  }
  static Status NotSupported(std::string_view msg, std::string_view detail = {}) {
    return {Code::kNotSupported, msg, detail};
  }
  static Status InvalidArgument(std::string_view msg, std::string_view detail = {}) {
    return {Code::kInvalidArgument, msg, detail};
  }
  static Status IOError(std::string_view msg, std::string_view detail = {}) {
    return {Code::kIOError, msg, detail};
  }
  static Status Busy(std::string_view msg, std::string_view detail = {}) {
    return {Code::kBusy, msg, detail};
  }
  static Status Internal(std::string_view msg, std::string_view detail = {}) {
    return {Code::kInternal, msg, detail};
  }

  [[nodiscard]] bool ok() const noexcept { return code_ == Code::kOk; }
  [[nodiscard]] Code code() const noexcept { return code_; }
  [[nodiscard]] const std::string& message() const noexcept { return message_; }

  // "OK", "Corruption", or "Corruption: <message>".
  [[nodiscard]] std::string ToString() const;

  // Same code and same message.
  friend bool operator==(const Status&, const Status&) = default;

 private:
  Status(Code code, std::string_view msg, std::string_view detail);

  Code code_ = Code::kOk;
  std::string message_;
};

// "OK", "NotFound", "Corruption", ...
[[nodiscard]] std::string_view CodeName(Status::Code code) noexcept;

}  // namespace forgedb

// Evaluates `expr` (a Status) and returns it from the current function if it
// is not OK. Works in functions returning Status or Result<T>.
// A macro because it has to `return` from the caller.
// NOLINTNEXTLINE(cppcoreguidelines-macro-usage)
#define FORGEDB_RETURN_IF_ERROR(expr)                    \
  do {                                                   \
    ::forgedb::Status forgedb_return_if_error_ = (expr); \
    if (!forgedb_return_if_error_.ok()) {                \
      return forgedb_return_if_error_;                   \
    }                                                    \
  } while (false)
