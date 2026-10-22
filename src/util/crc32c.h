// CRC-32C: the checksum on every ForgeDB record and block.
//
// CRC-32C uses the Castagnoli polynomial 0x1EDC6F41 (0x82F63B78 bit-reversed),
// the same checksum as iSCSI, ext4, LevelDB and RocksDB. It is chosen over the
// classic zlib CRC-32 because x86 CPUs since SSE4.2 (2008) compute it with one
// instruction, and it detects more error patterns in short messages.
//
// Two implementations produce identical results:
//   - ExtendPortable: one 256-entry table lookup per byte, any x64 CPU
//   - ExtendSse42:    the CRC32 instruction, 8 bytes per step
// Extend() picks one the first time it is called, using CPUID.
#pragma once

#include <bit>
#include <cstddef>
#include <cstdint>
#include <string_view>

namespace forgedb::crc32c {

// The CRC-32C of B, given crc = CRC-32C of A, where the input is A followed by
// `data`. Use crc = 0 for a fresh checksum. This lets a record header and its
// payload be checksummed without copying them into one buffer.
[[nodiscard]] std::uint32_t Extend(std::uint32_t crc, const char* data, std::size_t size) noexcept;

[[nodiscard]] inline std::uint32_t Extend(std::uint32_t crc, std::string_view data) noexcept {
  return Extend(crc, data.data(), data.size());
}

// The CRC-32C of `data`.
[[nodiscard]] inline std::uint32_t Value(std::string_view data) noexcept { return Extend(0, data); }

// Masking for STORED checksums.
//
// Computing the CRC of bytes that contain their own CRC behaves badly (a CRC
// "absorbs" itself, so some corruptions cancel out). This matters when a record
// with an embedded CRC is later checksummed again, e.g. inside a block. Formats
// therefore store Mask(crc) and compare against Unmask(stored). Same scheme and
// constant as LevelDB: rotate right by 15 bits, then add a constant.
inline constexpr std::uint32_t kMaskDelta = 0xa282ead8U;

[[nodiscard]] constexpr std::uint32_t Mask(std::uint32_t crc) noexcept {
  return std::rotr(crc, 15) + kMaskDelta;
}
[[nodiscard]] constexpr std::uint32_t Unmask(std::uint32_t masked) noexcept {
  return std::rotl(masked - kMaskDelta, 15);
}

// The individual implementations, exposed for tests and benchmarks.
[[nodiscard]] std::uint32_t ExtendPortable(std::uint32_t crc, const char* data,
                                           std::size_t size) noexcept;
// Precondition: Sse42Supported(). On a CPU without SSE4.2 it would crash with an
// illegal-instruction exception.
[[nodiscard]] std::uint32_t ExtendSse42(std::uint32_t crc, const char* data,
                                        std::size_t size) noexcept;
// Whether this CPU has the SSE4.2 CRC32 instruction (CPUID leaf 1, ECX bit 20).
[[nodiscard]] bool Sse42Supported() noexcept;

}  // namespace forgedb::crc32c
