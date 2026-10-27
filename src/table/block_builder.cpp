#include "table/block_builder.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <string_view>

#include "forgedb/check.h"

#include "util/coding.h"

namespace forgedb {

namespace {

constexpr std::size_t kMaxU32 = std::numeric_limits<std::uint32_t>::max();

std::size_t SharedPrefixLength(std::string_view a, std::string_view b) noexcept {
  const std::size_t limit = std::min(a.size(), b.size());
  std::size_t n = 0;
  while (n < limit && a[n] == b[n]) {
    ++n;
  }
  return n;
}

}  // namespace

BlockBuilder::BlockBuilder(KeyCompare compare, std::size_t restart_interval)
    : compare_(compare), restart_interval_(restart_interval), restarts_{0} {
  FORGEDB_CHECK(restart_interval >= 1);
}

void BlockBuilder::Add(std::string_view key, std::string_view value) {
  FORGEDB_DCHECK(!finished_);
  FORGEDB_DCHECK(entries_ == 0 || compare_(key, last_key_) > 0);
  // Lengths and offsets are stored in 32 bits; a larger entry or block would
  // be written truncated, so this is checked in every build.
  FORGEDB_CHECK(key.size() <= kMaxU32 && value.size() <= kMaxU32);

  std::size_t shared = 0;
  if (counter_ < restart_interval_) {
    shared = SharedPrefixLength(last_key_, key);
  } else {
    FORGEDB_CHECK(buffer_.size() <= kMaxU32);
    restarts_.push_back(static_cast<std::uint32_t>(buffer_.size()));
    counter_ = 0;
  }
  const std::size_t unshared = key.size() - shared;

  PutVarint32(buffer_, static_cast<std::uint32_t>(shared));
  PutVarint32(buffer_, static_cast<std::uint32_t>(unshared));
  PutVarint32(buffer_, static_cast<std::uint32_t>(value.size()));
  buffer_.append(key.substr(shared));
  buffer_.append(value);
  // The restart array and count are fixed32 offsets into the block.
  FORGEDB_CHECK(CurrentSizeEstimate() <= kMaxU32);

  last_key_.resize(shared);
  last_key_.append(key.substr(shared));
  ++counter_;
  ++entries_;
}

std::string_view BlockBuilder::Finish() {
  FORGEDB_DCHECK(!finished_);
  for (const std::uint32_t restart : restarts_) {
    PutFixed32(buffer_, restart);
  }
  PutFixed32(buffer_, static_cast<std::uint32_t>(restarts_.size()));
  finished_ = true;
  return buffer_;
}

void BlockBuilder::Reset() {
  buffer_.clear();
  restarts_.assign(1, 0);
  counter_ = 0;
  entries_ = 0;
  last_key_.clear();
  finished_ = false;
}

std::size_t BlockBuilder::CurrentSizeEstimate() const noexcept {
  if (finished_) {
    return buffer_.size();
  }
  return buffer_.size() + (restarts_.size() * sizeof(std::uint32_t)) + sizeof(std::uint32_t);
}

}  // namespace forgedb
