#include "forgedb/write_batch.h"

#include <cstdint>
#include <optional>
#include <string>

#include "forgedb/check.h"

#include "db/internal_key.h"
#include "db/write_batch_internal.h"
#include "memtable/memtable.h"
#include "util/coding.h"

namespace forgedb {
namespace {

constexpr std::size_t kHeaderSize = WriteBatchInternal::kHeaderSize;
constexpr std::size_t kCountOffset = 8;  // the count follows the 8-byte sequence

constexpr auto kPutTag = static_cast<char>(ValueType::kValue);
constexpr auto kDeleteTag = static_cast<char>(ValueType::kDeletion);

std::string AtOffset(std::size_t offset) { return "at byte " + std::to_string(offset); }

// Walks the records of an encoded batch, which may be untrusted bytes. Calls
// `handler` for each record when it is non-null; with nullptr it only
// validates. Every check that can fail happens here, so a batch that passes a
// validating walk is guaranteed to pass a handler walk too.
Status WalkRecords(std::string_view rep, WriteBatch::Handler* handler) {
  if (rep.size() < kHeaderSize) {
    return Status::Corruption("write batch shorter than its 12-byte header",
                              std::to_string(rep.size()) + " bytes");
  }
  const std::uint32_t declared_count = DecodeFixed32(rep.data() + kCountOffset);
  std::string_view input = rep.substr(kHeaderSize);
  std::uint64_t found = 0;  // 64-bit: a corrupt batch may hold more than 2^32 records
  while (!input.empty()) {
    const std::size_t record_offset = rep.size() - input.size();
    const char tag = input.front();
    input.remove_prefix(1);
    if (tag == kPutTag) {
      const std::optional<std::string_view> key = GetLengthPrefixed(input);
      const std::optional<std::string_view> value = key ? GetLengthPrefixed(input) : std::nullopt;
      if (!value) {
        return Status::Corruption("truncated Put record in write batch", AtOffset(record_offset));
      }
      if (handler != nullptr) {
        handler->Put(*key, *value);
      }
    } else if (tag == kDeleteTag) {
      const std::optional<std::string_view> key = GetLengthPrefixed(input);
      if (!key) {
        return Status::Corruption("truncated Delete record in write batch",
                                  AtOffset(record_offset));
      }
      if (handler != nullptr) {
        handler->Delete(*key);
      }
    } else {
      return Status::Corruption("unknown record type in write batch",
                                "type " + std::to_string(static_cast<unsigned char>(tag)) + " " +
                                    AtOffset(record_offset));
    }
    ++found;
  }
  if (found != declared_count) {
    return Status::Corruption(
        "write batch record count mismatch",
        "header says " + std::to_string(declared_count) + ", found " + std::to_string(found));
  }
  return Status::OK();
}

// Applies a batch's operations to a memtable, numbering them from `sequence`.
class MemTableInserter final : public WriteBatch::Handler {
 public:
  MemTableInserter(SequenceNumber sequence, MemTable* memtable) noexcept
      : sequence_(sequence), memtable_(memtable) {}

  void Put(std::string_view key, std::string_view value) override {
    memtable_->Add(sequence_, ValueType::kValue, key, value);
    ++sequence_;
  }
  void Delete(std::string_view key) override {
    memtable_->Add(sequence_, ValueType::kDeletion, key, {});
    ++sequence_;
  }

 private:
  SequenceNumber sequence_;
  MemTable* memtable_;
};

}  // namespace

// --- WriteBatch ------------------------------------------------------------------

WriteBatch::Handler::~Handler() = default;

WriteBatch::WriteBatch() : rep_(kHeaderSize, '\0') {}

void WriteBatch::Put(std::string_view key, std::string_view value) {
  // Lengths are stored as varint32 (see PutLengthPrefixed's precondition).
  FORGEDB_CHECK(key.size() <= UINT32_MAX && value.size() <= UINT32_MAX);
  const std::uint32_t count = Count();
  FORGEDB_CHECK(count < UINT32_MAX);
  WriteBatchInternal::SetCount(*this, count + 1);
  rep_.push_back(kPutTag);
  PutLengthPrefixed(rep_, key);
  PutLengthPrefixed(rep_, value);
}

void WriteBatch::Delete(std::string_view key) {
  FORGEDB_CHECK(key.size() <= UINT32_MAX);
  const std::uint32_t count = Count();
  FORGEDB_CHECK(count < UINT32_MAX);
  WriteBatchInternal::SetCount(*this, count + 1);
  rep_.push_back(kDeleteTag);
  PutLengthPrefixed(rep_, key);
}

void WriteBatch::Clear() { rep_.assign(kHeaderSize, '\0'); }

std::uint32_t WriteBatch::Count() const noexcept {
  return DecodeFixed32(rep_.data() + kCountOffset);
}

Status WriteBatch::Iterate(Handler& handler) const {
  // Validate everything first, so a malformed batch never reaches the handler.
  FORGEDB_RETURN_IF_ERROR(WalkRecords(rep_, nullptr));
  return WalkRecords(rep_, &handler);
}

// --- WriteBatchInternal ----------------------------------------------------------

void WriteBatchInternal::SetCount(WriteBatch& batch, std::uint32_t count) noexcept {
  EncodeFixed32(batch.rep_.data() + kCountOffset, count);
}

SequenceNumber WriteBatchInternal::Sequence(const WriteBatch& batch) noexcept {
  return DecodeFixed64(batch.rep_.data());
}

void WriteBatchInternal::SetSequence(WriteBatch& batch, SequenceNumber sequence) noexcept {
  EncodeFixed64(batch.rep_.data(), sequence);
}

void WriteBatchInternal::Append(WriteBatch& dst, const WriteBatch& src) {
  const std::uint64_t total = std::uint64_t{dst.Count()} + src.Count();
  FORGEDB_CHECK(total <= UINT32_MAX);
  SetCount(dst, static_cast<std::uint32_t>(total));
  dst.rep_.append(src.rep_, kHeaderSize);
}

Status WriteBatchInternal::SetContents(WriteBatch& batch, std::string_view contents) {
  if (contents.size() < kHeaderSize) {
    batch.Clear();
    return Status::Corruption("write batch shorter than its 12-byte header",
                              std::to_string(contents.size()) + " bytes");
  }
  batch.rep_.assign(contents);
  return Status::OK();
}

Status WriteBatchInternal::InsertInto(const WriteBatch& batch, MemTable& memtable) {
  // Check the sequence range before touching the memtable. The count is
  // checked against the records by Iterate's validation pass, so exactly
  // `count` operations get numbered.
  const SequenceNumber first = Sequence(batch);
  const std::uint32_t count = batch.Count();
  if (count > 0 && (first > kMaxSequenceNumber || count - 1 > kMaxSequenceNumber - first)) {
    return Status::Corruption(
        "write batch sequence numbers out of range",
        "first " + std::to_string(first) + ", count " + std::to_string(count));
  }
  MemTableInserter inserter(first, &memtable);
  return batch.Iterate(inserter);
}

}  // namespace forgedb
