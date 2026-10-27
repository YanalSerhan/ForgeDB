#include "util/hash.h"

#include <bit>
#include <cstddef>
#include <cstdint>
#include <string_view>

#include "util/coding.h"

namespace forgedb {

namespace {

constexpr std::uint64_t kC1 = 0x87c37b91114253d5ULL;
constexpr std::uint64_t kC2 = 0x4cf5ad432745937fULL;

constexpr std::uint64_t FinalMix(std::uint64_t k) noexcept {
  k ^= k >> 33;
  k *= 0xff51afd7ed558ccdULL;
  k ^= k >> 33;
  k *= 0xc4ceb9fe1a85ec53ULL;
  k ^= k >> 33;
  return k;
}

constexpr std::uint64_t MixK1(std::uint64_t k1) noexcept { return std::rotl(k1 * kC1, 31) * kC2; }

constexpr std::uint64_t MixK2(std::uint64_t k2) noexcept { return std::rotl(k2 * kC2, 33) * kC1; }

}  // namespace

Hash128 MurmurHash3x64(std::string_view data, std::uint32_t seed) noexcept {
  const std::size_t size = data.size();
  const std::size_t blocks = size / 16;
  std::uint64_t h1 = seed;
  std::uint64_t h2 = seed;

  for (std::size_t i = 0; i < blocks; ++i) {
    const std::uint64_t k1 = DecodeFixed64(data.data() + (i * 16));
    const std::uint64_t k2 = DecodeFixed64(data.data() + (i * 16) + 8);

    h1 ^= MixK1(k1);
    h1 = std::rotl(h1, 27);
    h1 += h2;
    h1 = (h1 * 5) + 0x52dce729;

    h2 ^= MixK2(k2);
    h2 = std::rotl(h2, 31);
    h2 += h1;
    h2 = (h2 * 5) + 0x38495ab5;
  }

  // The last 0-15 bytes: bytes 8-14 go to k2 and bytes 0-7 to k1, each as a
  // little-endian number (the reference's switch, written as loops).
  const std::string_view tail = data.substr(blocks * 16);
  std::uint64_t k1 = 0;
  std::uint64_t k2 = 0;
  for (std::size_t i = tail.size(); i > 8; --i) {
    k2 |= std::uint64_t{static_cast<unsigned char>(tail[i - 1])} << ((i - 9) * 8);
  }
  for (std::size_t i = tail.size() < 8 ? tail.size() : 8; i > 0; --i) {
    k1 |= std::uint64_t{static_cast<unsigned char>(tail[i - 1])} << ((i - 1) * 8);
  }
  if (tail.size() > 8) {
    h2 ^= MixK2(k2);
  }
  if (!tail.empty()) {
    h1 ^= MixK1(k1);
  }

  h1 ^= size;
  h2 ^= size;
  h1 += h2;
  h2 += h1;
  h1 = FinalMix(h1);
  h2 = FinalMix(h2);
  h1 += h2;
  h2 += h1;
  return {.low = h1, .high = h2};
}

}  // namespace forgedb
