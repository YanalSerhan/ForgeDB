#include "memtable/memtable.h"

#include <algorithm>
#include <cstdint>

#include "forgedb/check.h"

#include "util/coding.h"

namespace forgedb {
namespace {

// A decoded entry. Both views point into the arena.
struct Entry {
  std::string_view internal_key;
  std::string_view value;
};

// Entries are written by this process (Add), so they are trusted: decoding
// can't fail unless memory is corrupted, which debug builds check. This
// decoder takes no end pointer because computing one could point past the end
// of the arena block (e.g. p + 5 when the entry ends sooner), which is
// undefined behavior even if never dereferenced.
const char* DecodeTrustedVarint32(const char* p, std::uint32_t& value) noexcept {
  std::uint32_t result = 0;
  for (unsigned shift = 0; shift <= 28; shift += 7) {
    const auto byte = static_cast<std::uint8_t>(*p);
    ++p;
    result |= static_cast<std::uint32_t>(byte & 0x7FU) << shift;
    if ((byte & 0x80U) == 0) {
      value = result;
      return p;
    }
  }
  ::forgedb::detail::CheckFailed("valid varint32 in a memtable entry");
}

// The internal key at the start of an entry, or of a LookupKey's memtable key
// (the two share their first part: varint32 length + internal key).
std::string_view DecodeInternalKey(const char* entry) noexcept {
  std::uint32_t length = 0;
  const char* const key = DecodeTrustedVarint32(entry, length);
  return {key, length};
}

Entry DecodeEntry(const char* entry) noexcept {
  const std::string_view internal_key = DecodeInternalKey(entry);
  std::uint32_t value_length = 0;
  const char* const value =
      DecodeTrustedVarint32(internal_key.data() + internal_key.size(), value_length);
  return {.internal_key = internal_key, .value = {value, value_length}};
}

}  // namespace

std::strong_ordering MemTable::KeyComparator::operator()(const char* a,
                                                         const char* b) const noexcept {
  return CompareInternalKeys(DecodeInternalKey(a), DecodeInternalKey(b));
}

MemTable::MemTable() : table_(KeyComparator{}, &arena_) {}

MemTableRef MemTable::Create() {
  // The only `new` of a MemTable; Unref() is the matching delete. MemTableRef
  // takes the first reference.
  return MemTableRef(new MemTable());  // NOLINT(cppcoreguidelines-owning-memory)
}

void MemTable::Ref() noexcept {
  // Relaxed: taking a reference requires already holding one (or the DB
  // mutex), so the memtable can't be destroyed concurrently; nothing else is
  // published by the increment.
  refs_.fetch_add(1, std::memory_order_relaxed);
}

void MemTable::Unref() noexcept {
  // acq_rel, as in std::shared_ptr: release so this thread's reads of the
  // memtable happen before its destruction by whichever thread drops the last
  // reference; acquire so that thread sees every other thread's releases.
  const int previous = refs_.fetch_sub(1, std::memory_order_acq_rel);
  FORGEDB_DCHECK(previous >= 1);
  if (previous == 1) {
    delete this;  // NOLINT(cppcoreguidelines-owning-memory): the matching new is in Create()
  }
}

void MemTable::Add(SequenceNumber sequence, ValueType type, std::string_view user_key,
                   std::string_view value) {
  // Both lengths are stored as varint32. Keys and values that large are
  // rejected long before this (options limits); this check guards memory.
  const std::size_t internal_key_size = user_key.size() + kTagSize;
  FORGEDB_CHECK(internal_key_size <= UINT32_MAX && value.size() <= UINT32_MAX);
  const auto key_length = static_cast<std::uint32_t>(internal_key_size);
  const auto value_length = static_cast<std::uint32_t>(value.size());

  // varint32 key length | user key | fixed64 tag | varint32 value length | value
  const std::size_t encoded_size =
      VarintLength(key_length) + internal_key_size + VarintLength(value_length) + value.size();
  char* const entry = arena_.Allocate(encoded_size);
  char* p = EncodeVarint32(entry, key_length);
  p = std::ranges::copy(user_key, p).out;  // no memcpy: data() may be null when empty
  EncodeFixed64(p, PackSequenceAndType(sequence, type));
  p += kTagSize;
  p = EncodeVarint32(p, value_length);
  p = std::ranges::copy(value, p).out;
  FORGEDB_DCHECK(p == entry + encoded_size);

  // The entry's bytes are written before Insert publishes the node with a
  // release store, so a reader that finds the node sees the whole entry.
  table_.Insert(entry);
}

MemTable::GetResult MemTable::Get(const LookupKey& key, std::string* value) const {
  FORGEDB_DCHECK(value != nullptr);
  // The lookup key sorts just before every version of the user key with a
  // sequence <= the lookup's (newest first), so Seek lands on the newest
  // visible version, if the user key has one.
  Table::Iterator it(&table_);
  it.Seek(key.MemtableKey().data());
  if (!it.Valid()) {
    return GetResult::kNotFound;
  }
  const Entry entry = DecodeEntry(it.key());
  if (ExtractUserKey(entry.internal_key) != key.UserKey()) {
    return GetResult::kNotFound;  // landed on the next user key: no visible version
  }
  const auto type = static_cast<ValueType>(ExtractTag(entry.internal_key) & 0xFFU);
  switch (type) {
    case ValueType::kValue:
      value->assign(entry.value);
      return GetResult::kFound;
    case ValueType::kDeletion:
      return GetResult::kDeleted;
  }
  // Add() only writes the two types above; anything else is memory corruption.
  ::forgedb::detail::CheckFailed("memtable entry with a known value type");
}

}  // namespace forgedb
