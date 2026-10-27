// Fuzz target (TODO 2.7): decoding a write batch from untrusted bytes, as
// recovery does with every log record.
//
// The input is a batch's encoded bytes. Promises checked:
//   - decoding never crashes or reads out of bounds (ASan);
//   - Iterate is all or nothing: a malformed batch reaches the handler not
//     once, and InsertInto then fails without touching the memtable;
//   - a well-formed batch yields exactly Count() operations; re-encoding them
//     decodes to the same operations; InsertInto succeeds unless the sequence
//     numbers would pass kMaxSequenceNumber, and then adds Count() entries.
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <source_location>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "forgedb/write_batch.h"

#include "db/internal_key.h"
#include "db/write_batch_internal.h"
#include "memtable/memtable.h"

namespace {

// Like FORGEDB_CHECK, but aborts: FORGEDB_CHECK ends the process with
// _Exit, which bypasses libFuzzer's handler, so the input would not be saved.
void Expect(bool condition, std::source_location location = std::source_location::current()) {
  if (!condition) {
    std::fprintf(stderr, "%s(%u): broken promise\n",  // NOLINT(cppcoreguidelines-pro-type-vararg)
                 location.file_name(), static_cast<unsigned>(location.line()));
    std::abort();
  }
}

struct Operation {
  bool is_put = false;
  std::string key;
  std::string value;
  friend bool operator==(const Operation&, const Operation&) = default;
};

class Recorder final : public forgedb::WriteBatch::Handler {
 public:
  void Put(std::string_view key, std::string_view value) override {
    operations.push_back({.is_put = true, .key = std::string(key), .value = std::string(value)});
  }
  void Delete(std::string_view key) override {
    operations.push_back({.is_put = false, .key = std::string(key), .value = {}});
  }
  std::vector<Operation> operations;  // NOLINT(misc-non-private-member-variables-in-classes)
};

std::size_t EntriesIn(const forgedb::MemTable& memtable) {
  std::size_t entries = 0;
  forgedb::MemTable::Iterator it(&memtable);
  for (it.SeekToFirst(); it.Valid(); it.Next()) {
    ++entries;
  }
  return entries;
}

}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
  using forgedb::WriteBatchInternal;
  const std::span<const std::uint8_t> bytes(data, size);
  const std::string input(bytes.begin(), bytes.end());

  forgedb::WriteBatch batch;
  if (!WriteBatchInternal::SetContents(batch, input).ok()) {
    Expect(input.size() < WriteBatchInternal::kHeaderSize);
    return 0;
  }
  Recorder recorder;
  const forgedb::MemTableRef memtable = forgedb::MemTable::Create();
  if (!batch.Iterate(recorder).ok()) {
    Expect(recorder.operations.empty());  // all or nothing
    Expect(!WriteBatchInternal::InsertInto(batch, *memtable).ok());
    Expect(EntriesIn(*memtable) == 0);
    return 0;
  }
  Expect(recorder.operations.size() == batch.Count());

  // Re-encode and decode again: the same operations.
  forgedb::WriteBatch copy;
  for (const Operation& operation : recorder.operations) {
    if (operation.is_put) {
      copy.Put(operation.key, operation.value);
    } else {
      copy.Delete(operation.key);
    }
  }
  Recorder again;
  Expect(copy.Iterate(again).ok());
  Expect(again.operations == recorder.operations);

  // Apply: fails only if the sequence numbers would run out.
  const forgedb::SequenceNumber first = WriteBatchInternal::Sequence(batch);
  const std::uint32_t count = batch.Count();
  const bool exhausted = count > 0 && (first > forgedb::kMaxSequenceNumber ||
                                       count - 1 > forgedb::kMaxSequenceNumber - first);
  const bool inserted = WriteBatchInternal::InsertInto(batch, *memtable).ok();
  Expect(inserted == !exhausted);
  Expect(EntriesIn(*memtable) == (inserted ? count : 0));
  return 0;
}
