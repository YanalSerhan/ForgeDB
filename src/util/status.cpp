#include "forgedb/status.h"

#include "forgedb/check.h"

namespace forgedb {

Status::Status(Code code, std::string_view msg, std::string_view detail) : code_(code) {
  FORGEDB_DCHECK(code != Code::kOk);
  message_.reserve(msg.size() + (detail.empty() ? 0 : 2 + detail.size()));
  message_.append(msg);
  if (!detail.empty()) {
    message_.append(": ");
    message_.append(detail);
  }
}

std::string Status::ToString() const {
  std::string result(CodeName(code_));
  if (!ok() && !message_.empty()) {
    result.append(": ");
    result.append(message_);
  }
  return result;
}

std::string_view CodeName(Status::Code code) noexcept {
  switch (code) {
    case Status::Code::kOk:
      return "OK";
    case Status::Code::kNotFound:
      return "NotFound";
    case Status::Code::kCorruption:
      return "Corruption";
    case Status::Code::kNotSupported:
      return "NotSupported";
    case Status::Code::kInvalidArgument:
      return "InvalidArgument";
    case Status::Code::kIOError:
      return "IOError";
    case Status::Code::kBusy:
      return "Busy";
    case Status::Code::kInternal:
      return "Internal";
  }
  return "Unknown";  // only reachable if someone casts an invalid number to Code
}

}  // namespace forgedb
