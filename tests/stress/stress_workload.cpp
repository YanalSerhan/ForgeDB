#include "stress/stress_workload.h"

#include <charconv>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>

namespace forgedb::stress {
namespace {

std::uint64_t Mix(std::uint64_t x) {  // splitmix64's finalizer
  x += 0x9E3779B97F4A7C15ULL;
  x = (x ^ (x >> 30U)) * 0xBF58476D1CE4E5B9ULL;
  x = (x ^ (x >> 27U)) * 0x94D049BB133111EBULL;
  return x ^ (x >> 31U);
}

std::uint64_t Hash(int writer, int group, int member, std::uint64_t version) {
  std::uint64_t h = Mix(static_cast<std::uint64_t>(writer));
  h = Mix(h ^ static_cast<std::uint64_t>(group));
  h = Mix(h ^ static_cast<std::uint64_t>(member));
  return Mix(h ^ version);
}

std::string Padded(int n, int width) {
  std::string digits = std::to_string(n);
  if (digits.size() < static_cast<std::size_t>(width)) {
    digits.insert(0, static_cast<std::size_t>(width) - digits.size(), '0');
  }
  return digits;
}

// Parses an unsigned number that fills `text` exactly.
template <typename T>
std::optional<T> Number(std::string_view text) {
  T value{};
  const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
  if (error != std::errc() || end != text.data() + text.size() || text.empty()) {
    return std::nullopt;
  }
  return value;
}

std::string Describe(const KeyId& id) { return MakeKey(id); }

std::string DescribeValue(const KeyId& id, const std::optional<std::string>& value) {
  if (!value) {
    return "NotFound";
  }
  const std::optional<std::uint64_t> version = ValueVersion(id, *value);
  if (!version) {
    return "a damaged or foreign value (" + std::to_string(value->size()) + " bytes, starting \"" +
           value->substr(0, 32) + "\")";
  }
  return "version " + std::to_string(*version);
}

}  // namespace

std::string MakeKey(const KeyId& id) {
  return "w" + std::to_string(id.writer) + "-g" + Padded(id.group, 7) + "-k" +
         std::to_string(id.member);
}

std::optional<KeyId> ParseKey(std::string_view key) {
  // w<writer>-g<7 digits>-k<member>
  if (key.size() < 2 || key[0] != 'w') {
    return std::nullopt;
  }
  const std::size_t g = key.find("-g");
  const std::size_t k = key.find("-k");
  if (g == std::string_view::npos || k == std::string_view::npos || k != g + 2 + 7) {
    return std::nullopt;
  }
  const std::optional<int> writer = Number<int>(key.substr(1, g - 1));
  const std::optional<int> group = Number<int>(key.substr(g + 2, 7));
  const std::optional<int> member = Number<int>(key.substr(k + 2));
  if (!writer || !group || !member || *member >= kKeysPerGroup) {
    return std::nullopt;
  }
  KeyId id{.writer = *writer, .group = *group, .member = *member};
  if (MakeKey(id) != key) {
    return std::nullopt;  // e.g. a leading zero in the writer
  }
  return id;
}

std::string GroupStart(int writer, int group) {
  return MakeKey({.writer = writer, .group = group, .member = 0});
}

bool IsDeletion(int writer, int group, std::uint64_t version) {
  return version > 0 && Hash(writer, group, -1, version) % 5 == 0;  // one in five
}

bool IsAbsent(int writer, int group, std::uint64_t version) {
  return version == 0 || IsDeletion(writer, group, version);
}

std::string MakeValue(const KeyId& id, std::uint64_t version) {
  std::string value = std::to_string(id.writer) + "." + std::to_string(id.group) + "." +
                      std::to_string(id.member) + "." + std::to_string(version) + "|";
  std::uint64_t h = Hash(id.writer, id.group, id.member, version);
  // Mostly short values; one in eight up to 4 KiB.
  const std::uint64_t length = (h % 8 == 0) ? 1 + ((h >> 8U) % 4096) : 1 + ((h >> 8U) % 200);
  value.reserve(value.size() + length);
  for (std::uint64_t i = 0; i < length; ++i) {
    if (i % 8 == 0) {
      h = Mix(h);
    }
    value.push_back(static_cast<char>('a' + ((h >> ((i % 8) * 8)) % 26)));
  }
  return value;
}

std::optional<std::uint64_t> ValueVersion(const KeyId& id, std::string_view value) {
  const std::size_t bar = value.find('|');
  if (bar == std::string_view::npos) {
    return std::nullopt;
  }
  const std::string_view header = value.substr(0, bar);
  const std::size_t last_dot = header.rfind('.');
  if (last_dot == std::string_view::npos) {
    return std::nullopt;
  }
  const std::optional<std::uint64_t> version = Number<std::uint64_t>(header.substr(last_dot + 1));
  if (!version || *version == 0 || MakeValue(id, *version) != value) {
    return std::nullopt;
  }
  return version;
}

std::string CheckRead(const KeyId& id, const std::optional<std::string>& observed, std::uint64_t lo,
                      std::uint64_t hi) {
  const std::string window = " (window " + std::to_string(lo) + ".." + std::to_string(hi) + ")";
  if (!observed) {
    for (std::uint64_t v = lo; v <= hi; ++v) {
      if (IsAbsent(id.writer, id.group, v)) {
        return "";
      }
    }
    return Describe(id) + ": NotFound, but no version in the window deletes it" + window;
  }
  const std::optional<std::uint64_t> version = ValueVersion(id, *observed);
  if (!version) {
    return Describe(id) + ": read " + DescribeValue(id, observed) + window;
  }
  if (*version < lo || *version > hi) {
    return Describe(id) + ": read version " + std::to_string(*version) + ", outside" + window;
  }
  if (IsDeletion(id.writer, id.group, *version)) {
    return Describe(id) + ": read version " + std::to_string(*version) + ", which deleted the key" +
           window;
  }
  return "";
}

std::string CheckGroup(int writer, int group, const GroupView& view, std::uint64_t lo,
                       std::uint64_t hi) {
  const KeyId first{.writer = writer, .group = group, .member = 0};
  if (std::string problem = CheckRead(first, view[0], lo, hi); !problem.empty()) {
    return problem;
  }
  const std::optional<std::uint64_t> version =
      view[0].has_value() ? ValueVersion(first, view[0].value_or("")) : std::nullopt;
  for (int member = 1; member < kKeysPerGroup; ++member) {
    const KeyId id{.writer = writer, .group = group, .member = member};
    const std::optional<std::string>& seen = view[static_cast<std::size_t>(member)];
    const bool same = version ? (seen && ValueVersion(id, *seen) == version) : !seen;
    if (!same) {
      return "group " + GroupStart(writer, group) + " is torn: member 0 has " +
             DescribeValue(first, view[0]) + ", member " + std::to_string(member) + " has " +
             DescribeValue(id, seen);
    }
  }
  return "";
}

}  // namespace forgedb::stress
