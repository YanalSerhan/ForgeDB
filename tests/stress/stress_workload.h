// The stress test's data and how a read is judged (TODO 4.5).
//
// Each writer owns `groups` groups of kKeysPerGroup keys and writes a group
// in one WriteBatch, all keys at the group's next version (1, 2, 3, ...).
// Whether version v of a group is a deletion is a function of (writer,
// group, v), and a value is a function of (writer, group, member, version),
// so a reader can judge what it sees without any history:
//
//   - A writer stores the version it starts into started[group] before the
//     Write, and into acked[group] after the Write returned OK.
//   - A reader loads lo = acked[group] before reading and hi =
//     started[group] after. What it read must be the group's state at some
//     version in [lo, hi]: a write acknowledged before the read began must
//     be visible, and one not started before it ended cannot be.
//   - Version 0 (never written) and deletion versions mean "no value".
//
// An iterator or a snapshot sees one moment, so it must also see all keys of
// a group at the same version (batches are atomic).
#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace forgedb::stress {

inline constexpr int kKeysPerGroup = 4;

struct KeyId {
  int writer = 0;
  int group = 0;
  int member = 0;
  bool operator==(const KeyId&) const = default;
};

// "w1-g0000042-k3": sorted by writer, then group, then member.
[[nodiscard]] std::string MakeKey(const KeyId& id);
[[nodiscard]] std::optional<KeyId> ParseKey(std::string_view key);
// The first key of the group (members follow it).
[[nodiscard]] std::string GroupStart(int writer, int group);

// Whether version `version` (1 or more) of the group deletes its keys.
[[nodiscard]] bool IsDeletion(int writer, int group, std::uint64_t version);
// Whether the group has no value at `version` (never written, or deleted).
[[nodiscard]] bool IsAbsent(int writer, int group, std::uint64_t version);

// The value of key `id` at `version` (not a deletion): a readable header
// ("w.g.m.v|"), then 1 to about 4 KiB of bytes that depend on all four.
[[nodiscard]] std::string MakeValue(const KeyId& id, std::uint64_t version);
// The version a value was made for, if it is exactly MakeValue(id, version)
// for some version; nullopt if it is damaged or belongs to another key.
[[nodiscard]] std::optional<std::uint64_t> ValueVersion(const KeyId& id, std::string_view value);

// "" if reading key `id` could have returned `observed` (nullopt: NotFound)
// given the window [lo, hi]; otherwise what is wrong.
[[nodiscard]] std::string CheckRead(const KeyId& id, const std::optional<std::string>& observed,
                                    std::uint64_t lo, std::uint64_t hi);

// What one moment's view (an iterator, a snapshot) showed of a group.
using GroupView = std::array<std::optional<std::string>, kKeysPerGroup>;

// "" if `view` is the group's state at one version in [lo, hi]: every key at
// that version, or none at all for an absent version.
[[nodiscard]] std::string CheckGroup(int writer, int group, const GroupView& view, std::uint64_t lo,
                                     std::uint64_t hi);

}  // namespace forgedb::stress
