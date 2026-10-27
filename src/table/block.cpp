#include "table/block.h"

#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

#include "forgedb/check.h"
#include "forgedb/iterator.h"
#include "forgedb/result.h"
#include "forgedb/status.h"

#include "util/coding.h"

namespace forgedb {

namespace {

constexpr std::size_t kU32Size = sizeof(std::uint32_t);

// One decoded entry header: [shared][unshared][value length], followed by the
// `unshared` key bytes and the value.
struct EntryHeader {
  std::uint32_t shared = 0;
  std::uint32_t unshared = 0;
  std::uint32_t value_length = 0;
  const char* key_delta = nullptr;  // the unshared key bytes; the value follows
};

// Decodes the entry header at p, whose entry must end by `limit`. nullopt if
// the varints are truncated or the key bytes and value run past `limit`.
std::optional<EntryHeader> DecodeEntryHeader(const char* p, const char* limit) noexcept {
  EntryHeader header;
  p = DecodeVarint32(p, limit, header.shared);
  if (p == nullptr) {
    return std::nullopt;
  }
  p = DecodeVarint32(p, limit, header.unshared);
  if (p == nullptr) {
    return std::nullopt;
  }
  p = DecodeVarint32(p, limit, header.value_length);
  if (p == nullptr) {
    return std::nullopt;
  }
  const auto available = static_cast<std::uint64_t>(limit - p);
  if (std::uint64_t{header.unshared} + header.value_length > available) {
    return std::nullopt;
  }
  header.key_delta = p;
  return header;
}

class BlockIterator final : public Iterator {
 public:
  BlockIterator(const char* data, std::uint32_t restarts_offset, std::uint32_t num_restarts,
                KeyCompare compare) noexcept
      : data_(data),
        restarts_offset_(restarts_offset),
        num_restarts_(num_restarts),
        compare_(compare) {}

  [[nodiscard]] bool Valid() const override { return valid_; }

  void SeekToFirst() override {
    if (!status_.ok()) {
      return;
    }
    SeekToRestart(0);
    ParseNextEntry();
  }

  void Seek(std::string_view target) override {
    if (!status_.ok()) {
      return;
    }
    // Find the last restart point whose key is < target (or restart 0): the
    // first key >= target is at or after it, and before the next one.
    std::uint32_t left = 0;
    std::uint32_t right = num_restarts_ - 1;
    while (left < right) {
      const std::uint32_t mid = left + ((right - left + 1) / 2);
      const std::optional<std::string_view> key = RestartKey(mid);
      if (!key) {
        return;  // RestartKey recorded the corruption
      }
      if (compare_(*key, target) < 0) {
        left = mid;
      } else {
        right = mid - 1;
      }
    }
    SeekToRestart(left);
    while (ParseNextEntry()) {
      if (compare_(key_, target) >= 0) {
        return;
      }
    }
  }

  void Next() override {
    FORGEDB_DCHECK(valid_);
    ParseNextEntry();
  }

  [[nodiscard]] std::string_view key() const override {
    FORGEDB_DCHECK(valid_);
    return key_;
  }

  [[nodiscard]] std::string_view value() const override {
    FORGEDB_DCHECK(valid_);
    return value_;
  }

  [[nodiscard]] Status status() const override { return status_; }

 private:
  [[nodiscard]] std::uint32_t RestartOffset(std::uint32_t index) const noexcept {
    FORGEDB_DCHECK(index < num_restarts_);
    return DecodeFixed32(data_ + restarts_offset_ + (std::size_t{index} * kU32Size));
  }

  // Positions before the entry at restart point `index`; ParseNextEntry() then
  // decodes it.
  void SeekToRestart(std::uint32_t index) noexcept {
    valid_ = false;
    key_.clear();
    restart_index_ = index;
    next_ = RestartOffset(index);
  }

  // The full key stored at restart point `index`, or nullopt after recording
  // corruption.
  std::optional<std::string_view> RestartKey(std::uint32_t index) {
    const std::uint32_t offset = RestartOffset(index);
    const std::optional<EntryHeader> header =
        DecodeEntryHeader(data_ + offset, data_ + restarts_offset_);
    if (!header) {
      Corrupt("truncated block entry", offset);
      return std::nullopt;
    }
    if (header->shared != 0) {
      Corrupt("block restart point does not store its full key", offset);
      return std::nullopt;
    }
    return std::string_view(header->key_delta, header->unshared);
  }

