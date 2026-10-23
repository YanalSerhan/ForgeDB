// Internal keys: how ForgeDB stores every version of a user key.
//
// An internal key is the user key followed by an 8-byte little-endian tag:
//
//   [ user key bytes ][ tag: (sequence << 8) | type ]
//
// Every write gets a sequence number, so several versions of one user key can
// coexist (in the memtable, in different tables) until compaction drops the
// ones no snapshot can see. A deletion is a version too: an entry of type
// kDeletion (a "tombstone") that hides the older values beneath it.
//
// Ordering (CompareInternalKeys):
//   1. user key ascending, bytewise as unsigned bytes (like memcmp)
//   2. tag descending: the newest version of a key comes first, so a reader
//      looking for "the newest version visible at sequence S" can seek to
//      (key, S) and take the first entry it lands on.
//
// Byte layout: docs/file-formats.md, "Internal key".
#pragma once

#include <array>
#include <compare>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

#include "forgedb/check.h"

namespace forgedb {

using SequenceNumber = std::uint64_t;

// Sequence numbers use 56 bits so that sequence and type pack into one 64-bit tag.
inline constexpr SequenceNumber kMaxSequenceNumber = (SequenceNumber{1} << 56) - 1;

// The low byte of every tag. These values are written to disk: never renumber them.
enum class ValueType : std::uint8_t {
  kDeletion = 0x0,
  kValue = 0x1,
};

// The type used in lookup keys. Because tags sort descending, a lookup at
// sequence S must use the *largest* tag at S, i.e. the highest ValueType, so
// that entries written at exactly S (of any type) sort at or after it.
inline constexpr ValueType kValueTypeForSeek = ValueType::kValue;

inline constexpr std::size_t kTagSize = 8;

[[nodiscard]] constexpr std::uint64_t PackSequenceAndType(SequenceNumber sequence,
                                                          ValueType type) noexcept {
  FORGEDB_DCHECK(sequence <= kMaxSequenceNumber);
  return (sequence << 8) | static_cast<std::uint64_t>(type);
}

// An internal key split into its parts. `user_key` points into the bytes it was
// parsed from.
struct ParsedInternalKey {
  std::string_view user_key;
  SequenceNumber sequence = 0;
  ValueType type = ValueType::kValue;

  friend bool operator==(const ParsedInternalKey&, const ParsedInternalKey&) = default;
};

// Appends the encoded internal key to `dst`.
void AppendInternalKey(std::string& dst, const ParsedInternalKey& key);

// Parses bytes that may come from disk: nullopt if they are shorter than a tag
// or the type byte is not a known ValueType.
[[nodiscard]] std::optional<ParsedInternalKey> ParseInternalKey(
    std::string_view internal_key) noexcept;

// For keys already known to be valid (e.g. produced by this process).
// Precondition: internal_key.size() >= kTagSize (checked in debug builds).
[[nodiscard]] std::string_view ExtractUserKey(std::string_view internal_key) noexcept;
[[nodiscard]] std::uint64_t ExtractTag(std::string_view internal_key) noexcept;

// User key ascending (unsigned bytewise), then tag descending.
[[nodiscard]] std::strong_ordering CompareInternalKeys(std::string_view a,
                                                       std::string_view b) noexcept;

struct InternalKeyComparator {
  [[nodiscard]] std::strong_ordering operator()(std::string_view a,
                                                std::string_view b) const noexcept {
    return CompareInternalKeys(a, b);
  }
};

// The key a Get(user_key) at snapshot `sequence` searches for. It holds the
// memtable's key encoding, varint32(internal key length) + internal key, and
// exposes three views of it. Keys up to ~190 bytes live in an inline buffer, so
// a typical Get does no heap allocation.
//
//   MemtableKey(): [varint32 length][user key][tag]
//   InternalKey():                  [user key][tag]
//   UserKey():                      [user key]
//
// The views point into this object, so it can be neither copied nor moved.
class LookupKey {
 public:
  LookupKey(std::string_view user_key, SequenceNumber sequence);
  ~LookupKey() = default;
  LookupKey(const LookupKey&) = delete;
  LookupKey& operator=(const LookupKey&) = delete;
  LookupKey(LookupKey&&) = delete;
  LookupKey& operator=(LookupKey&&) = delete;

  [[nodiscard]] std::string_view MemtableKey() const noexcept;
  [[nodiscard]] std::string_view InternalKey() const noexcept;
  [[nodiscard]] std::string_view UserKey() const noexcept;

 private:
  std::array<char, 200> inline_buffer_{};
  std::string heap_buffer_;          // only for long keys
  const char* start_ = nullptr;      // varint32 length
  const char* key_start_ = nullptr;  // internal key
  const char* end_ = nullptr;
};

}  // namespace forgedb
