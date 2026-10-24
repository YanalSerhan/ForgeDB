#include "log/log_reader.h"

#include <cstddef>
#include <span>
#include <utility>

#include "forgedb/check.h"

#include "util/coding.h"
#include "util/crc32c.h"

namespace forgedb::log {
namespace {

struct Header {
  std::uint32_t masked_crc = 0;
  std::size_t length = 0;
  std::uint8_t type = 0;
};

// Precondition: bytes.size() >= kHeaderSize.
Header DecodeHeader(std::string_view bytes) noexcept {
  FORGEDB_DCHECK(bytes.size() >= kHeaderSize);
  std::string_view crc_bytes = bytes.substr(0, 4);
  return {.masked_crc = GetFixed32(crc_bytes).value_or(0),
          .length = static_cast<std::size_t>(static_cast<std::uint8_t>(bytes[4])) |
                    (static_cast<std::size_t>(static_cast<std::uint8_t>(bytes[5])) << 8U),
          .type = static_cast<std::uint8_t>(bytes[6])};
}

// Whether `header` carries the right CRC for `type_and_payload`: the header's
// type byte followed by its payload (header.length bytes).
bool CrcMatches(const Header& header, std::string_view type_and_payload) noexcept {
  std::uint32_t crc = crc32c::Value(type_and_payload.substr(0, 1));
  crc = crc32c::Extend(crc, type_and_payload.substr(1));
  return crc32c::Unmask(header.masked_crc) == crc;
}

// The position of the first valid fragment (known type, in bounds, correct
// CRC) that starts in block[from, end), or npos.
std::size_t FindValidFragment(std::string_view block, std::size_t from) noexcept {
  for (std::size_t p = from; p + kHeaderSize <= block.size(); ++p) {
    const Header header = DecodeHeader(block.substr(p));
    if (header.type == 0 || header.type > kMaxRecordType ||
        header.length > block.size() - p - kHeaderSize) {
      continue;  // cheap rejections first; most offsets stop here
    }
    if (CrcMatches(header, block.substr(p + kHeaderSize - 1, 1 + header.length))) {
      return p;
    }
  }
  return std::string_view::npos;
}

std::string OffsetText(std::uint64_t offset) { return "offset " + std::to_string(offset); }

}  // namespace

Reader::Reader(std::unique_ptr<SequentialFile> file) : file_(std::move(file)) {}

Status Reader::ReadBlock() {
  block_.resize(kBlockSize);
  const Result<std::size_t> read = file_->Read(std::span<char>(block_.data(), block_.size()));
  if (!read.ok()) {
    return read.status();
  }
  block_.resize(*read);
  block_position_ = 0;
  block_file_offset_ = next_block_offset_;
  next_block_offset_ += *read;
  eof_ = *read < kBlockSize;  // SequentialFile returns a short read only at the end
  return Status::OK();
}

Result<Reader::Next> Reader::ReadFragment(Fragment* fragment) {
  while (true) {
    const std::size_t remaining = block_.size() - block_position_;
    if (remaining < kHeaderSize) {
      if (!eof_) {
        // Zero padding at the end of a full block (or the very first call):
        // the next fragment starts in the next block.
        FORGEDB_RETURN_IF_ERROR(ReadBlock());
        continue;
      }
      if (remaining == 0) {
        return Next::kEnd;
      }
      problem_offset_ = block_file_offset_ + block_position_;
      problem_reason_ = "the file ends inside a fragment header";
      return Next::kProblem;
    }

    const char* const start = block_.data() + block_position_;
    const Header header = DecodeHeader(std::string_view(start, remaining));
    problem_offset_ = block_file_offset_ + block_position_;
    if (header.length > remaining - kHeaderSize) {
      problem_reason_ = eof_ ? "the file ends inside a fragment"
                             : "fragment length runs past the end of its block";
      return Next::kProblem;
    }
    if (header.type == static_cast<std::uint8_t>(RecordType::kZero)) {
      problem_reason_ = "zero-filled space where a fragment should start";
      return Next::kProblem;
    }
    if (header.type > kMaxRecordType) {
      problem_reason_ = "unknown fragment type " + std::to_string(header.type);
      return Next::kProblem;
    }
    if (!CrcMatches(header, std::string_view(start + kHeaderSize - 1, 1 + header.length))) {
      problem_reason_ = "fragment checksum mismatch";
      return Next::kProblem;
    }

    *fragment = {.type = static_cast<RecordType>(header.type),
                 .payload = std::string_view(start + kHeaderSize, header.length),
                 .offset = block_file_offset_ + block_position_};
    block_position_ += kHeaderSize + header.length;
    return Next::kFragment;
  }
}

Result<std::optional<std::string_view>> Reader::ReadRecord() {
  if (finished_) {
    return Finish(final_status_);
  }
  bool in_record = false;
  std::uint64_t record_start = 0;
  record_.clear();
  while (true) {
    Fragment fragment;
    const Result<Next> next = ReadFragment(&fragment);
    if (!next.ok()) {
      return Finish(next.status());  // the file itself failed to read
    }
    switch (*next) {
      case Next::kEnd:
        if (in_record) {
          // A crash while a multi-fragment record was being written.
          ignored_tail_ = TornTail{.offset = record_start,
                                   .reason = "the file ends before the record's last fragment"};
        }
        return Finish(Status::OK());
      case Next::kProblem:
        // An incomplete record is dropped with the tail, so the tail starts
        // where that record does.
        return ClassifyProblem(in_record ? record_start : problem_offset_);
      case Next::kFragment:
        break;
    }

    // A valid fragment in the wrong place means a record is missing a part
    // while valid data follows: by the rule, mid-log corruption.
    switch (fragment.type) {
      case RecordType::kFull:
        if (in_record) {
          return Finish(Status::Corruption("log record without its last fragment",
                                           "record at " + OffsetText(record_start) +
                                               ", next record at " + OffsetText(fragment.offset)));
        }
        last_record_offset_ = fragment.offset;
        return std::optional<std::string_view>(fragment.payload);
      case RecordType::kFirst:
        if (in_record) {
          return Finish(Status::Corruption("log record without its last fragment",
                                           "record at " + OffsetText(record_start) +
                                               ", next record at " + OffsetText(fragment.offset)));
        }
        in_record = true;
        record_start = fragment.offset;
        record_.assign(fragment.payload);
        break;
      case RecordType::kMiddle:
      case RecordType::kLast:
        if (!in_record) {
          return Finish(Status::Corruption("log fragment without the start of its record",
                                           OffsetText(fragment.offset)));
        }
        record_.append(fragment.payload);
        if (fragment.type == RecordType::kLast) {
          last_record_offset_ = record_start;
          return std::optional<std::string_view>(record_);
        }
        break;
      case RecordType::kZero:
        // ReadFragment reports zero-filled space as a problem, never as a fragment.
        return Finish(Status::Internal("log reader returned a zero fragment"));
    }
  }
}

Result<std::optional<std::string_view>> Reader::ClassifyProblem(std::uint64_t tail_offset) {
  // Look for any valid fragment after the problem. Byte by byte through the
  // rest of the problem's block and the next block (damage can spill over a
  // block boundary); after that only at block starts, because the Writer
  // starts every block with a fragment header. That keeps the cost linear in
  // the size of the damaged file instead of quadratic.
  //
  // Limitation: a torn record whose own payload happens to contain bytes that
  // form a valid fragment (e.g. a log file stored as a value) is reported as
  // corruption instead of a torn tail: a false alarm, never a silent loss.
  std::uint64_t found_at = 0;
  bool found = false;
  int byte_scanned_blocks = 0;
  std::size_t from = static_cast<std::size_t>(problem_offset_ - block_file_offset_) + 1;
  while (true) {
    const std::string_view block(block_);
    if (byte_scanned_blocks < 2) {
      const std::size_t position = FindValidFragment(block, from);
      if (position != std::string_view::npos) {
        found = true;
        found_at = block_file_offset_ + position;
        break;
      }
      ++byte_scanned_blocks;
    } else if (block.size() >= kHeaderSize) {
      const Header header = DecodeHeader(block);
      if (header.type != 0 && header.type <= kMaxRecordType &&
          header.length <= block.size() - kHeaderSize &&
          CrcMatches(header, block.substr(kHeaderSize - 1, 1 + header.length))) {
        found = true;
        found_at = block_file_offset_;
        break;
      }
    }
    if (eof_) {
      break;
    }
    if (Status read = ReadBlock(); !read.ok()) {
      return Finish(std::move(read));
    }
    from = 0;
  }

  if (found) {
    return Finish(Status::Corruption("log corrupted: " + problem_reason_,
                                     "at " + OffsetText(problem_offset_) +
                                         "; valid log data follows at " + OffsetText(found_at)));
  }
  ignored_tail_ = TornTail{.offset = tail_offset, .reason = problem_reason_};
  return Finish(Status::OK());
}

Result<std::optional<std::string_view>> Reader::Finish(Status status) {
  finished_ = true;
  final_status_ = status;
  if (status.ok()) {
    return std::optional<std::string_view>();
  }
  return status;
}

}  // namespace forgedb::log
