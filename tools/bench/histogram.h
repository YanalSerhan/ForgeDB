// LatencyHistogram: operation latencies for forgedb_bench (TODO 5.1).
//
// Values (nanoseconds) go into log-linear buckets: below 16 one bucket per
// value, above that 16 buckets per power of two. A percentile is reported as
// the upper edge of its bucket, so it is at most 1/16 (6.25%) above the true
// value; the minimum, maximum and mean are exact. Each thread fills its own
// histogram; Merge combines them.
#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace forgedb::bench {

class LatencyHistogram {
 public:
  void Add(std::uint64_t nanoseconds) noexcept;
  void Merge(const LatencyHistogram& other) noexcept;

  [[nodiscard]] std::uint64_t Count() const noexcept { return count_; }
  [[nodiscard]] std::uint64_t Min() const noexcept { return count_ == 0 ? 0 : min_; }
  [[nodiscard]] std::uint64_t Max() const noexcept { return max_; }
  [[nodiscard]] double Mean() const noexcept;
  // The value below which `fraction` (0 to 1) of the samples fall, as the
  // upper edge of its bucket (never above Max()); 0 if empty.
  [[nodiscard]] std::uint64_t Percentile(double fraction) const noexcept;

  // Bucket arithmetic (exposed for tests).
  static constexpr std::size_t kBuckets = 16 + (60 * 16);
  [[nodiscard]] static std::size_t BucketOf(std::uint64_t value) noexcept;
  // The largest value that falls in `bucket`.
  [[nodiscard]] static std::uint64_t UpperEdge(std::size_t bucket) noexcept;

 private:
  // On the heap: results and per-thread tallies each hold a histogram, and
  // 7.8 KB apiece on the stack added up (MSVC /analyze, C6262).
  std::vector<std::uint64_t> buckets_ = std::vector<std::uint64_t>(kBuckets);
  std::uint64_t count_ = 0;
  std::uint64_t min_ = UINT64_MAX;
  std::uint64_t max_ = 0;
  double sum_ = 0;
};

}  // namespace forgedb::bench
