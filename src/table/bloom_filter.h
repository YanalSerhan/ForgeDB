// Bloom filter: answers "might this table contain user key K?" with no false
// negatives and a small, tunable rate of false positives, so a Get can skip a
// table without reading any of its data blocks.
//
//   BloomFilterBuilder builder(10);  // bits per key
//   for (each user key in the table, in order) builder.AddKey(user_key);
//   const std::string filter = builder.Finish();
//   ...
//   if (!BloomFilterMayMatch(filter, user_key)) { /* not in this table */ }
//
// One filter covers a whole table (ADR 0005), and it holds user keys, not
// internal keys: a Get asks whether the key is there at any version.
//
// Layout: [bit array: m bits = m/8 bytes][k: 1 byte]. Each key sets k bits,
// chosen by enhanced double hashing from MurmurHash3 x64_128 (seed 0) of the
// key, in unsigned 64-bit wrap-around arithmetic:
//   x = hash.low; y = hash.high
//   for i = 1..k: set bit (x mod m); x += y; y += i
// (docs/file-formats.md, "Bloom filter").
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "util/hash.h"

namespace forgedb {

// k = round(bits_per_key x ln 2), clamped to [1, 30]: the number of bits each
// key sets. ln 2 bits-per-key is the k that minimizes false positives.
[[nodiscard]] int BloomFilterProbes(int bits_per_key) noexcept;

class BloomFilterBuilder {
 public:
  // Precondition: bits_per_key >= 1.
  explicit BloomFilterBuilder(int bits_per_key);

  // Adds a key. A key equal to the one added just before it is skipped, so
  // the several versions of one user key in a table count once.
  void AddKey(std::string_view key);

  // The filter for every key added since construction or the last Finish(),
  // which also starts a new, empty filter. A filter for no keys matches
  // nothing.
  [[nodiscard]] std::string Finish();

  // Distinct keys added to the current filter.
  [[nodiscard]] std::size_t NumKeys() const noexcept { return hashes_.size(); }

 private:
  int bits_per_key_;
  int probes_;
  std::vector<Hash128> hashes_;
  std::string last_key_;
};

// False only if `key` was certainly not added to `filter`. A malformed filter
// (shorter than 2 bytes, or k outside [1, 30]) matches everything: it can cost
// extra reads, never hide a key.
[[nodiscard]] bool BloomFilterMayMatch(std::string_view filter, std::string_view key) noexcept;

}  // namespace forgedb
