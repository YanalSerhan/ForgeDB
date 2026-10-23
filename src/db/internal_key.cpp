#include "db/internal_key.h"

#include <cstring>

#include "util/coding.h"

namespace forgedb {

void AppendInternalKey(std::string& dst, const ParsedInternalKey& key) {
  dst.append(key.user_key);
  PutFixed64(dst, PackSequenceAndType(key.sequence, key.type));
}

std::optional<ParsedInternalKey> ParseInternalKey(std::string_view internal_key) noexcept {
  if (internal_key.size() < kTagSize) {
    return std::nullopt;
  }
  std::string_view tag_bytes = internal_key.substr(internal_key.size() - kTagSize);
  const std::optional<std::uint64_t> tag = GetFixed64(tag_bytes);
  if (!tag) {
    return std::nullopt;  // unreachable: exactly kTagSize bytes are present
  }
  const auto type_byte = static_cast<std::uint8_t>(*tag & 0xFFU);
  if (type_byte > static_cast<std::uint8_t>(ValueType::kValue)) {
    return std::nullopt;
  }
  return ParsedInternalKey{.user_key = internal_key.substr(0, internal_key.size() - kTagSize),
                           .sequence = *tag >> 8,
                           .type = static_cast<ValueType>(type_byte)};
}

std::string_view ExtractUserKey(std::string_view internal_key) noexcept {
  FORGEDB_DCHECK(internal_key.size() >= kTagSize);
  return internal_key.substr(0, internal_key.size() - kTagSize);
}

std::uint64_t ExtractTag(std::string_view internal_key) noexcept {
  FORGEDB_DCHECK(internal_key.size() >= kTagSize);
  const std::string_view tag_bytes = internal_key.substr(internal_key.size() - kTagSize);
  std::uint64_t tag = 0;
  std::memcpy(&tag, tag_bytes.data(), tag_bytes.size());  // little-endian, see coding.h
  return tag;
}

std::strong_ordering CompareInternalKeys(std::string_view a, std::string_view b) noexcept {
  // std::string_view compares through char_traits<char>, which compares bytes
  // as unsigned char, so "\xff" sorts after "a", as with memcmp.
  if (const std::strong_ordering order = ExtractUserKey(a) <=> ExtractUserKey(b); order != 0) {
    return order;
  }
  return ExtractTag(b) <=> ExtractTag(a);  // reversed: larger tag (newer) first
}

LookupKey::LookupKey(std::string_view user_key, SequenceNumber sequence) {
  const std::size_t internal_size = user_key.size() + kTagSize;
  FORGEDB_CHECK(internal_size <= UINT32_MAX);  // the length is stored as a varint32
  const std::size_t needed = kMaxVarint32Bytes + internal_size;

  char* dst = inline_buffer_.data();
  if (needed > inline_buffer_.size()) {
    heap_buffer_.resize(needed);
    dst = heap_buffer_.data();
  }
  start_ = dst;
  dst = EncodeVarint32(dst, static_cast<std::uint32_t>(internal_size));
  key_start_ = dst;
  if (!user_key.empty()) {  // memcpy from a null data() is undefined, even for 0 bytes
    std::memcpy(dst, user_key.data(), user_key.size());
    dst += user_key.size();
  }
  EncodeFixed64(dst, PackSequenceAndType(sequence, kValueTypeForSeek));
  end_ = dst + kTagSize;
}

std::string_view LookupKey::MemtableKey() const noexcept {
  return {start_, static_cast<std::size_t>(end_ - start_)};
}

std::string_view LookupKey::InternalKey() const noexcept {
  return {key_start_, static_cast<std::size_t>(end_ - key_start_)};
}

std::string_view LookupKey::UserKey() const noexcept {
  return {key_start_, static_cast<std::size_t>(end_ - key_start_) - kTagSize};
}

}  // namespace forgedb
