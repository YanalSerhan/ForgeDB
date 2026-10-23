#include "db/internal_key.h"

#include <algorithm>
#include <compare>
#include <cstdint>

#include <gtest/gtest.h>

#include <random>
#include <string>
#include <string_view>
#include <vector>

#include "util/coding.h"

namespace forgedb {
namespace {

std::string Key(std::string_view user_key, SequenceNumber sequence,
                ValueType type = ValueType::kValue) {
  std::string encoded;
  AppendInternalKey(encoded, {.user_key = user_key, .sequence = sequence, .type = type});
  return encoded;
}

bool Less(std::string_view a, std::string_view b) { return CompareInternalKeys(a, b) < 0; }

// --- Encoding ----------------------------------------------------------------

TEST(InternalKeyTest, ByteLayout) {
  // tag = (5 << 8) | 1 = 0x501, stored little-endian after the user key.
  EXPECT_EQ(Key("ab", 5, ValueType::kValue), std::string("ab\x01\x05\x00\x00\x00\x00\x00\x00", 10));
  EXPECT_EQ(Key("", 0, ValueType::kDeletion), std::string(8, '\0'));
}

TEST(InternalKeyTest, ParseRoundTrip) {
  const std::vector<std::string> user_keys = {"", "k", "hello", std::string("\x00\xff\x01", 3),
                                              std::string(300, 'x')};
  for (const std::string& user_key : user_keys) {
    for (const SequenceNumber sequence :
         {SequenceNumber{0}, SequenceNumber{1}, SequenceNumber{1} << 40, kMaxSequenceNumber}) {
      for (const ValueType type : {ValueType::kValue, ValueType::kDeletion}) {
        const std::string encoded = Key(user_key, sequence, type);
        EXPECT_EQ(encoded.size(), user_key.size() + kTagSize);
        const ParsedInternalKey expected{.user_key = user_key, .sequence = sequence, .type = type};
        EXPECT_TRUE(ParseInternalKey(encoded) == expected);
        EXPECT_EQ(ExtractUserKey(encoded), user_key);
        EXPECT_EQ(ExtractTag(encoded), PackSequenceAndType(sequence, type));
      }
    }
  }
}

TEST(InternalKeyTest, ParseRejectsMalformedInput) {
  EXPECT_FALSE(ParseInternalKey("").has_value());
  EXPECT_FALSE(ParseInternalKey(std::string(7, '\0')).has_value()) << "shorter than a tag";
  std::string bad_type = Key("k", 1);
  bad_type[1] = '\x02';  // the type byte is the first tag byte (little-endian)
  EXPECT_FALSE(ParseInternalKey(bad_type).has_value());
  bad_type[1] = '\xff';
  EXPECT_FALSE(ParseInternalKey(bad_type).has_value());
}

// --- Ordering ------------------------------------------------------------------

TEST(InternalKeyTest, UserKeyAscendingFirst) {
  EXPECT_TRUE(Less(Key("a", 1), Key("b", 100)));  // sequence doesn't matter across keys
  EXPECT_TRUE(Less(Key("a", 100), Key("b", 1)));
  EXPECT_TRUE(Less(Key("a", 1), Key("ab", 1))) << "a prefix sorts first";
  EXPECT_TRUE(Less(Key("", 1), Key("a", 1)));
}

TEST(InternalKeyTest, BytesCompareAsUnsigned) {
  // With signed chars, '\xff' (-1) would sort before 'a'; ForgeDB, like memcmp, sorts it after.
  EXPECT_TRUE(Less(Key("a", 1), Key("\xff", 1)));
  EXPECT_TRUE(Less(Key(std::string("\x00", 1), 1), Key("\x01", 1)));
}

TEST(InternalKeyTest, NewerVersionsOfAKeyComeFirst) {
  EXPECT_TRUE(Less(Key("k", 5), Key("k", 4)));
  EXPECT_TRUE(Less(Key("k", kMaxSequenceNumber), Key("k", 0)));
  // Same sequence: the larger type (kValue) first.
  EXPECT_TRUE(Less(Key("k", 7, ValueType::kValue), Key("k", 7, ValueType::kDeletion)));
}

TEST(InternalKeyTest, EqualKeysAreEquivalent) {
  EXPECT_EQ(CompareInternalKeys(Key("k", 7), Key("k", 7)), std::strong_ordering::equal);
  EXPECT_EQ(InternalKeyComparator{}(Key("k", 7), Key("k", 8)), std::strong_ordering::greater);
}

TEST(InternalKeyTest, SortingShuffledKeys) {
  const std::vector<std::string> expected = {
      Key("apple", 9),  Key("apple", 3, ValueType::kDeletion),  Key("apple", 1),
      Key("banana", 7), Key("banana", 7, ValueType::kDeletion), Key("cherry", 2),
  };
  std::vector<std::string> shuffled = expected;
  std::mt19937 rng(3);
  for (int round = 0; round < 20; ++round) {
    std::shuffle(shuffled.begin(), shuffled.end(), rng);
    std::ranges::sort(shuffled, Less);
    EXPECT_EQ(shuffled, expected);
  }
}

// --- LookupKey -----------------------------------------------------------------

TEST(LookupKeyTest, Views) {
  const LookupKey lookup("foo", 42);
  EXPECT_EQ(lookup.UserKey(), "foo");
  const ParsedInternalKey expected{.user_key = "foo", .sequence = 42, .type = kValueTypeForSeek};
  EXPECT_TRUE(ParseInternalKey(lookup.InternalKey()) == expected);

  // MemtableKey = varint32(internal key length) + internal key.
  std::string_view memtable_key = lookup.MemtableKey();
  EXPECT_EQ(GetVarint32(memtable_key), 3U + kTagSize);
  EXPECT_EQ(memtable_key, lookup.InternalKey());
}

TEST(LookupKeyTest, LongKeysUseTheHeap) {
  const std::string user_key(1000, 'z');  // larger than the inline buffer
  const LookupKey lookup(user_key, 7);
  EXPECT_EQ(lookup.UserKey(), user_key);
  std::string_view memtable_key = lookup.MemtableKey();
  EXPECT_EQ(GetVarint32(memtable_key), 1000U + kTagSize);
  EXPECT_EQ(memtable_key, lookup.InternalKey());
}

TEST(LookupKeyTest, EmptyUserKey) {
  const LookupKey lookup("", 1);
  EXPECT_EQ(lookup.UserKey(), "");
  EXPECT_EQ(lookup.InternalKey().size(), kTagSize);
}

// The property Get depends on: seeking to LookupKey(k, S) lands on the newest
// entry for k with sequence <= S. That holds iff lookup <= entry exactly for
// the entries with sequence <= S.
TEST(LookupKeyTest, SeeksToVersionsAtOrBeforeTheSnapshot) {
  const LookupKey lookup("k", 10);
  for (SequenceNumber s = 0; s <= 20; ++s) {
    for (const ValueType type : {ValueType::kValue, ValueType::kDeletion}) {
      const bool lookup_sorts_at_or_before =
          CompareInternalKeys(lookup.InternalKey(), Key("k", s, type)) <= 0;
      EXPECT_EQ(lookup_sorts_at_or_before, s <= 10) << "sequence " << s;
    }
  }
}

#ifdef FORGEDB_ENABLE_ASSERTS
TEST(InternalKeyDeathTest, SequenceAboveMaximumIsRejected) {
  EXPECT_DEATH(static_cast<void>(PackSequenceAndType(kMaxSequenceNumber + 1, ValueType::kValue)),
               "FORGEDB_CHECK failed");
}
#endif

}  // namespace
}  // namespace forgedb
