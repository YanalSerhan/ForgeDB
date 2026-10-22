#include "util/crc32c.h"

#include <array>
#include <cstdint>

#include <gtest/gtest.h>

#include <random>
#include <string>
#include <string_view>
#include <vector>

namespace forgedb::crc32c {
namespace {

struct Implementation {
  const char* name;
  std::uint32_t (*extend)(std::uint32_t, const char*, std::size_t) noexcept;
};

// Every implementation this CPU can run, plus the dispatching Extend().
std::vector<Implementation> Implementations() {
  std::vector<Implementation> result = {
      {.name = "portable", .extend = &ExtendPortable},
      {.name = "dispatch",
       .extend = static_cast<std::uint32_t (*)(std::uint32_t, const char*, std::size_t) noexcept>(
           &Extend)},
  };
  if (Sse42Supported()) {
    result.push_back({.name = "sse4.2", .extend = &ExtendSse42});
  }
  return result;
}

std::uint32_t Crc(const Implementation& impl, std::string_view data) {
  return impl.extend(0, data.data(), data.size());
}

// Test vectors: the standard check value, and RFC 3720 (iSCSI) Appendix B.4.
TEST(Crc32cTest, StandardCheckValue) {
  for (const Implementation& impl : Implementations()) {
    EXPECT_EQ(Crc(impl, "123456789"), 0xE3069283U) << impl.name;
  }
}

TEST(Crc32cTest, Rfc3720Vectors) {
  const std::string zeros(32, '\0');
  const std::string ones(32, '\xFF');
  std::string ascending(32, '\0');
  std::string descending(32, '\0');
  for (std::size_t i = 0; i < 32; ++i) {
    ascending[i] = static_cast<char>(i);
    descending[i] = static_cast<char>(31 - i);
  }
  // An iSCSI SCSI Read (10) command PDU.
  const std::array<unsigned char, 48> pdu = {
      0x01, 0xc0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
      0x00, 0x00, 0x00, 0x00, 0x14, 0x00, 0x00, 0x00, 0x00, 0x00, 0x04, 0x00,
      0x00, 0x00, 0x00, 0x14, 0x00, 0x00, 0x00, 0x18, 0x28, 0x00, 0x00, 0x00,
      0x00, 0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
  };
  const std::string pdu_bytes(pdu.begin(), pdu.end());

  for (const Implementation& impl : Implementations()) {
    EXPECT_EQ(Crc(impl, zeros), 0x8A9136AAU) << impl.name;
    EXPECT_EQ(Crc(impl, ones), 0x62A8AB43U) << impl.name;
    EXPECT_EQ(Crc(impl, ascending), 0x46DD794EU) << impl.name;
    EXPECT_EQ(Crc(impl, descending), 0x113FDB5CU) << impl.name;
    EXPECT_EQ(Crc(impl, pdu_bytes), 0xD9963A56U) << impl.name;
  }
}

TEST(Crc32cTest, EmptyInput) {
  for (const Implementation& impl : Implementations()) {
    EXPECT_EQ(Crc(impl, ""), 0U) << impl.name;
    EXPECT_EQ(impl.extend(0x12345678U, nullptr, 0), 0x12345678U) << impl.name;
  }
}

TEST(Crc32cTest, ExtendEqualsWhole) {
  EXPECT_EQ(Value("hello world"), Extend(Value("hello "), "world"));
  for (const Implementation& impl : Implementations()) {
    const std::string data = "The quick brown fox jumps over the lazy dog";
    for (std::size_t split = 0; split <= data.size(); ++split) {
      const std::uint32_t first = impl.extend(0, data.data(), split);
      EXPECT_EQ(impl.extend(first, data.data() + split, data.size() - split), Crc(impl, data))
          << impl.name << " split at " << split;
    }
  }
}

TEST(Crc32cTest, DifferentInputsDiffer) {
  EXPECT_NE(Value("a"), Value("foo"));
  EXPECT_NE(Value("foo"), Value("fop"));
}

TEST(Crc32cTest, MaskRoundTripsAndChangesValue) {
  const std::uint32_t crc = Value("foo");
  EXPECT_NE(crc, Mask(crc));
  EXPECT_NE(crc, Mask(Mask(crc)));
  EXPECT_EQ(crc, Unmask(Mask(crc)));
  EXPECT_EQ(crc, Unmask(Unmask(Mask(Mask(crc)))));
  static_assert(Unmask(Mask(0xE3069283U)) == 0xE3069283U);
  static_assert(Mask(0xE3069283U) == 0xC78AB0E5U);  // computed independently
}

// The two implementations must agree on everything: random lengths (covering
// every tail length mod 8), random contents and unaligned start addresses.
TEST(Crc32cTest, ImplementationsAgreeOnRandomBuffers) {
  if (!Sse42Supported()) {
    GTEST_SKIP() << "CPU has no SSE4.2; only the portable implementation exists here";
  }
  constexpr std::uint64_t kSeed = 20261022;
  std::mt19937_64 rng(kSeed);
  std::uniform_int_distribution<int> byte(0, 255);
  std::uniform_int_distribution<std::size_t> length(0, 4100);
  std::uniform_int_distribution<std::size_t> offset(0, 7);

  std::string buffer(4100 + 8, '\0');
  for (int round = 0; round < 2000; ++round) {
    for (char& c : buffer) {
      c = static_cast<char>(byte(rng));
    }
    const std::size_t start = offset(rng);
    const std::size_t size = length(rng);
    const char* data = buffer.data() + start;
    ASSERT_EQ(ExtendPortable(0, data, size), ExtendSse42(0, data, size))
        << "seed " << kSeed << " round " << round << " offset " << start << " size " << size;
  }
}

TEST(Crc32cTest, ImplementationsAgreeOnLargeBuffer) {
  if (!Sse42Supported()) {
    GTEST_SKIP() << "CPU has no SSE4.2";
  }
  std::string big(1 << 20, '\0');
  std::mt19937 rng(7);
  for (char& c : big) {
    c = static_cast<char>(rng());
  }
  EXPECT_EQ(ExtendPortable(0, big.data(), big.size()), ExtendSse42(0, big.data(), big.size()));
}

}  // namespace
}  // namespace forgedb::crc32c
