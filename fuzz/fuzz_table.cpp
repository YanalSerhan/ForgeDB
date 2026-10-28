// Fuzz target (TODO 3.9): opening and reading a table file (footer, index,
// filter, data blocks) from untrusted bytes.
//
// The first input byte picks a mode:
//
//   even  The rest is a table file. Table::Open, the iterator and Get must
//         not crash or read out of bounds (ASan); every error is Corruption
//         (or NotSupported for a well-formed footer of an unknown version).
//
//   odd   The rest describes a table (see ParsePlan): settings, entries, and
//         optionally one damaged byte or a truncation. The table is written
//         with TableBuilder, damaged, and read back through a small block
//         cache. Undamaged, everything reads back exactly: the scan, and Get
//         at every entry's sequence number, below it, above it, and for
//         absent keys. Damaged, the open fails with Corruption, or every read
//         gives the right answer or stops with Corruption: never a wrong one,
//         since every byte of a table is covered by a CRC.
#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "forgedb/iterator.h"
#include "forgedb/result.h"
#include "forgedb/status.h"

#include "db/internal_key.h"
#include "env/file_system.h"
#include "env/mem_file_system.h"
#include "fuzz_expect.h"
#include "table/table.h"
#include "table/table_builder.h"

namespace {

using forgedb::Iterator;
using forgedb::LookupKey;
using forgedb::SequenceNumber;
using forgedb::Status;
using forgedb::Table;
using forgedb::TableLookup;
using forgedb::ValueType;
using forgedb::fuzz::Expect;

constexpr std::size_t kMaxEntries = 500;
constexpr std::size_t kCacheBytes = 4096;  // small: blocks are evicted and read again

bool IsCorruption(const Status& status) { return status.code() == Status::Code::kCorruption; }

void WriteFile(forgedb::FileSystem& fs, const char* name, std::string_view contents) {
  const std::unique_ptr<forgedb::WritableFile> file = std::move(fs.NewWritableFile(name)).value();
  Expect(file->Append(contents).ok());
  Expect(file->Close().ok());
}

forgedb::Result<std::unique_ptr<Table>> OpenTable(forgedb::FileSystem& fs,
                                                  forgedb::BlockCache* cache) {
  const std::uint64_t size = std::move(fs.GetFileSize("t.sst")).value();
  return Table::Open(std::move(fs.NewRandomAccessFile("t.sst")).value(), size, cache);
}

// --- even: raw bytes ------------------------------------------------------------------

void RawTable(std::string_view contents) {
  forgedb::MemFileSystem fs;
  WriteFile(fs, "t.sst", contents);
  forgedb::BlockCache cache(kCacheBytes);
  const forgedb::Result<std::unique_ptr<Table>> table = OpenTable(fs, &cache);
  if (!table.ok()) {
    Expect(IsCorruption(table.status()) || table.status().code() == Status::Code::kNotSupported);
    return;
  }
  std::vector<std::string> keys;
  {
    const std::unique_ptr<Iterator> it = (*table)->NewIterator();
    for (it->SeekToFirst(); it->Valid() && keys.size() < kMaxEntries; it->Next()) {
      keys.emplace_back(it->key());
    }
    Expect(it->status().ok() || IsCorruption(it->status()));
  }
  for (const std::string& key : keys) {
    const std::optional<forgedb::ParsedInternalKey> parsed = forgedb::ParseInternalKey(key);
    if (!parsed) {
      continue;
    }
    for (const SequenceNumber sequence : {parsed->sequence, forgedb::kMaxSequenceNumber}) {
      std::string value;
      const forgedb::Result<TableLookup> found =
          (*table)->Get(LookupKey(parsed->user_key, sequence), &value);
      Expect(found.ok() || IsCorruption(found.status()));
    }
  }
}

// --- odd: written, damaged, read back ----------------------------------------------------

struct Entry {
  std::string user_key;
  SequenceNumber sequence = 0;
  ValueType type = ValueType::kValue;
  std::string value;

