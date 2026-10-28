// Fuzz target (TODO 3.9): the block parser (table/block.h) on untrusted bytes.
//
// The first input byte picks a mode:
//
//   even  The rest is a block's contents as found in a file (after its
//         trailer was checked). Open() and the iterator must not crash or
//         read out of bounds (ASan). Errors are Corruption. A scan yields at
//         most one entry per 3 bytes. A Seek never returns a key before its
//         target.
//
//   odd   The rest describes entries (see ParseEntries). They are sorted, built
//         into a block with BlockBuilder (restart interval from the input),
//         and read back: the scan returns exactly the entries, and every Seek
//         lands where std::lower_bound says.
#include <algorithm>
#include <compare>
#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "forgedb/iterator.h"
#include "forgedb/result.h"
#include "forgedb/status.h"

#include "fuzz_expect.h"
#include "table/block.h"
#include "table/block_builder.h"

namespace {

using forgedb::Block;
using forgedb::Iterator;
using forgedb::Status;
using forgedb::fuzz::Expect;

std::strong_ordering Bytewise(std::string_view a, std::string_view b) noexcept {
  return a.compare(b) <=> 0;
}

bool IsCorruption(const Status& status) { return status.code() == Status::Code::kCorruption; }

void RawBlock(std::string_view contents) {
  forgedb::Result<Block> block = Block::Open(std::string(contents));
  if (!block.ok()) {
    Expect(IsCorruption(block.status()));
    return;
  }
  const std::unique_ptr<Iterator> it = block->NewIterator(&Bytewise);
  std::vector<std::string> keys;
  for (it->SeekToFirst(); it->Valid(); it->Next()) {
    keys.emplace_back(it->key());
    Expect(keys.size() * 3 <= contents.size());
  }
  Expect(it->status().ok() || IsCorruption(it->status()));

  // Seek never returns a key before its target. (Which key it returns is
  // promised only for blocks BlockBuilder wrote: these bytes may hold keys
  // out of order or restart points inside entries.)
  keys.emplace_back("");
  keys.emplace_back(300, '\xFF');
  for (const std::string& target : keys) {
    it->Seek(target);
    if (it->Valid()) {
      Expect(it->key() >= std::string_view(target));
    } else {
      Expect(it->status().ok() || IsCorruption(it->status()));
    }
  }
}

// Entries: each is a key-length byte, the key, a value-length byte, the
// value (lengths cut at the end of the input). Then sorted, duplicates
// dropped.
std::map<std::string, std::string> ParseEntries(std::string_view input) {
  std::map<std::string, std::string> entries;
  const auto take = [&input](std::size_t n) {
    const std::string_view part = input.substr(0, std::min(n, input.size()));
    input.remove_prefix(part.size());
    return std::string(part);
  };
  while (!input.empty()) {
    const auto key_size = static_cast<unsigned char>(input.front());
    input.remove_prefix(1);
    std::string key = take(key_size);
    std::size_t value_size = 0;
    if (!input.empty()) {
      value_size = static_cast<unsigned char>(input.front());
      input.remove_prefix(1);
    }
    entries.try_emplace(std::move(key), take(value_size));
  }
  return entries;
}

void RoundTrip(std::string_view input) {
  if (input.empty()) {
    return;
  }
  const std::size_t restart_interval = (static_cast<unsigned char>(input.front()) % 32U) + 1U;
  const std::map<std::string, std::string> entries = ParseEntries(input.substr(1));

  forgedb::BlockBuilder builder(&Bytewise, restart_interval);
  for (const auto& [key, value] : entries) {
    builder.Add(key, value);
  }
  forgedb::Result<Block> block = Block::Open(std::string(builder.Finish()));
  Expect(block.ok());
  const std::unique_ptr<Iterator> it = block->NewIterator(&Bytewise);

  auto want = entries.begin();
  for (it->SeekToFirst(); it->Valid(); it->Next(), ++want) {
    Expect(want != entries.end() && it->key() == want->first && it->value() == want->second);
  }
  Expect(it->status().ok() && want == entries.end());

  // Seek to every key, just after it, and just before it.
  std::vector<std::string> targets = {""};
  for (const auto& entry : entries) {
    const std::string& key = entry.first;
    targets.push_back(key);
    targets.push_back(key + '\0');
    if (!key.empty()) {
      targets.push_back(key.substr(0, key.size() - 1));
    }
  }
  for (const std::string& target : targets) {
    it->Seek(target);
    const auto expected = entries.lower_bound(target);
    if (expected == entries.end()) {
      Expect(!it->Valid() && it->status().ok());
    } else {
      Expect(it->Valid() && it->key() == expected->first && it->value() == expected->second);
    }
  }
}

}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
  if (size == 0) {
    return 0;
  }
  const std::span<const std::uint8_t> bytes(data, size);
  const std::string input(bytes.begin(), bytes.end());
  const std::string_view rest = std::string_view(input).substr(1);
  if ((bytes[0] & 1U) == 0) {
    RawBlock(rest);
  } else {
    RoundTrip(rest);
  }
  return 0;
}
