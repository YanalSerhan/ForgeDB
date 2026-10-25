#include "db/filename.h"

#include <cstddef>

namespace forgedb {
namespace {

constexpr std::size_t kMinDigits = 6;

// The number in decimal, zero-padded to at least six digits.
std::string NumberText(std::uint64_t number) {
  std::string digits = std::to_string(number);
  if (digits.size() < kMinDigits) {
    digits.insert(0, kMinDigits - digits.size(), '0');
  }
  return digits;
}

// Parses digits written exactly as NumberText writes them.
std::optional<std::uint64_t> ParseNumber(std::string_view text) noexcept {
  if (text.size() < kMinDigits || text.size() > 20) {  // 2^64 - 1 has 20 digits
    return std::nullopt;
  }
  std::uint64_t value = 0;
  for (const char c : text) {
    if (c < '0' || c > '9') {
      return std::nullopt;
    }
    const auto digit = static_cast<std::uint64_t>(c - '0');
    if (value > (UINT64_MAX - digit) / 10) {
      return std::nullopt;  // overflow
    }
    value = (value * 10) + digit;
  }
  // Canonical form only: more than six digits means no leading zero.
  if (text.size() > kMinDigits && text.front() == '0') {
    return std::nullopt;
  }
  return value;
}

}  // namespace

std::filesystem::path LogFileName(const std::filesystem::path& db, std::uint64_t number) {
  return db / (NumberText(number) + ".log");
}

std::filesystem::path TableFileName(const std::filesystem::path& db, std::uint64_t number) {
  return db / (NumberText(number) + ".sst");
}

std::filesystem::path DescriptorFileName(const std::filesystem::path& db, std::uint64_t number) {
  return db / ("MANIFEST-" + NumberText(number));
}

std::filesystem::path CurrentFileName(const std::filesystem::path& db) { return db / "CURRENT"; }

std::filesystem::path LockFileName(const std::filesystem::path& db) { return db / "LOCK"; }

std::optional<ParsedFileName> ParseFileName(std::string_view name) noexcept {
  if (name == "CURRENT") {
    return ParsedFileName{.type = FileType::kCurrent, .number = 0};
  }
  if (name == "LOCK") {
    return ParsedFileName{.type = FileType::kLock, .number = 0};
  }
  constexpr std::string_view kManifestPrefix = "MANIFEST-";
  if (name.starts_with(kManifestPrefix)) {
    const std::optional<std::uint64_t> number = ParseNumber(name.substr(kManifestPrefix.size()));
    if (!number) {
      return std::nullopt;
    }
    return ParsedFileName{.type = FileType::kDescriptor, .number = *number};
  }
  for (const auto& [suffix, type] :
       {std::pair<std::string_view, FileType>{".log", FileType::kLog},
        std::pair<std::string_view, FileType>{".sst", FileType::kTable}}) {
    if (name.ends_with(suffix)) {
      const std::optional<std::uint64_t> number =
          ParseNumber(name.substr(0, name.size() - suffix.size()));
      if (!number) {
        return std::nullopt;
      }
      return ParsedFileName{.type = type, .number = *number};
    }
  }
  return std::nullopt;
}

}  // namespace forgedb
