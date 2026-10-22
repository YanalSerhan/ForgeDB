#include "util/coding.h"

#include <array>
#include <cstdint>

#include <gtest/gtest.h>

#include <initializer_list>
#include <limits>
#include <string>
#include <string_view>
#include <vector>

namespace forgedb {
namespace {

std::string Bytes(std::initializer_list<unsigned char> bytes) {
  std::string s;
  for (const unsigned char b : bytes) {
    s.push_back(static_cast<char>(b));
  }
  return s;
}

// Values around every 7-bit boundary, plus the extremes.
std::vector<std::uint64_t> InterestingValues() {
  std::vector<std::uint64_t> values = {0, 1, 2, 127, 128, 255, 256, 300};
  for (unsigned bits = 7; bits < 64; bits += 7) {
    const std::uint64_t power = std::uint64_t{1} << bits;
    values.push_back(power - 1);
    values.push_back(power);
    values.push_back(power + 1);
  }
  values.push_back(std::numeric_limits<std::uint32_t>::max());
  values.push_back(std::numeric_limits<std::uint64_t>::max() - 1);
  values.push_back(std::numeric_limits<std::uint64_t>::max());
  return values;
}

// ----- Fixed width -----------------------------------------------------------

TEST(CodingTest, Fixed32IsLittleEndian) {
  std::string s;
  PutFixed32(s, 0x04030201U);
  EXPECT_EQ(s, Bytes({0x01, 0x02, 0x03, 0x04}));
}

TEST(CodingTest, Fixed64IsLittleEndian) {
  std::string s;
  PutFixed64(s, 0x0807060504030201ULL);
  EXPECT_EQ(s, Bytes({0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08}));
}

TEST(CodingTest, FixedRoundTrip) {
  std::string s;
  for (const std::uint64_t v : InterestingValues()) {
    PutFixed32(s, static_cast<std::uint32_t>(v));
    PutFixed64(s, v);
  }
  std::string_view input = s;
  for (const std::uint64_t v : InterestingValues()) {
    EXPECT_EQ(GetFixed32(input), static_cast<std::uint32_t>(v));
    EXPECT_EQ(GetFixed64(input), v);
  }
  EXPECT_TRUE(input.empty());
}

TEST(CodingTest, FixedRejectsShortInput) {
  const std::string s = Bytes({1, 2, 3});
  std::string_view input = s;
  EXPECT_EQ(GetFixed32(input), std::nullopt);
  EXPECT_EQ(GetFixed64(input), std::nullopt);
  EXPECT_EQ(input.size(), 3U) << "failed reads must not consume input";
}

TEST(CodingTest, FixedDecodeFromUnalignedAddress) {
  std::array<char, 16> buf{};
  EncodeFixed64(buf.data() + 3, 0x1122334455667788ULL);
  EXPECT_EQ(DecodeFixed64(buf.data() + 3), 0x1122334455667788ULL);
}

// ----- Varint ----------------------------------------------------------------

TEST(CodingTest, VarintKnownEncodings) {
  std::string s;
  PutVarint32(s, 0);
  EXPECT_EQ(s, Bytes({0x00}));
  s.clear();
  PutVarint32(s, 127);
  EXPECT_EQ(s, Bytes({0x7F}));
  s.clear();
  PutVarint32(s, 300);  // 300 = 0b10'0101100 -> 0xAC 0x02
  EXPECT_EQ(s, Bytes({0xAC, 0x02}));
  s.clear();
  PutVarint32(s, std::numeric_limits<std::uint32_t>::max());
  EXPECT_EQ(s, Bytes({0xFF, 0xFF, 0xFF, 0xFF, 0x0F}));
}

TEST(CodingTest, Varint32RoundTrip) {
  std::string s;
  std::vector<std::uint32_t> values;
  for (const std::uint64_t v : InterestingValues()) {
    if (v <= std::numeric_limits<std::uint32_t>::max()) {
      values.push_back(static_cast<std::uint32_t>(v));
    }
  }
  for (const std::uint32_t v : values) {
    const std::size_t before = s.size();
    PutVarint32(s, v);
    EXPECT_EQ(s.size() - before, VarintLength(v)) << v;
  }
  std::string_view input = s;
  for (const std::uint32_t v : values) {
    EXPECT_EQ(GetVarint32(input), v);
  }
  EXPECT_TRUE(input.empty());
}

TEST(CodingTest, Varint64RoundTrip) {
  std::string s;
  for (const std::uint64_t v : InterestingValues()) {
    const std::size_t before = s.size();
    PutVarint64(s, v);
    EXPECT_EQ(s.size() - before, VarintLength(v)) << v;
  }
  std::string_view input = s;
  for (const std::uint64_t v : InterestingValues()) {
    EXPECT_EQ(GetVarint64(input), v);
  }
  EXPECT_TRUE(input.empty());
  EXPECT_EQ(VarintLength(std::numeric_limits<std::uint64_t>::max()), kMaxVarint64Bytes);
}

TEST(CodingTest, VarintEveryTruncationFails) {
  std::string s;
  PutVarint64(s, std::numeric_limits<std::uint64_t>::max());
  ASSERT_EQ(s.size(), kMaxVarint64Bytes);
  for (std::size_t length = 0; length < s.size(); ++length) {
    std::string_view input(s.data(), length);
    EXPECT_EQ(GetVarint64(input), std::nullopt) << "prefix length " << length;
    EXPECT_EQ(input.size(), length) << "failed reads must not consume input";
  }
}

TEST(CodingTest, Varint32RejectsOverflow) {
  // A 5th byte above 0x0F sets bits beyond 32.
  const std::string overflow = Bytes({0xFF, 0xFF, 0xFF, 0xFF, 0x10});
  std::string_view input = overflow;
  EXPECT_EQ(GetVarint32(input), std::nullopt);
  // A continuation bit on the 5th byte would mean a 6th byte: too long.
  const std::string too_long = Bytes({0x80, 0x80, 0x80, 0x80, 0x80, 0x00});
  input = too_long;
  EXPECT_EQ(GetVarint32(input), std::nullopt);
}

TEST(CodingTest, Varint64RejectsOverflow) {
  const std::string overflow = Bytes({0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x02});
  std::string_view input = overflow;
  EXPECT_EQ(GetVarint64(input), std::nullopt);
  const std::string too_long =
      Bytes({0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x00});
  input = too_long;
  EXPECT_EQ(GetVarint64(input), std::nullopt);
}

TEST(CodingTest, VarintAcceptsOverlongEncoding) {
  // Documented choice: 0x80 0x00 decodes as 0 (records are checksummed anyway).
  const std::string overlong = Bytes({0x80, 0x00});
  std::string_view input = overlong;
  EXPECT_EQ(GetVarint32(input), 0U);
  EXPECT_TRUE(input.empty());
}

TEST(CodingTest, DecodeVarintPointerForm) {
  std::string s;
  PutVarint32(s, 300);
  PutVarint64(s, 1ULL << 40);
  const char* p = s.data();
  const char* limit = s.data() + s.size();
  std::uint32_t a = 0;
  std::uint64_t b = 0;
  p = DecodeVarint32(p, limit, a);
  ASSERT_NE(p, nullptr);
  p = DecodeVarint64(p, limit, b);
  ASSERT_NE(p, nullptr);
  EXPECT_EQ(a, 300U);
  EXPECT_EQ(b, 1ULL << 40);
  EXPECT_EQ(p, limit);
  EXPECT_EQ(DecodeVarint32(limit, limit, a), nullptr);  // empty input
}

// ----- Length-prefixed -------------------------------------------------------

TEST(CodingTest, LengthPrefixedRoundTrip) {
  std::string s;
  PutLengthPrefixed(s, "");
  PutLengthPrefixed(s, "foo");
  PutLengthPrefixed(s, std::string(300, 'x'));
  std::string_view input = s;
  EXPECT_EQ(GetLengthPrefixed(input), std::string_view(""));
  EXPECT_EQ(GetLengthPrefixed(input), std::string_view("foo"));
  EXPECT_EQ(GetLengthPrefixed(input), std::string_view(std::string(300, 'x')));
  EXPECT_TRUE(input.empty());
}

TEST(CodingTest, LengthPrefixedRejectsLengthPastEnd) {
  // Claims 5 bytes, has 3: an untrusted length must never cause a read past the end.
  const std::string s = Bytes({0x05, 'a', 'b', 'c'});
  std::string_view input = s;
  EXPECT_EQ(GetLengthPrefixed(input), std::nullopt);
  EXPECT_EQ(input.size(), s.size());
}

TEST(CodingTest, LengthPrefixedRejectsBadLength) {
  const std::string s = Bytes({0x80});  // truncated varint
  std::string_view input = s;
  EXPECT_EQ(GetLengthPrefixed(input), std::nullopt);
}

}  // namespace
}  // namespace forgedb
