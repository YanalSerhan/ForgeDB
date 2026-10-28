#include "tools/tool_util.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

#include "db/internal_key.h"

namespace forgedb::tools {

std::string EscapeBytes(std::string_view bytes, std::size_t max_bytes) {
  constexpr std::array<char, 16> kHex = {'0', '1', '2', '3', '4', '5', '6', '7',
                                         '8', '9', 'A', 'B', 'C', 'D', 'E', 'F'};
  const std::string_view shown = bytes.substr(0, max_bytes);
  std::string out;
  out.reserve(shown.size());
  for (const char c : shown) {
    const auto byte = static_cast<unsigned char>(c);
    if (c == '\\') {
      out += "\\\\";
    } else if (c == '\t') {
      out += "\\t";
    } else if (c == '\n') {
      out += "\\n";
    } else if (c == '\r') {
      out += "\\r";
    } else if (byte >= 0x20 && byte < 0x7F) {
      out += c;
    } else {
      out += "\\x";
      out += kHex.at(byte >> 4U);
      out += kHex.at(byte & 0x0FU);
    }
  }
  if (shown.size() < bytes.size()) {
    out += "...(" + std::to_string(bytes.size()) + " bytes)";
  }
  return out;
}

std::string FormatInternalKey(std::string_view internal_key) {
  const std::optional<ParsedInternalKey> parsed = ParseInternalKey(internal_key);
  if (!parsed) {
    return "malformed key '" + EscapeBytes(internal_key) + "'";
  }
  return "'" + EscapeBytes(parsed->user_key) + "' @ " + std::to_string(parsed->sequence) +
         (parsed->type == ValueType::kDeletion ? " del" : " put");
}

std::string Count(std::uint64_t n, std::string_view singular, std::string_view plural) {
  std::string out = std::to_string(n) + " ";
  if (n == 1) {
    out += singular;
  } else if (plural.empty()) {
    out.append(singular).append("s");
  } else {
    out += plural;
  }
  return out;
}

std::filesystem::path PathFromUtf8(std::string_view utf8) {
  // char8_t is the type that tells std::filesystem the bytes are UTF-8.
  return {std::u8string(utf8.begin(), utf8.end())};
}

std::optional<std::uint64_t> ParseUnsigned(std::string_view text) noexcept {
  if (text.empty()) {
    return std::nullopt;
  }
  std::uint64_t value = 0;
  for (const char c : text) {
    if (c < '0' || c > '9') {
      return std::nullopt;
    }
    const auto digit = static_cast<std::uint64_t>(c - '0');
    if (value > (UINT64_MAX - digit) / 10) {
      return std::nullopt;
    }
    value = (value * 10) + digit;
  }
  return value;
}

}  // namespace forgedb::tools
