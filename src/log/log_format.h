// The write-ahead log's physical format, shared by log::Writer and
// log::Reader. Byte layout: docs/file-formats.md, "Write-ahead log".
//
// A log is a sequence of 32 KiB blocks. Each record is stored as one or more
// fragments; a fragment never crosses a block boundary:
//
//   fragment := masked CRC-32C (fixed32) | length (fixed16) | type (1 byte) | payload
//
// The CRC covers the type byte and the payload. A record that fits in the
// rest of the current block is one kFull fragment; a longer one is kFirst,
// zero or more kMiddle, then kLast. When fewer than kHeaderSize bytes are
// left in a block, they are filled with zeros and the next fragment starts in
// the next block.
#pragma once

#include <cstddef>
#include <cstdint>

namespace forgedb::log {

// Fragment types. Written to disk: never renumber.
enum class RecordType : std::uint8_t {
  kZero = 0,  // never written; zero-filled space (padding, or a file extended by a crash)
  kFull = 1,
  kFirst = 2,
  kMiddle = 3,
  kLast = 4,
};
inline constexpr std::uint8_t kMaxRecordType = static_cast<std::uint8_t>(RecordType::kLast);

inline constexpr std::size_t kBlockSize = 32768;
// CRC (4) + length (2) + type (1).
inline constexpr std::size_t kHeaderSize = 4 + 2 + 1;

}  // namespace forgedb::log
