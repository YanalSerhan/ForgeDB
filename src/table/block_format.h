// What every block in a table file shares: the key ordering it is built with,
// and the 5-byte trailer that protects it on disk.
//
//   [ block contents ][ type: 1 byte ][ masked CRC-32C: fixed32 ]
//
// The CRC covers the contents and the type byte. The trailer is checked before
// a block's contents are used (docs/file-formats.md, "Block trailer").
#pragma once

#include <array>
#include <compare>
#include <cstddef>
#include <cstdint>
#include <string_view>

#include "forgedb/result.h"

namespace forgedb {

// How a block's keys are ordered: data and index blocks hold internal keys
// (CompareInternalKeys); tests use plain bytewise keys.
using KeyCompare = std::strong_ordering (*)(std::string_view, std::string_view) noexcept;

// The type byte. Written to disk: never renumber.
enum class BlockType : std::uint8_t {
  kUncompressed = 0,
};

inline constexpr std::size_t kBlockTrailerSize = 5;

// The trailer for `contents` stored as `type`.
[[nodiscard]] std::array<char, kBlockTrailerSize> MakeBlockTrailer(
    std::string_view contents, BlockType type = BlockType::kUncompressed) noexcept;

// Checks the trailer at the end of `stored` (contents + trailer) and returns
// the contents, a view into `stored`. Corruption if `stored` is too short for a
// trailer, the CRC does not match, or the type is unknown.
[[nodiscard]] Result<std::string_view> CheckBlockTrailer(std::string_view stored);

}  // namespace forgedb
