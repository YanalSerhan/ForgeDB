#include "tools/bench/histogram.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>

namespace forgedb::bench {

std::size_t LatencyHistogram::BucketOf(std::uint64_t value) noexcept {
  if (value < 16) {
    return static_cast<std::size_t>(value);
  }
  const auto exponent = static_cast<unsigned>(63 - std::countl_zero(value));  // 4 to 63
  const std::uint64_t sub = (value >> (exponent - 4U)) & 15U;
  return 16 + (static_cast<std::size_t>(exponent - 4U) * 16) + static_cast<std::size_t>(sub);
}

std::uint64_t LatencyHistogram::UpperEdge(std::size_t bucket) noexcept {
  if (bucket < 16) {
    return bucket;
  }
  const auto shift = static_cast<unsigned>((bucket - 16) / 16);  // exponent - 4
  const std::uint64_t sub = (bucket - 16) % 16;
  const std::uint64_t lower = (16 + sub) << shift;
  return lower + ((std::uint64_t{1} << shift) - 1);
}

void LatencyHistogram::Add(std::uint64_t nanoseconds) noexcept {
  ++buckets_[BucketOf(nanoseconds)];
  ++count_;
  min_ = std::min(min_, nanoseconds);
  max_ = std::max(max_, nanoseconds);
  sum_ += static_cast<double>(nanoseconds);
}

void LatencyHistogram::Merge(const LatencyHistogram& other) noexcept {
  for (std::size_t i = 0; i < kBuckets; ++i) {
    buckets_[i] += other.buckets_[i];
  }
  count_ += other.count_;
  min_ = std::min(min_, other.min_);
  max_ = std::max(max_, other.max_);
  sum_ += other.sum_;
}

double LatencyHistogram::Mean() const noexcept {
  return count_ == 0 ? 0 : sum_ / static_cast<double>(count_);
}

std::uint64_t LatencyHistogram::Percentile(double fraction) const noexcept {
  if (count_ == 0) {
    return 0;
  }
  // The rank of the sample wanted, 1 to count_.
  const double wanted = std::clamp(fraction, 0.0, 1.0) * static_cast<double>(count_);
  const auto rank = std::max<std::uint64_t>(1, static_cast<std::uint64_t>(std::ceil(wanted)));
  std::uint64_t seen = 0;
  for (std::size_t i = 0; i < kBuckets; ++i) {
    seen += buckets_[i];
    if (seen >= rank) {
      return std::min(UpperEdge(i), max_);
    }
  }
  return max_;
}

}  // namespace forgedb::bench
