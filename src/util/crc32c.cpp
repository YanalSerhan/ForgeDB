#include "util/crc32c.h"

#include <intrin.h>     // __cpuid (MSVC and clang-cl; a compiler intrinsic, not a Win32 API)
#include <nmmintrin.h>  // _mm_crc32_u8, _mm_crc32_u64 (SSE4.2)

#include <array>
#include <cstring>

// clang-cl only emits SSE4.2 instructions inside functions marked for that
// target; MSVC allows any intrinsic anywhere. Marking just this one function
// (instead of compiling the file with -msse4.2) guarantees the compiler can't
// slip SSE4.2 instructions into code that runs on CPUs without it.
#ifdef __clang__
// NOLINTNEXTLINE(cppcoreguidelines-macro-usage)
#define FORGEDB_TARGET_SSE42 __attribute__((target("sse4.2")))
#else
// NOLINTNEXTLINE(cppcoreguidelines-macro-usage)
#define FORGEDB_TARGET_SSE42
#endif

namespace forgedb::crc32c {
namespace {

constexpr std::uint32_t kPolynomial = 0x82F63B78U;  // 0x1EDC6F41, bit-reversed

// kTable[b] = the CRC of the single byte b. Built by the compiler (constexpr),
// so no start-up cost and no initialization-order questions.
constexpr std::array<std::uint32_t, 256> MakeTable() noexcept {
  std::array<std::uint32_t, 256> table{};
  // A range-for instead of table[i]: same result, and there is no index for
  // MSVC /analyze to prove in bounds (it can't see that i < 256 here, C28020).
  std::uint32_t byte = 0;
  for (std::uint32_t& entry : table) {
    std::uint32_t crc = byte++;
    for (int bit = 0; bit < 8; ++bit) {
      crc = (crc & 1U) != 0 ? (crc >> 1) ^ kPolynomial : crc >> 1;
    }
    entry = crc;
  }
  return table;
}

constexpr std::array<std::uint32_t, 256> kTable = MakeTable();
static_assert(kTable[0] == 0 && kTable[1] == 0xF26B8303U && kTable[128] == 0x82F63B78U,
              "CRC-32C table does not match the published values");

using ExtendFunction = std::uint32_t (*)(std::uint32_t, const char*, std::size_t) noexcept;

}  // namespace

std::uint32_t ExtendPortable(std::uint32_t crc, const char* data, std::size_t size) noexcept {
  // CRC-32C starts from all ones and inverts the result; inverting on the way
  // in and out is what lets Extend() chain calls.
  std::uint32_t state = ~crc;
  for (std::size_t i = 0; i < size; ++i) {
    const auto byte = static_cast<std::uint8_t>(data[i]);
    state = kTable[(state ^ byte) & 0xFFU] ^ (state >> 8);
  }
  return ~state;
}

FORGEDB_TARGET_SSE42 std::uint32_t ExtendSse42(std::uint32_t crc, const char* data,
                                               std::size_t size) noexcept {
  // Invert in 32 bits first, then widen; the instruction takes a 64-bit operand.
  const std::uint32_t inverted = ~crc;
  std::uint64_t state = inverted;
  // 8 bytes per instruction. memcpy keeps unaligned loads well-defined; it
  // compiles to a single mov.
  while (size >= sizeof(std::uint64_t)) {
    std::uint64_t chunk = 0;
    std::memcpy(&chunk, data, sizeof(chunk));
    state = _mm_crc32_u64(state, chunk);
    data += sizeof(chunk);
    size -= sizeof(chunk);
  }
  auto state32 = static_cast<std::uint32_t>(state);
  for (; size > 0; --size, ++data) {
    state32 = _mm_crc32_u8(state32, static_cast<std::uint8_t>(*data));
  }
  return ~state32;
}

bool Sse42Supported() noexcept {
  // A function-local static is initialized once, thread-safely, on first use.
  static const bool kSupported = [] {
    std::array<int, 4> registers{};  // EAX, EBX, ECX, EDX
    __cpuid(registers.data(), 1);
    return (registers[2] & (1 << 20)) != 0;
  }();
  return kSupported;
}

std::uint32_t Extend(std::uint32_t crc, const char* data, std::size_t size) noexcept {
  // Chosen once; afterwards each call is one well-predicted indirect call.
  static const ExtendFunction kImplementation = Sse42Supported() ? &ExtendSse42 : &ExtendPortable;
  return kImplementation(crc, data, size);
}

}  // namespace forgedb::crc32c
