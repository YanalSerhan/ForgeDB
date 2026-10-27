#include "table/bloom_filter.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <numbers>
#include <string>
#include <string_view>

#include "forgedb/check.h"

#include "util/hash.h"

namespace forgedb {

namespace {

constexpr int kMinProbes = 1;
constexpr int kMaxProbes = 30;
constexpr std::uint64_t kMinBits = 64;

// The bits a key sets: enhanced double hashing (Dillinger and Manolios, "Bloom
// Filters in Probabilistic Verification", FMCAD 2004). Plain double hashing,
// x + i*y, degrades when y shares a large power-of-two factor with the bit
// count (always a multiple of 8): the probes then cycle over a few bits, which
// doubled the worst false-positive rate of small filters in our tests (2.32%
// vs 1.18%, ADR 0005). Growing the step by 1, 2, 3, ... after each probe breaks
// those cycles.
class ProbeSequence {
 public:
  ProbeSequence(const Hash128& hash, std::uint64_t bits) noexcept
      : x_(hash.low), y_(hash.high), bits_(bits) {}

  std::uint64_t Next() noexcept {
    const std::uint64_t bit = x_ % bits_;
    x_ += y_;
    y_ += ++i_;
    return bit;
  }

 private:
  std::uint64_t x_;
  std::uint64_t y_;
  std::uint64_t bits_;
  std::uint64_t i_ = 0;
};

}  // namespace

int BloomFilterProbes(int bits_per_key) noexcept {
  const long k = std::lround(bits_per_key * std::numbers::ln2);
  return static_cast<int>(std::clamp<long>(k, kMinProbes, kMaxProbes));
}

BloomFilterBuilder::BloomFilterBuilder(int bits_per_key)
    : bits_per_key_(bits_per_key), probes_(BloomFilterProbes(bits_per_key)) {
  FORGEDB_CHECK(bits_per_key >= 1);
}

void BloomFilterBuilder::AddKey(std::string_view key) {
  if (!hashes_.empty() && key == last_key_) {
    return;
  }
  hashes_.push_back(MurmurHash3x64(key));
  last_key_.assign(key);
}

std::string BloomFilterBuilder::Finish() {
  const std::uint64_t wanted =
      static_cast<std::uint64_t>(hashes_.size()) * static_cast<std::uint64_t>(bits_per_key_);
  const std::uint64_t bytes = (std::max(wanted, kMinBits) + 7) / 8;
  const std::uint64_t bits = bytes * 8;

  std::string filter(static_cast<std::size_t>(bytes) + 1, '\0');
  for (const Hash128& hash : hashes_) {
    ProbeSequence probe(hash, bits);
    for (int i = 0; i < probes_; ++i) {
      const std::uint64_t bit = probe.Next();
      filter[bit / 8] =
          static_cast<char>(static_cast<unsigned char>(filter[bit / 8]) | (1U << (bit % 8)));
    }
  }
  filter.back() = static_cast<char>(probes_);

  hashes_.clear();
  last_key_.clear();
  return filter;
}

bool BloomFilterMayMatch(std::string_view filter, std::string_view key) noexcept {
  if (filter.size() < 2) {
    return true;
  }
  const int probes = static_cast<unsigned char>(filter.back());
  if (probes < kMinProbes || probes > kMaxProbes) {
    return true;
  }
  const std::uint64_t bits = static_cast<std::uint64_t>(filter.size() - 1) * 8;
  const Hash128 hash = MurmurHash3x64(key);
  ProbeSequence probe(hash, bits);
  for (int i = 0; i < probes; ++i) {
    const std::uint64_t bit = probe.Next();
    if ((static_cast<unsigned char>(filter[bit / 8]) & (1U << (bit % 8))) == 0) {
      return false;
    }
  }
  return true;
}

}  // namespace forgedb
