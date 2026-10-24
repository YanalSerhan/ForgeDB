#include "log/log_writer.h"

#include <algorithm>
#include <array>
#include <cstdint>

#include "forgedb/check.h"

#include "util/coding.h"
#include "util/crc32c.h"

namespace forgedb::log {

Status Writer::AddRecord(std::string_view record) {
  std::string_view left = record;
  bool begin = true;
  // do-while: an empty record still gets one (empty) kFull fragment.
  do {
    const std::size_t leftover = kBlockSize - block_offset_;
    if (leftover < kHeaderSize) {
      // Too small for a header: zero-fill to the end of the block.
      static constexpr std::array<char, kHeaderSize - 1> kZeros{};
      if (leftover > 0) {
        FORGEDB_RETURN_IF_ERROR(file_->Append(std::string_view(kZeros.data(), leftover)));
      }
      block_offset_ = 0;
    }
    // May be 0 when exactly a header fits: then this block gets an empty
    // kFirst fragment and the payload starts in the next block.
    const std::size_t available = kBlockSize - block_offset_ - kHeaderSize;
    const std::size_t fragment_size = std::min(left.size(), available);
    const bool end = fragment_size == left.size();
    RecordType type = RecordType::kMiddle;
    if (begin && end) {
      type = RecordType::kFull;
    } else if (begin) {
      type = RecordType::kFirst;
    } else if (end) {
      type = RecordType::kLast;
    }
    FORGEDB_RETURN_IF_ERROR(EmitFragment(type, left.substr(0, fragment_size)));
    left.remove_prefix(fragment_size);
    begin = false;
  } while (!left.empty());
  return file_->Flush();
}

Status Writer::EmitFragment(RecordType type, std::string_view payload) {
  FORGEDB_DCHECK(payload.size() <= kBlockSize - kHeaderSize);
  FORGEDB_DCHECK(block_offset_ + kHeaderSize + payload.size() <= kBlockSize);

  const auto type_byte = static_cast<char>(type);
  std::uint32_t crc = crc32c::Value(std::string_view(&type_byte, 1));
  crc = crc32c::Extend(crc, payload);

  std::array<char, kHeaderSize> header{};
  EncodeFixed32(header.data(), crc32c::Mask(crc));
  const auto length = static_cast<std::uint16_t>(payload.size());
  header[4] = static_cast<char>(length & 0xFFU);
  header[5] = static_cast<char>(length >> 8U);
  header[6] = type_byte;

  FORGEDB_RETURN_IF_ERROR(file_->Append(std::string_view(header.data(), header.size())));
  FORGEDB_RETURN_IF_ERROR(file_->Append(payload));
  block_offset_ += kHeaderSize + payload.size();
  return Status::OK();
}

}  // namespace forgedb::log
