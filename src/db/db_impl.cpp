#include "db/db_impl.h"

#include <cstdint>
#include <string>
#include <utility>

#include "forgedb/check.h"

#include "db/db_iter.h"
#include "db/write_batch_internal.h"

namespace forgedb {
namespace {

Status CheckKeySize(std::string_view key) {
  if (key.size() > kMaxKeySize) {
    return Status::InvalidArgument("key too large", std::to_string(key.size()) + " bytes, limit " +
                                                        std::to_string(kMaxKeySize));
  }
  return Status::OK();
}

Status CheckValueSize(std::string_view value) {
  if (value.size() > kMaxValueSize) {
    return Status::InvalidArgument(
        "value too large",
        std::to_string(value.size()) + " bytes, limit " + std::to_string(kMaxValueSize));
  }
  return Status::OK();
}

// Checks every key and value in a batch against the size limits.
class SizeChecker final : public WriteBatch::Handler {
 public:
  void Put(std::string_view key, std::string_view value) override {
    if (status_.ok()) {
      status_ = CheckKeySize(key);
    }
    if (status_.ok()) {
      status_ = CheckValueSize(value);
    }
  }
  void Delete(std::string_view key) override {
    if (status_.ok()) {
      status_ = CheckKeySize(key);
    }
  }
  [[nodiscard]] const Status& status() const noexcept { return status_; }

 private:
  Status status_;
};

}  // namespace

// --- Public interfaces (defined here so their vtables live in the library) ---

Iterator::~Iterator() = default;
DB::~DB() = default;

Result<std::unique_ptr<DB>> DB::Open(const Options& options, const std::filesystem::path& name) {
  if (name.empty()) {
    return Status::InvalidArgument("database name is empty");
  }
  return std::unique_ptr<DB>(std::make_unique<DBImpl>(options));
}

Status DB::Put(const WriteOptions& options, std::string_view key, std::string_view value) {
  // Check before building the batch: WriteBatch::Put aborts on sizes it
  // cannot encode, and the caller should get a Status instead.
  FORGEDB_RETURN_IF_ERROR(CheckKeySize(key));
  FORGEDB_RETURN_IF_ERROR(CheckValueSize(value));
  WriteBatch batch;
  batch.Put(key, value);
  return Write(options, batch);
}

Status DB::Delete(const WriteOptions& options, std::string_view key) {
  FORGEDB_RETURN_IF_ERROR(CheckKeySize(key));
  WriteBatch batch;
  batch.Delete(key);
  return Write(options, batch);
}

// --- DBImpl ------------------------------------------------------------------

DBImpl::DBImpl(const Options& /*options*/) : mem_(MemTable::Create()) {}

Status DBImpl::Write(const WriteOptions& /*options*/, const WriteBatch& batch) {
  // Validation needs no lock: the batch is the caller's, the limits are fixed.
  SizeChecker sizes;
  FORGEDB_RETURN_IF_ERROR(batch.Iterate(sizes));
  FORGEDB_RETURN_IF_ERROR(sizes.status());
  const std::uint32_t count = batch.Count();
  if (count == 0) {
    return Status::OK();
  }

  // The DB numbers the batch; the caller's batch stays unchanged.
  WriteBatch numbered = batch;
  const std::scoped_lock lock(mutex_);
  WriteBatchInternal::SetSequence(numbered, last_sequence_ + 1);
  // InsertInto checks the sequence range (an error, not a crash, if the 2^56
  // sequence numbers ever run out) and applies all records or none.
  FORGEDB_RETURN_IF_ERROR(WriteBatchInternal::InsertInto(numbered, *mem_));
  // Publish only now: readers take last_sequence_ as their read point, so
  // until this line none of the batch is visible, and after it all of it is.
  last_sequence_ += count;
  return Status::OK();
}

DBImpl::ReadView DBImpl::CurrentReadView() {
  const std::scoped_lock lock(mutex_);
  return {.memtable = mem_, .sequence = last_sequence_};
}

Result<std::string> DBImpl::Get(const ReadOptions& /*options*/, std::string_view key) {
  FORGEDB_RETURN_IF_ERROR(CheckKeySize(key));
  const ReadView view = CurrentReadView();
  std::string value;
  switch (view.memtable->Get(LookupKey(key, view.sequence), &value)) {
    case MemTable::GetResult::kFound:
      return value;
    case MemTable::GetResult::kDeleted:
    case MemTable::GetResult::kNotFound:
      break;
  }
  return Status::NotFound("key not found");
}

std::unique_ptr<Iterator> DBImpl::NewIterator(const ReadOptions& /*options*/) {
  ReadView view = CurrentReadView();
  return NewDBIterator(std::move(view.memtable), view.sequence);
}

}  // namespace forgedb
