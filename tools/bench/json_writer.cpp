#include "tools/bench/json_writer.h"

#include <array>
#include <charconv>
#include <cmath>
#include <cstdint>
#include <string>
#include <string_view>
#include <system_error>

#include "forgedb/check.h"

namespace forgedb::bench {

std::string JsonWriter::Quote(std::string_view value) {
  static constexpr std::string_view kHex = "0123456789abcdef";
  std::string out = "\"";
  for (const char c : value) {
    const auto byte = static_cast<unsigned char>(c);
    switch (c) {
      case '"':
        out += "\\\"";
        break;
      case '\\':
        out += "\\\\";
        break;
      case '\n':
        out += "\\n";
        break;
      case '\r':
        out += "\\r";
        break;
      case '\t':
        out += "\\t";
        break;
      default:
        if (byte < 0x20) {
          out += "\\u00";
          out += kHex[byte >> 4U];
          out += kHex[byte & 0xFU];
        } else {
          out += c;  // UTF-8 passes through
        }
    }
  }
  out += '"';
  return out;
}

void JsonWriter::NewLine() {
  out_ += '\n';
  out_.append(levels_.size() * 2, ' ');
}

void JsonWriter::BeforeValue() {
  if (after_key_) {
    after_key_ = false;
    return;
  }
  if (!levels_.empty()) {
    if (!levels_.back().empty) {
      out_ += ',';
    }
    levels_.back().empty = false;
    NewLine();
  }
}

void JsonWriter::Open(char bracket) {
  BeforeValue();
  out_ += bracket;
  levels_.push_back({});
}

void JsonWriter::Close(char bracket) {
  FORGEDB_DCHECK(!(levels_.empty() || after_key_));
  const bool empty = levels_.back().empty;
  levels_.pop_back();
  if (!empty) {
    NewLine();
  }
  out_ += bracket;
}

void JsonWriter::Key(std::string_view name) {
  FORGEDB_DCHECK(!(levels_.empty() || after_key_));
  BeforeValue();
  out_ += Quote(name);
  out_ += ": ";
  after_key_ = true;
}

void JsonWriter::String(std::string_view value) {
  BeforeValue();
  out_ += Quote(value);
}

void JsonWriter::Number(double value) {
  if (!std::isfinite(value)) {
    Null();
    return;
  }
  BeforeValue();
  std::array<char, 64> buffer{};
  const auto [end, error] = std::to_chars(buffer.data(), buffer.data() + buffer.size(), value);
  FORGEDB_CHECK(error == std::errc());
  out_.append(buffer.data(), end);
}

void JsonWriter::Number(std::uint64_t value) {
  BeforeValue();
  out_ += std::to_string(value);
}

void JsonWriter::Number(std::int64_t value) {
  BeforeValue();
  out_ += std::to_string(value);
}

void JsonWriter::Bool(bool value) {
  BeforeValue();
  out_ += value ? "true" : "false";
}

void JsonWriter::Null() {
  BeforeValue();
  out_ += "null";
}

}  // namespace forgedb::bench
