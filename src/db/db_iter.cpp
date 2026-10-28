#include "db/db_iter.h"

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

#include "forgedb/check.h"

namespace forgedb {
namespace {

class DBIter final : public Iterator {
 public:
  DBIter(std::unique_ptr<Iterator> entries, SequenceNumber sequence,
         std::shared_ptr<const void> pin) noexcept
      : pin_(std::move(pin)), entries_(std::move(entries)), sequence_(sequence) {}

  [[nodiscard]] bool Valid() const override { return valid_; }

  void SeekToFirst() override {
    entries_->SeekToFirst();
    FindNextUserEntry(/*skipping=*/false);
  }

  void Seek(std::string_view target) override {
    // (target, sequence_) sorts before every version of `target` visible at
    // sequence_ and after every newer one, so the scan starts at the newest
    // visible version of the first user key >= target.
    seek_key_.clear();
    AppendInternalKey(seek_key_,
                      {.user_key = target, .sequence = sequence_, .type = kValueTypeForSeek});
    entries_->Seek(seek_key_);
    FindNextUserEntry(/*skipping=*/false);
  }

  void Next() override {
    FORGEDB_DCHECK(valid_);
    // Skip the older versions of the key we are on, then find the next key.
    skip_key_.assign(key_);
    entries_->Next();
    FindNextUserEntry(/*skipping=*/true);
  }

  [[nodiscard]] std::string_view key() const override {
    FORGEDB_DCHECK(valid_);
    return key_;
  }
  [[nodiscard]] std::string_view value() const override {
    FORGEDB_DCHECK(valid_);
    return value_;
  }
  // OK, the first malformed entry, or the entries' error (a damaged table).
  [[nodiscard]] Status status() const override {
    return status_.ok() ? entries_->status() : status_;
  }

 private:
  // Advances `entries_` to the newest visible value of a user key that is not
  // hidden, and makes it current; or becomes !Valid() at the end. If
  // `skipping`, entries of skip_key_ are hidden (already returned or deleted).
  void FindNextUserEntry(bool skipping);

  // Moves `entries_` past the run of entries the scan is skipping, by seeking
  // instead of stepping. `user_key` is the current entry's user key.
  void SkipAheadBySeeking(bool skipping, std::string_view user_key, SequenceNumber entry_sequence);

  std::shared_ptr<const void> pin_;    // keeps what the entries read from alive (the Version)
  std::unique_ptr<Iterator> entries_;  // internal keys, in order
  SequenceNumber sequence_;

  bool valid_ = false;
  Status status_;  // a malformed entry; sticky
  // Views into the entry entries_ is positioned at; valid until it moves.
  std::string_view key_;
  std::string_view value_;
  // A copy: the scan moves on while still skipping this key's versions.
  std::string skip_key_;
  std::string seek_key_;  // scratch buffer for building seek targets
};

void DBIter::FindNextUserEntry(bool skipping) {
  valid_ = false;
  if (!status_.ok()) {
    return;
  }
  std::size_t skipped = 0;
  while (entries_->Valid()) {
    // Table entries passed a CRC, but parse them as untrusted anyway.
    const std::optional<ParsedInternalKey> parsed = ParseInternalKey(entries_->key());
    if (!parsed) {
      status_ = Status::Corruption("malformed internal key in the database");
      return;
    }
    const std::string_view user_key = parsed->user_key;
    const SequenceNumber entry_sequence = parsed->sequence;
    const ValueType type = parsed->type;

    if (entry_sequence <= sequence_) {
      if (skipping && user_key == skip_key_) {
        // An older version of a key already returned or deleted: skip.
      } else if (type == ValueType::kDeletion) {
        // The newest visible version is a deletion: hide this key entirely.
        skip_key_.assign(user_key);
        skipping = true;
      } else {
        // The newest visible version is a value: that's the next user key.
        valid_ = true;
        key_ = user_key;
        value_ = entries_->value();
        return;
      }
    }
    // Else: written after this iterator's snapshot, so invisible: skip.

    if (++skipped > kMaxSequentialSkip) {
      SkipAheadBySeeking(skipping, user_key, entry_sequence);
      skipped = 0;
    } else {
      entries_->Next();
    }
  }
}

void DBIter::SkipAheadBySeeking(bool skipping, std::string_view user_key,
                                SequenceNumber entry_sequence) {
  seek_key_.clear();
  if (skipping && user_key == skip_key_) {
    if (entry_sequence == 0) {
      // The target below would be this very entry; the DB never writes
      // sequence 0, but stepping keeps the loop finite regardless.
      entries_->Next();
      return;
    }
    // Past every version of skip_key_: tag 0 (sequence 0, deletion) is the
    // smallest tag, so it sorts after all of them.
    AppendInternalKey(seek_key_,
                      {.user_key = skip_key_, .sequence = 0, .type = ValueType::kDeletion});
  } else {
    // Past the invisible (too new) versions of user_key, to its newest
    // visible one, as Seek() does.
    FORGEDB_DCHECK(entry_sequence > sequence_);
    static_cast<void>(entry_sequence);
    AppendInternalKey(seek_key_,
                      {.user_key = user_key, .sequence = sequence_, .type = kValueTypeForSeek});
  }
  // Both targets sort strictly after the current entry (whose sequence is
  // > 0 in the first case, > sequence_ in the second), so this always
  // makes progress. (user_key may point into the entry; it is copied into
  // seek_key_ before the move.)
  entries_->Seek(seek_key_);
}

}  // namespace

std::unique_ptr<Iterator> NewDBIterator(std::unique_ptr<Iterator> entries, SequenceNumber sequence,
                                        std::shared_ptr<const void> pin) {
  return std::make_unique<DBIter>(std::move(entries), sequence, std::move(pin));
}

}  // namespace forgedb
