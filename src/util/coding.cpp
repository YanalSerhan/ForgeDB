#include "util/coding.h"

#include <array>

#include "forgedb/check.h"

namespace forgedb {

char* EncodeVarint64(char* dst, std::uint64_t value) noexcept {
  // Emit 7 bits at a time, lowest first; the high bit says "more bytes follow".
  while (value >= 0x80) {
    *dst++ = static_cast<char>((value & 0x7F) | 0x80);
    value >>= 7;
  }
  *dst++ = static_cast<char>(value);
  return dst;
}

char* EncodeVarint32(char* dst, std::uint32_t value) noexcept { return EncodeVarint64(dst, value); }

std::size_t VarintLength(std::uint64_t value) noexcept {
  std::size_t length = 1;
  while (value >= 0x80) {
    value >>= 7;
    ++length;
  }
  return length;
}

void PutFixed32(std::string& dst, std::uint32_t value) {
  std::array<char, sizeof(value)> buf{};
  EncodeFixed32(buf.data(), value);
  dst.append(buf.data(), buf.size());
}

void PutFixed64(std::string& dst, std::uint64_t value) {
  std::array<char, sizeof(value)> buf{};
  EncodeFixed64(buf.data(), value);
  dst.append(buf.data(), buf.size());
}

void PutVarint32(std::string& dst, std::uint32_t value) { PutVarint64(dst, value); }

void PutVarint64(std::string& dst, std::uint64_t value) {
  std::array<char, kMaxVarint64Bytes> buf{};
  const char* end = EncodeVarint64(buf.data(), value);
  dst.append(buf.data(), static_cast<std::size_t>(end - buf.data()));
}

void PutLengthPrefixed(std::string& dst, std::string_view value) {
  FORGEDB_CHECK(value.size() <= UINT32_MAX);
  PutVarint32(dst, static_cast<std::uint32_t>(value.size()));
  dst.append(value);
}

const char* DecodeVarint32(const char* p, const char* limit, std::uint32_t& value) noexcept {
  std::uint32_t result = 0;
  for (unsigned shift = 0; shift <= 28 && p < limit; shift += 7) {
    const auto byte = static_cast<std::uint8_t>(*p);
    ++p;
    // The 5th byte carries bits 28-31, so only its low 4 bits may be set. A
    // larger value would overflow 32 bits; a set high bit would mean a 6th byte.
    if (shift == 28 && byte > 0x0F) {
      return nullptr;
    }
    result |= static_cast<std::uint32_t>(byte & 0x7FU) << shift;
    if ((byte & 0x80U) == 0) {
      value = result;
      return p;
    }
  }
  return nullptr;  // ran out of input in the middle of the number
}

const char* DecodeVarint64(const char* p, const char* limit, std::uint64_t& value) noexcept {
  std::uint64_t result = 0;
  for (unsigned shift = 0; shift <= 63 && p < limit; shift += 7) {
    const auto byte = static_cast<std::uint8_t>(*p);
    ++p;
    // The 10th byte carries only bit 63.
    if (shift == 63 && byte > 0x01) {
      return nullptr;
    }
    result |= static_cast<std::uint64_t>(byte & 0x7FU) << shift;
    if ((byte & 0x80U) == 0) {
      value = result;
      return p;
    }
  }
  return nullptr;
}

std::optional<std::uint32_t> GetFixed32(std::string_view& input) noexcept {
  if (input.size() < sizeof(std::uint32_t)) {
    return std::nullopt;
  }
  // Size checked above; DecodeFixed32 reads exactly 4 bytes.
  // NOLINTNEXTLINE(bugprone-suspicious-stringview-data-usage)
  const std::uint32_t value = DecodeFixed32(input.data());
  input.remove_prefix(sizeof(std::uint32_t));
  return value;
}

std::optional<std::uint64_t> GetFixed64(std::string_view& input) noexcept {
  if (input.size() < sizeof(std::uint64_t)) {
    return std::nullopt;
  }
  // Size checked above; DecodeFixed64 reads exactly 8 bytes.
  // NOLINTNEXTLINE(bugprone-suspicious-stringview-data-usage)
  const std::uint64_t value = DecodeFixed64(input.data());
  input.remove_prefix(sizeof(std::uint64_t));
  return value;
}

std::optional<std::uint32_t> GetVarint32(std::string_view& input) noexcept {
  std::uint32_t value = 0;
  const char* next = DecodeVarint32(input.data(), input.data() + input.size(), value);
  if (next == nullptr) {
    return std::nullopt;
  }
  input.remove_prefix(static_cast<std::size_t>(next - input.data()));
  return value;
}

std::optional<std::uint64_t> GetVarint64(std::string_view& input) noexcept {
  std::uint64_t value = 0;
  const char* next = DecodeVarint64(input.data(), input.data() + input.size(), value);
  if (next == nullptr) {
    return std::nullopt;
  }
  input.remove_prefix(static_cast<std::size_t>(next - input.data()));
  return value;
}

std::optional<std::string_view> GetLengthPrefixed(std::string_view& input) noexcept {
  std::string_view rest = input;
  const std::optional<std::uint32_t> length = GetVarint32(rest);
  // The length field is untrusted: it must not claim more bytes than exist.
  if (!length || *length > rest.size()) {
    return std::nullopt;
  }
  const std::string_view value = rest.substr(0, *length);
  rest.remove_prefix(*length);
  input = rest;
  return value;
}

}  // namespace forgedb
