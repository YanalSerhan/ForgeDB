// Helpers shared by the command-line tools (tools/): formatting bytes and
// keys for people, and parsing arguments.
#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

namespace forgedb::tools {

// `bytes` as printable ASCII: printable characters as they are, backslash as
// "\\", tab, newline and carriage return as "\t", "\n", "\r", and every other
// byte (including UTF-8 sequences) as "\xHH". If `bytes` is longer than
// `max_bytes`, only that many are shown, followed by "...(N bytes)".
[[nodiscard]] std::string EscapeBytes(std::string_view bytes,
                                      std::size_t max_bytes = std::string_view::npos);

// An internal key as "'user key' @ sequence put|del", escaped; a malformed
// one as "malformed key '...'".
[[nodiscard]] std::string FormatInternalKey(std::string_view internal_key);

// "1 record", "2 records": `n` and the noun, plural unless n is 1 (the
// singular plus "s" if no plural is given).
[[nodiscard]] std::string Count(std::uint64_t n, std::string_view singular,
                                std::string_view plural = {});

// A path from a UTF-8 string (the tools take their arguments as UTF-8).
[[nodiscard]] std::filesystem::path PathFromUtf8(std::string_view utf8);

// A decimal number with no sign, spaces or overflow; nullopt otherwise.
[[nodiscard]] std::optional<std::uint64_t> ParseUnsigned(std::string_view text) noexcept;

}  // namespace forgedb::tools
