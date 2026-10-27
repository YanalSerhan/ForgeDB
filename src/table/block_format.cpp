#include "table/block_format.h"

#include <array>
#include <cstdint>
#include <string>
#include <string_view>

#include "forgedb/result.h"
#include "forgedb/status.h"

#include "util/coding.h"
#include "util/crc32c.h"

namespace forgedb {

namespace {

std::uint32_t TrailerCrc(std::string_view contents, char type) noexcept {
  return crc32c::Extend(crc32c::Value(contents), &type, 1);
}

}  // namespace

std::array<char, kBlockTrailerSize> MakeBlockTrailer(std::string_view contents,
                                                     BlockType type) noexcept {
  std::array<char, kBlockTrailerSize> trailer{};
  trailer[0] = static_cast<char>(type);
  EncodeFixed32(&trailer[1], crc32c::Mask(TrailerCrc(contents, trailer[0])));
  return trailer;
}

Result<std::string_view> CheckBlockTrailer(std::string_view stored) {
  if (stored.size() < kBlockTrailerSize) {
    return Status::Corruption("block too short for its trailer",
                              std::to_string(stored.size()) + " bytes");
  }
  const std::string_view contents = stored.substr(0, stored.size() - kBlockTrailerSize);
  const char type = stored[contents.size()];
  const std::uint32_t expected = crc32c::Unmask(DecodeFixed32(&stored[contents.size() + 1]));
  if (TrailerCrc(contents, type) != expected) {
    return Status::Corruption("block checksum mismatch");
  }
  // Checked after the CRC: a matching CRC means the writer chose this type.
  if (static_cast<std::uint8_t>(type) != static_cast<std::uint8_t>(BlockType::kUncompressed)) {
    return Status::Corruption("unknown block type",
                              std::to_string(static_cast<std::uint8_t>(type)));
  }
  return contents;
}

}  // namespace forgedb
