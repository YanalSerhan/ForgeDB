// log::Reader: reads back the records of a write-ahead log, for recovery.
//
//   log::Reader reader(std::move(file));
//   while (true) {
//     FORGEDB_ASSIGN_OR_RETURN(std::optional<std::string_view> record, reader.ReadRecord());
//     if (!record) break;  // end of the log
//     Replay(*record);
//   }
//
// The file's bytes are untrusted: every fragment's header, length, type and
// CRC is checked, and nothing is read outside the current block.
//
// Damage is either a torn tail or mid-log corruption, decided by one rule:
//
//   a problem is a torn tail if and only if no valid fragment exists
//   anywhere after it in the file.
//
// A crash can only damage the end of a log: the record being written when the
// power failed, and anything after the last Sync. That leaves an incomplete
// header or payload, a chain of fragments that never finishes, garbage, or
// zeros (a file extended but never written). The reader ends the log there,
// silently dropping the incomplete record (its write was never acknowledged
// as synced), and reports the cut through IgnoredTail().
//
// Damage followed by valid data cannot come from a crash in this model, so it
// is reported as Corruption with the offset and reason, and recovery stops:
// skipping it could silently lose synced writes. (The strict policy, ADR 0004.
// A lenient recovery mode could be added later on top of the same
// classification.)
#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

#include "forgedb/result.h"
#include "forgedb/status.h"

#include "env/file_system.h"
#include "log/log_format.h"

namespace forgedb::log {

class Reader {
 public:
  // Where a torn tail began and what was wrong there.
  struct TornTail {
    std::uint64_t offset = 0;
    std::string reason;
  };

  explicit Reader(std::unique_ptr<SequentialFile> file);

  // The next record, or nullopt at the end of the log (including after an
  // ignored torn tail). The view is valid until the next call. Corruption for
  // mid-log damage; the file's error if reading fails. Errors are sticky:
  // later calls return the same status.
  [[nodiscard]] Result<std::optional<std::string_view>> ReadRecord();

  // File offset of the first fragment of the record last returned.
  [[nodiscard]] std::uint64_t LastRecordOffset() const noexcept { return last_record_offset_; }

  // Set once the reader has ended the log at a torn tail.
  [[nodiscard]] const std::optional<TornTail>& IgnoredTail() const noexcept {
    return ignored_tail_;
  }

 private:
  struct Fragment {
    RecordType type = RecordType::kZero;
    std::string_view payload;
    std::uint64_t offset = 0;
  };
  // What ReadFragment found.
  enum class Next : std::uint8_t {
    kFragment,  // a valid fragment, in *fragment
    kEnd,       // a clean end of file
    kProblem,   // damage at problem_offset_, described by problem_reason_
  };

  [[nodiscard]] Result<Next> ReadFragment(Fragment* fragment);
  // Reads the next block into block_; sets eof_ on a short read.
  [[nodiscard]] Status ReadBlock();
  // Decides whether the problem recorded in problem_offset_/problem_reason_
  // is a torn tail (returns nullopt, sets ignored_tail_ to start at
  // `tail_offset`) or mid-log corruption (returns the Corruption status).
  // Consumes the rest of the file.
  [[nodiscard]] Result<std::optional<std::string_view>> ClassifyProblem(std::uint64_t tail_offset);
  // A finished log: every later ReadRecord returns `status` (OK = end of log).
  Result<std::optional<std::string_view>> Finish(Status status);

  std::unique_ptr<SequentialFile> file_;
  std::string block_;                    // the current block (up to kBlockSize bytes)
  std::size_t block_position_ = 0;       // next unread byte in block_
  std::uint64_t block_file_offset_ = 0;  // file offset of block_[0]
  std::uint64_t next_block_offset_ = 0;  // file offset of the next block to read
  bool eof_ = false;                     // the file ended inside or at the end of block_

  std::string record_;  // a multi-fragment record being reassembled
  std::uint64_t last_record_offset_ = 0;

  std::uint64_t problem_offset_ = 0;
  std::string problem_reason_;

  bool finished_ = false;
  Status final_status_;  // what a finished reader returns: OK (end) or the error
  std::optional<TornTail> ignored_tail_;
};

}  // namespace forgedb::log