  [[nodiscard]] std::string InternalKey() const {
    std::string key;
    forgedb::AppendInternalKey(key, {.user_key = user_key, .sequence = sequence, .type = type});
    return key;
  }
};

// Input: a settings byte (bits 0-1: block size 16, 64, 256 or 4096; bits
// 2-6: restart interval - 1; bit 7: no filter), three damage bytes (a
// position as a fraction of the file, 0xFFFF for none, then a byte to XOR
// there, 0 for a truncation at that position), then entries: a user-key
// length byte, the key, a flags byte (bit 0: a deletion), a value length
// byte, the value. Entry i gets sequence number i + 1.
struct Plan {
  forgedb::TableOptions options;
  std::uint16_t damage_at = 0xFFFF;
  std::uint8_t damage_xor = 0;
  std::vector<Entry> entries;  // in internal-key order
};

std::optional<Plan> ParsePlan(std::string_view input) {
  if (input.size() < 4) {
    return std::nullopt;
  }
  Plan plan;
  const auto settings = static_cast<unsigned char>(input[0]);
  constexpr std::array<std::size_t, 4> kBlockSizes = {16, 64, 256, 4096};
  plan.options.block_size = kBlockSizes.at(settings & 3U);
  plan.options.block_restart_interval = ((settings >> 2U) & 31U) + 1U;
  plan.options.filter_bits_per_key = (settings & 0x80U) != 0 ? 0 : 10;
  plan.damage_at = static_cast<std::uint16_t>((static_cast<unsigned char>(input[1]) << 8U) |
                                              static_cast<unsigned char>(input[2]));
  plan.damage_xor = static_cast<std::uint8_t>(input[3]);
  input.remove_prefix(4);

  const auto take = [&input](std::size_t n) {
    const std::string_view part = input.substr(0, std::min(n, input.size()));
    input.remove_prefix(part.size());
    return std::string(part);
  };
  const auto take_byte = [&input]() -> unsigned {
    if (input.empty()) {
      return 0;
    }
    const auto byte = static_cast<unsigned char>(input.front());
    input.remove_prefix(1);
    return byte;
  };
  while (!input.empty() && plan.entries.size() < kMaxEntries) {
    Entry entry;
    entry.user_key = take(take_byte());
    entry.type = (take_byte() & 1U) != 0 ? ValueType::kDeletion : ValueType::kValue;
    entry.value = take(take_byte());
    if (entry.type == ValueType::kDeletion) {
      entry.value.clear();
    }
    entry.sequence = plan.entries.size() + 1;
    plan.entries.push_back(std::move(entry));
  }
  std::ranges::sort(plan.entries, [](const Entry& a, const Entry& b) {
    return forgedb::CompareInternalKeys(a.InternalKey(), b.InternalKey()) < 0;
  });
  return plan;
}

// Every user key's versions, newest first: what a Get must find.
using Versions = std::map<std::string, std::vector<const Entry*>, std::less<>>;

Versions VersionsOf(const Plan& plan) {
  Versions versions;
  for (const Entry& entry : plan.entries) {  // in internal-key order: newest first per key
    versions[entry.user_key].push_back(&entry);
  }
  return versions;
}

// The newest version of `user_key` at or below `sequence`, or null.
const Entry* Newest(const Versions& versions, std::string_view user_key, SequenceNumber sequence) {
  const auto it = versions.find(user_key);
  if (it == versions.end()) {
    return nullptr;
  }
  for (const Entry* entry : it->second) {
    if (entry->sequence <= sequence) {
      return entry;
    }
  }
  return nullptr;
}

void CheckGet(const Table& table, const Versions& versions, std::string_view user_key,
              SequenceNumber sequence, bool damaged) {
  std::string value;
  const forgedb::Result<TableLookup> found = table.Get(LookupKey(user_key, sequence), &value);
  if (!found.ok()) {
    Expect(damaged && IsCorruption(found.status()));
    return;
  }
  const Entry* expected = Newest(versions, user_key, sequence);
  if (expected == nullptr) {
    Expect(*found == TableLookup::kNotFound);
  } else if (expected->type == ValueType::kDeletion) {
    Expect(*found == TableLookup::kDeleted);
  } else {
    Expect(*found == TableLookup::kFound && value == expected->value);
  }
}

void RoundTrip(std::string_view input) {
  const std::optional<Plan> plan = ParsePlan(input);
  if (!plan) {
    return;
  }
  forgedb::MemFileSystem fs;
  std::string bytes;
  {
    const std::unique_ptr<forgedb::WritableFile> file =
        std::move(fs.NewWritableFile("built")).value();
    forgedb::TableBuilder builder(plan->options, file.get());
    for (const Entry& entry : plan->entries) {
      builder.Add(entry.InternalKey(), entry.value);
    }
    Expect(builder.Finish().ok());
    Expect(file->Close().ok());
    bytes.resize(builder.FileSize());
    const std::unique_ptr<forgedb::RandomAccessFile> read =
        std::move(fs.NewRandomAccessFile("built")).value();
    Expect(std::move(read->Read(0, std::span<char>(bytes))).value() == bytes.size());
  }
  const bool damaged = plan->damage_at != 0xFFFF;
  if (damaged) {
    const std::size_t at = bytes.size() * plan->damage_at / 0xFFFFU;
    if (plan->damage_xor == 0) {
      bytes.resize(at);
    } else {
      bytes[at] = static_cast<char>(static_cast<unsigned char>(bytes[at]) ^ plan->damage_xor);
    }
  }
  WriteFile(fs, "t.sst", bytes);

  forgedb::BlockCache cache(kCacheBytes);
  const forgedb::Result<std::unique_ptr<Table>> table = OpenTable(fs, &cache);
  if (!table.ok()) {
    Expect(damaged && IsCorruption(table.status()));
    return;
  }
  // The scan: the entries in order, all of them unless it stops with
  // Corruption.
  {
    const std::unique_ptr<Iterator> it = (*table)->NewIterator();
    auto want = plan->entries.begin();
    for (it->SeekToFirst(); it->Valid(); it->Next(), ++want) {
      Expect(want != plan->entries.end() && it->key() == want->InternalKey() &&
             it->value() == want->value);
    }
    if (it->status().ok()) {
      Expect(want == plan->entries.end());
    } else {
      Expect(damaged && IsCorruption(it->status()));
    }
  }
  const Versions versions = VersionsOf(*plan);
  for (const Entry& entry : plan->entries) {
    CheckGet(**table, versions, entry.user_key, entry.sequence, damaged);
    CheckGet(**table, versions, entry.user_key, entry.sequence - 1, damaged);
    CheckGet(**table, versions, entry.user_key, forgedb::kMaxSequenceNumber, damaged);
    CheckGet(**table, versions, entry.user_key + '\0', forgedb::kMaxSequenceNumber, damaged);
  }
  CheckGet(**table, versions, "", forgedb::kMaxSequenceNumber, damaged);
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
    RawTable(rest);
  } else {
    RoundTrip(rest);
  }
  return 0;
}