  // Decodes the entry at next_. False (and not Valid()) at the end of the
  // entries or on corruption.
  bool ParseNextEntry() {
    valid_ = false;
    const std::uint32_t offset = next_;
    if (offset >= restarts_offset_) {
      return false;  // past the last entry
    }
    const std::optional<EntryHeader> header =
        DecodeEntryHeader(data_ + offset, data_ + restarts_offset_);
    if (!header) {
      Corrupt("truncated block entry", offset);
      return false;
    }
    // Restart offsets strictly increase and every entry boundary is checked
    // against the next one, so the walk lands on each restart point exactly.
    if (restart_index_ + 1 < num_restarts_ && RestartOffset(restart_index_ + 1) == offset) {
      ++restart_index_;
    }
    if (RestartOffset(restart_index_) == offset && header->shared != 0) {
      Corrupt("block restart point does not store its full key", offset);
      return false;
    }
    if (header->shared > key_.size()) {
      Corrupt("block entry shares more bytes than the previous key has", offset);
      return false;
    }
    const char* value = header->key_delta + header->unshared;
    const auto end = static_cast<std::uint32_t>(value + header->value_length - data_);
    if (restart_index_ + 1 < num_restarts_ && RestartOffset(restart_index_ + 1) < end) {
      Corrupt("block restart point inside an entry", offset);
      return false;
    }
    key_.resize(header->shared);
    key_.append(header->key_delta, header->unshared);
    value_ = std::string_view(value, header->value_length);
    next_ = end;
    valid_ = true;
    return true;
  }

  void Corrupt(std::string_view what, std::uint32_t offset) {
    valid_ = false;
    status_ = Status::Corruption(what, "at block offset " + std::to_string(offset));
  }

  const char* data_;
  std::uint32_t restarts_offset_;
  std::uint32_t num_restarts_;
  KeyCompare compare_;

  bool valid_ = false;
  std::uint32_t restart_index_ = 0;  // the restart point at or before the current entry
  std::uint32_t next_ = 0;           // offset of the entry after the current one
  std::string key_;
  std::string_view value_;
  Status status_;
};

}  // namespace

Result<Block> Block::Open(std::string contents) {
  const std::size_t size = contents.size();
  if (size > std::numeric_limits<std::uint32_t>::max()) {
    return Status::Corruption("block larger than 4 GiB");
  }
  if (size < kU32Size) {
    return Status::Corruption("block too short for its restart count",
                              std::to_string(size) + " bytes");
  }
  const std::uint32_t num_restarts = DecodeFixed32(&contents[size - kU32Size]);
  if (num_restarts == 0) {
    return Status::Corruption("block has no restart points");
  }
  if (num_restarts > (size - kU32Size) / kU32Size) {
    return Status::Corruption("block restart count larger than the block",
                              std::to_string(num_restarts));
  }
  const auto restarts_offset =
      static_cast<std::uint32_t>(size - kU32Size - (std::size_t{num_restarts} * kU32Size));

  // An empty block (no entries) has exactly one restart point, at offset 0.
  std::uint32_t previous = 0;
  for (std::uint32_t i = 0; i < num_restarts; ++i) {
    const std::uint32_t offset =
        DecodeFixed32(&contents[restarts_offset + (std::size_t{i} * kU32Size)]);
    if (i == 0) {
      if (offset != 0) {
        return Status::Corruption("block's first restart point is not at offset 0",
                                  std::to_string(offset));
      }
      if (restarts_offset == 0 && num_restarts != 1) {
        return Status::Corruption("empty block has more than one restart point");
      }
    } else if (offset <= previous || offset >= restarts_offset) {
      return Status::Corruption(
          "block restart points out of order or outside the entries",
          "restart " + std::to_string(i) + " at offset " + std::to_string(offset));
    }
    previous = offset;
  }
  return Block(std::move(contents), restarts_offset, num_restarts);
}

std::unique_ptr<Iterator> Block::NewIterator(KeyCompare compare) const {
  return std::make_unique<BlockIterator>(contents_.data(), restarts_offset_, num_restarts_,
                                         compare);
}

}  // namespace forgedb
