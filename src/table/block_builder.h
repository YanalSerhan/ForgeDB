// BlockBuilder: builds one block of a table file from sorted key/value pairs.
//
//   BlockBuilder builder(&CompareInternalKeys);
//   for (const auto& [key, value] : sorted_entries) builder.Add(key, value);
//   const std::string_view contents = builder.Finish();
//   // write contents, then MakeBlockTrailer(contents)
//
// Keys are prefix-compressed: each entry stores only the bytes in which its
// key differs from the previous key. Every `restart_interval` entries a
// "restart point" stores its key in full, so a reader can binary-search the
// restart points and decode forward from there. The offsets of the restart
// points and their count end the block (docs/file-formats.md, "Block").
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "table/block_format.h"

namespace forgedb {

inline constexpr std::size_t kDefaultBlockRestartInterval = 16;

class BlockBuilder {
 public:
  // `compare` is used only to check, in debug builds, that keys are added in
  // strictly increasing order. Precondition: restart_interval >= 1.
  explicit BlockBuilder(KeyCompare compare,
                        std::size_t restart_interval = kDefaultBlockRestartInterval);

  // Appends an entry. Preconditions: Finish() not called since the last
  // Reset(); `key` sorts after every key added before it; key and value are
  // each shorter than 4 GiB.
  void Add(std::string_view key, std::string_view value);

  // Appends the restart array and returns the block's contents (without the
  // trailer). The view stays valid until the next Reset() or destruction.
  [[nodiscard]] std::string_view Finish();

  // Starts a new, empty block.
  void Reset();

  // The size of the contents Finish() would return now.
  [[nodiscard]] std::size_t CurrentSizeEstimate() const noexcept;

  [[nodiscard]] bool empty() const noexcept { return entries_ == 0; }

 private:
  KeyCompare compare_;
  std::size_t restart_interval_;
  std::string buffer_;
  std::vector<std::uint32_t> restarts_;
  std::size_t counter_ = 0;  // entries since the last restart point
  std::size_t entries_ = 0;
  std::string last_key_;
  bool finished_ = false;
};

}  // namespace forgedb
