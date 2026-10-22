// Byte encodings shared by every ForgeDB file format (docs/file-formats.md,
// "Primitive encodings"):
//
//   fixed32 / fixed64   4 / 8 bytes, little-endian
//   varint32 / varint64 LEB128: 7 bits per byte, low bits first, high bit set on
//                       every byte except the last (1-5 / 1-10 bytes)
//   length-prefixed     varint32 length, then that many bytes
//
// Buffers are std::string / std::string_view of char: the same types that hold
// keys and values, so encoded data never needs converting between byte types.
#pragma once

#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <optional>
#include <string>
#include <string_view>

namespace forgedb {

// The fixed-width helpers copy the CPU's in-memory representation, which equals
// the little-endian file format only on a little-endian CPU. x64 always is; the
// assertion documents the assumption and would stop a port that breaks it.
static_assert(std::endian::native == std::endian::little,
              "ForgeDB's fixed-width encoders assume a little-endian CPU");

inline constexpr std::size_t kMaxVarint32Bytes = 5;
inline constexpr std::size_t kMaxVarint64Bytes = 10;

// ---------------------------------------------------------------------------
// Fixed width, unchecked: the caller guarantees 4 or 8 bytes at the pointer.
// std::memcpy rather than a pointer cast: it is well-defined for unaligned
// addresses, and compilers turn it into a single load or store.
// ---------------------------------------------------------------------------
inline void EncodeFixed32(char* dst, std::uint32_t value) noexcept {
  std::memcpy(dst, &value, sizeof(value));
}
inline void EncodeFixed64(char* dst, std::uint64_t value) noexcept {
  std::memcpy(dst, &value, sizeof(value));
}

[[nodiscard]] inline std::uint32_t DecodeFixed32(const char* src) noexcept {
  std::uint32_t value = 0;
  std::memcpy(&value, src, sizeof(value));
  return value;
}
[[nodiscard]] inline std::uint64_t DecodeFixed64(const char* src) noexcept {
  std::uint64_t value = 0;
  std::memcpy(&value, src, sizeof(value));
  return value;
}

// ---------------------------------------------------------------------------
// Encoding
// ---------------------------------------------------------------------------

// Write a varint at dst (which must have room for kMaxVarint{32,64}Bytes) and
// return the position just past it.
char* EncodeVarint32(char* dst, std::uint32_t value) noexcept;
char* EncodeVarint64(char* dst, std::uint64_t value) noexcept;

// Number of bytes EncodeVarint64 writes for `value` (1-10).
[[nodiscard]] std::size_t VarintLength(std::uint64_t value) noexcept;

// Append to dst.
void PutFixed32(std::string& dst, std::uint32_t value);
void PutFixed64(std::string& dst, std::uint64_t value);
void PutVarint32(std::string& dst, std::uint32_t value);
void PutVarint64(std::string& dst, std::uint64_t value);
// Precondition: value.size() fits in 32 bits.
void PutLengthPrefixed(std::string& dst, std::string_view value);

// ---------------------------------------------------------------------------
// Bounds-checked decoding of untrusted bytes
//
// Each Get* reads from the front of `input`. On success it returns the value
// and advances `input` past it. On truncated or malformed input it returns
// std::nullopt and leaves `input` unchanged. They never read outside `input`.
// ---------------------------------------------------------------------------
[[nodiscard]] std::optional<std::uint32_t> GetFixed32(std::string_view& input) noexcept;
[[nodiscard]] std::optional<std::uint64_t> GetFixed64(std::string_view& input) noexcept;
[[nodiscard]] std::optional<std::uint32_t> GetVarint32(std::string_view& input) noexcept;
[[nodiscard]] std::optional<std::uint64_t> GetVarint64(std::string_view& input) noexcept;
// Returns a view INTO input's buffer; it is valid only as long as that buffer is.
[[nodiscard]] std::optional<std::string_view> GetLengthPrefixed(std::string_view& input) noexcept;

// Pointer form for tight loops over a raw buffer: decode a varint from
// [p, limit) into `value` and return the position after it, or nullptr if the
// bytes are truncated or encode a number that doesn't fit the type.
// Overlong encodings (e.g. 0x80 0x00 for zero) are accepted, as in LevelDB;
// every ForgeDB record is checksummed, so they can't sneak in unnoticed.
[[nodiscard]] const char* DecodeVarint32(const char* p, const char* limit,
                                         std::uint32_t& value) noexcept;
[[nodiscard]] const char* DecodeVarint64(const char* p, const char* limit,
                                         std::uint64_t& value) noexcept;

}  // namespace forgedb
