#include "util/hash.h"

#include <array>
#include <cstddef>
#include <cstdint>

#include <gtest/gtest.h>

#include <string>
#include <string_view>

namespace forgedb {
namespace {

// Expected values come from an independent implementation, the Python package
// mmh3 5.3.1: mmh3.hash128(data, seed, x64arch=True, signed=False), split into
// its low and high 64 bits. The "quick brown fox" value is also the one widely
// published for MurmurHash3_x64_128.
struct Vector {
  std::size_t length;  // the input is the bytes 0, 1, ..., length-1
  std::uint64_t low;
  std::uint64_t high;
};

constexpr auto kAscendingBytes = std::to_array<Vector>({
    {.length = 0, .low = 0x0000000000000000ULL, .high = 0x0000000000000000ULL},
    {.length = 1, .low = 0x4610abe56eff5cb5ULL, .high = 0x51622daa78f83583ULL},
    {.length = 2, .low = 0x7cb3f5c58dab264cULL, .high = 0xf03c01326ec2e044ULL},
    {.length = 3, .low = 0xb872a12fef53e6beULL, .high = 0xfb6255c252b396b6ULL},
    {.length = 4, .low = 0xe1c594ae0ddfaf10ULL, .high = 0xd3d605bd13c2fde2ULL},
    {.length = 5, .low = 0x41ee8cd4a6f94036ULL, .high = 0xf8d0155e630c23f6ULL},
    {.length = 6, .low = 0x66983abba4f5043cULL, .high = 0x57e0240b16512ca0ULL},
    {.length = 7, .low = 0xbd4c6987ca4b0d68ULL, .high = 0x613addd4bd25c787ULL},
    {.length = 8, .low = 0x47a7e1bdd68e2fc8ULL, .high = 0x60e6ee02ec31dcc7ULL},
    {.length = 9, .low = 0xfbb4cb0f6e812d32ULL, .high = 0x78de751d0200ffb9ULL},
    {.length = 10, .low = 0xcfca25e89e58e463ULL, .high = 0x254313c472ad2076ULL},
    {.length = 11, .low = 0xc57b4f47c7564f88ULL, .high = 0x6965ea8d20711bb0ULL},
    {.length = 12, .low = 0xb35da7e69212a5caULL, .high = 0x8075f146ecf75346ULL},
    {.length = 13, .low = 0x4b52d9f2c55f41c2ULL, .high = 0x84ff869eafa6d8fcULL},
    {.length = 14, .low = 0x5fa933ee35906d64ULL, .high = 0x0d782573380fc9dfULL},
    {.length = 15, .low = 0x47231598fd4925e9ULL, .high = 0xcd846dee88c67de9ULL},
    {.length = 16, .low = 0x444924b591903f30ULL, .high = 0xab906456762fe845ULL},
    {.length = 17, .low = 0x5c76f40f9fe7c20eULL, .high = 0xc15f026b9edaa824ULL},
    {.length = 18, .low = 0x1648e4d1bae9c47bULL, .high = 0xce2684180bb9eb5fULL},
    {.length = 19, .low = 0xa665a45564a30163ULL, .high = 0x954f7960bfae1fdcULL},
    {.length = 20, .low = 0xa3d253705736954bULL, .high = 0x5a1f06256c7b9c7aULL},
    {.length = 21, .low = 0xcdd13da028d41203ULL, .high = 0x6bc50efc2da2e7cbULL},
    {.length = 22, .low = 0x45f8f3bf27c12a71ULL, .high = 0x1730a373601e5662ULL},
    {.length = 23, .low = 0xb9a1376f153581dbULL, .high = 0x26451c93b42a4941ULL},
    {.length = 24, .low = 0x734e846275b2dfdeULL, .high = 0x94707eabbbfb9e52ULL},
    {.length = 25, .low = 0x3bbe7cb52ee982cbULL, .high = 0xa2d35433beef9ffcULL},
    {.length = 26, .low = 0xa515dcfe02226f72ULL, .high = 0x2e0c68dcfcc47d78ULL},
    {.length = 27, .low = 0x111602884d592eedULL, .high = 0x28741948dc1db519ULL},
    {.length = 28, .low = 0xdc14ff62ee05813cULL, .high = 0x662050bb4567cb8bULL},
    {.length = 29, .low = 0xaae494b11518cf1aULL, .high = 0x649e12f78ccfe176ULL},
    {.length = 30, .low = 0xf069080d38a201e3ULL, .high = 0x22f3687fe3d26b19ULL},
    {.length = 31, .low = 0x053dd3e1a32cd094ULL, .high = 0x9ee59aefb4005490ULL},
    {.length = 32, .low = 0xc66d9022b62f500fULL, .high = 0x1c050a6e34c31151ULL},
});

// Lengths 0-32 cover every tail length (0-15) with zero, one and two full
// 16-byte blocks before it.
TEST(HashTest, MatchesReferenceForEveryTailLength) {
  for (const Vector& v : kAscendingBytes) {
    std::string data;
    for (std::size_t i = 0; i < v.length; ++i) {
      data.push_back(static_cast<char>(i));
    }
    const Hash128 h = MurmurHash3x64(data);
    EXPECT_EQ(h.low, v.low) << "length " << v.length;
    EXPECT_EQ(h.high, v.high) << "length " << v.length;
  }
}

TEST(HashTest, MatchesReferenceForTextAndSeeds) {
  EXPECT_EQ(MurmurHash3x64("The quick brown fox jumps over the lazy dog"),
            (Hash128{.low = 0xe34bbc7bbc071b6cULL, .high = 0x7a433ca9c49a9347ULL}));
  EXPECT_EQ(MurmurHash3x64("hello", 0x9747b28cU),
            (Hash128{.low = 0x8c23d6856f071a2eULL, .high = 0x2a905546b3c1cb83ULL}));
  // High bytes: catches sign extension of chars in the tail.
  EXPECT_EQ(MurmurHash3x64(std::string(40, '\xff'), 0xffffffffU),
            (Hash128{.low = 0x3b8a647c1949fdb7ULL, .high = 0xf5704fbace39d59cULL}));
}

TEST(HashTest, DoesNotReadPastTheView) {
  // The same bytes inside a longer buffer hash the same: only the view counts.
  const std::string buffer = "abcdefghijklmnopqrstuvwxyz";
  for (std::size_t n = 0; n <= buffer.size(); ++n) {
    EXPECT_EQ(MurmurHash3x64(std::string_view(buffer).substr(0, n)),
              MurmurHash3x64(std::string(buffer.substr(0, n))))
        << n;
  }
}

}  // namespace
}  // namespace forgedb
