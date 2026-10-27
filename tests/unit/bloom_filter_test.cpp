#include "table/bloom_filter.h"

#include <cstddef>
#include <cstdint>
#include <cstdio>

#include <gtest/gtest.h>

#include <string>
#include <string_view>

#include "util/coding.h"

namespace forgedb {
namespace {

std::string FixedKey(std::uint32_t i) {
  std::string key;
  PutFixed32(key, i);
  return key;
}

// 1, 2, ..., 10, 20, ..., 100, 200, ..., 1000, 2000, ...
std::uint32_t NextLength(std::uint32_t length) {
  if (length < 10) {
    return length + 1;
  }
  if (length < 100) {
    return length + 10;
  }
  if (length < 1000) {
    return length + 100;
  }
  return length + 1000;
}

std::string ToHex(std::string_view bytes) {
  static constexpr std::string_view kDigits = "0123456789abcdef";
  std::string hex;
  for (const char c : bytes) {
    const auto b = static_cast<unsigned char>(c);
    hex.push_back(kDigits[b >> 4]);
    hex.push_back(kDigits[b & 0xF]);
  }
  return hex;
}

TEST(BloomFilterTest, ProbeCount) {
  EXPECT_EQ(BloomFilterProbes(1), 1);   // 0.69 -> 1
  EXPECT_EQ(BloomFilterProbes(5), 3);   // 3.47 -> 3
  EXPECT_EQ(BloomFilterProbes(10), 7);  // 6.93 -> 7
  EXPECT_EQ(BloomFilterProbes(20), 14);
  EXPECT_EQ(BloomFilterProbes(43), 30);  // 29.8 -> 30
  EXPECT_EQ(BloomFilterProbes(100), 30);
  EXPECT_EQ(BloomFilterProbes(0), 1);
}

TEST(BloomFilterTest, EmptyFilterMatchesNothing) {
  BloomFilterBuilder builder(10);
  const std::string filter = builder.Finish();
  EXPECT_EQ(filter.size(), 9U);  // 64 bits + k
  EXPECT_FALSE(BloomFilterMayMatch(filter, ""));
  EXPECT_FALSE(BloomFilterMayMatch(filter, "hello"));
}

TEST(BloomFilterTest, SmallFilter) {
  BloomFilterBuilder builder(10);
  builder.AddKey("hello");
  builder.AddKey("world");
  const std::string filter = builder.Finish();
  EXPECT_TRUE(BloomFilterMayMatch(filter, "hello"));
  EXPECT_TRUE(BloomFilterMayMatch(filter, "world"));
  EXPECT_FALSE(BloomFilterMayMatch(filter, "x"));
  EXPECT_FALSE(BloomFilterMayMatch(filter, "foo"));
}

// The filter bytes are part of the table file format. These were computed by
// an independent implementation of the documented layout: Python with the
// mmh3 package (docs/file-formats.md, "Bloom filter").
TEST(BloomFilterTest, FormatIsStable) {
  {
    BloomFilterBuilder builder(10);
    builder.AddKey("hello");
    builder.AddKey("world");
    EXPECT_EQ(ToHex(builder.Finish()), "04000a981106210807");
  }
  {
    BloomFilterBuilder builder(10);
    for (int i = 0; i < 20; ++i) {
      const std::string number = std::to_string(i);
      builder.AddKey("key" + std::string(4 - number.size(), '0') + number);
    }
    EXPECT_EQ(ToHex(builder.Finish()), "d9518f831e7b1421d1a35cc9a8a08cd30ecb3a676cfad6ef2207");
  }
}

TEST(BloomFilterTest, ConsecutiveDuplicatesCountOnce) {
  BloomFilterBuilder builder(10);
  builder.AddKey("");
  builder.AddKey("");
  builder.AddKey("a");
  builder.AddKey("a");
  builder.AddKey("b");
  EXPECT_EQ(builder.NumKeys(), 3U);
  const std::string filter = builder.Finish();
  EXPECT_TRUE(BloomFilterMayMatch(filter, ""));
  EXPECT_TRUE(BloomFilterMayMatch(filter, "a"));
  EXPECT_TRUE(BloomFilterMayMatch(filter, "b"));
}

TEST(BloomFilterTest, FinishStartsANewFilter) {
  BloomFilterBuilder builder(10);
  builder.AddKey("first");
  const std::string first = builder.Finish();
  EXPECT_EQ(builder.NumKeys(), 0U);
  builder.AddKey("first");  // not a duplicate of the previous filter's last key
  EXPECT_EQ(builder.NumKeys(), 1U);
  builder.AddKey("second");
  const std::string second = builder.Finish();
  EXPECT_TRUE(BloomFilterMayMatch(second, "first"));
  EXPECT_TRUE(BloomFilterMayMatch(second, "second"));
  EXPECT_FALSE(BloomFilterMayMatch(first, "second"));
}

TEST(BloomFilterTest, MalformedFilterMatchesEverything) {
  EXPECT_TRUE(BloomFilterMayMatch("", "key"));
  EXPECT_TRUE(BloomFilterMayMatch(std::string(1, '\x07'), "key"));
  for (const int k : {0, 31, 255}) {
    std::string filter(9, '\0');
    filter.back() = static_cast<char>(k);
    EXPECT_TRUE(BloomFilterMayMatch(filter, "key")) << k;
  }
}

// LevelDB's "varying lengths" test: filters for 1 to 10,000 keys never miss an
// added key, and each one's false-positive rate stays at most 2%.
TEST(BloomFilterTest, NoFalseNegativesAtAnySize) {
  BloomFilterBuilder builder(10);
  for (std::uint32_t length = 1; length <= 10000; length = NextLength(length)) {
    for (std::uint32_t i = 0; i < length; ++i) {
      builder.AddKey(FixedKey(i));
    }
    const std::string filter = builder.Finish();
    EXPECT_LE(filter.size(), (length * 10 / 8) + 40) << length;
    for (std::uint32_t i = 0; i < length; ++i) {
      ASSERT_TRUE(BloomFilterMayMatch(filter, FixedKey(i))) << "length " << length << " key " << i;
    }
    int false_positives = 0;
    for (std::uint32_t i = 0; i < 10000; ++i) {
      false_positives += BloomFilterMayMatch(filter, FixedKey(i + 1000000000U)) ? 1 : 0;
    }
    EXPECT_LE(false_positives, 200) << "length " << length;  // 2%
  }
}

// 100,000 keys at 10 bits per key: the formula (1 - e^(-k/10))^k with k = 7
// predicts 0.82% false positives. Fails above 1.2%.
TEST(BloomFilterTest, FalsePositiveRateAt10BitsPerKey) {
  constexpr std::uint32_t kKeys = 100000;
  constexpr std::uint32_t kProbes = 1000000;
  BloomFilterBuilder builder(10);
  for (std::uint32_t i = 0; i < kKeys; ++i) {
    builder.AddKey("key-" + std::to_string(i));
  }
  const std::string filter = builder.Finish();
  EXPECT_EQ(filter.size(), (kKeys * 10 / 8) + 1);
  for (std::uint32_t i = 0; i < kKeys; ++i) {
    ASSERT_TRUE(BloomFilterMayMatch(filter, "key-" + std::to_string(i))) << i;
  }
  std::uint32_t false_positives = 0;
  for (std::uint32_t i = 0; i < kProbes; ++i) {
    false_positives += BloomFilterMayMatch(filter, "absent-" + std::to_string(i)) ? 1 : 0;
  }
  const double rate = static_cast<double>(false_positives) / kProbes;
  std::printf(  // NOLINT(cppcoreguidelines-pro-type-vararg)
      "[ bloom    ] 10 bits/key, %u keys: %u false positives in %u probes = %.3f%%\n", kKeys,
      false_positives, kProbes, rate * 100);
  RecordProperty("false_positive_rate_percent", std::to_string(rate * 100));
  EXPECT_LT(rate, 0.012);
}

}  // namespace
}  // namespace forgedb
