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

// Whether some prefix of `payload` shorter than `limit` bytes matches the
// header's CRC, i.e. the fragment's real length is shorter than its length
// field says. A torn fragment's CRC covers its whole, never-finished payload,
// so no prefix matches it (except by a 1 in 2^32 coincidence per length).
bool ShorterPrefixMatchesCrc(const Header& header, std::string_view payload,
                             std::size_t limit) noexcept {
  const std::uint32_t want = crc32c::Unmask(header.masked_crc);
  const auto type = static_cast<char>(header.type);
  std::uint32_t crc = crc32c::Value(std::string_view(&type, 1));
  for (std::size_t length = 0; length < limit && length <= payload.size(); ++length) {
    if (crc == want) {
      return true;
    }
    if (length < payload.size()) {
      crc = crc32c::Extend(crc, payload.substr(length, 1));
    }
  }
  return false;
}

std::string OffsetText(std::uint64_t offset) { return "offset " + std::to_string(offset); }

}  // namespace

Reader::Reader(std::unique_ptr<SequentialFile> file, Mode mode)
    : file_(std::move(file)), mode_(mode) {}

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
      problem_search_from_ = block_position_ + 1;
      problem_length_damaged_ = false;
      problem_reason_ = "the file ends inside a fragment header";
      return Next::kProblem;
    }

    const char* const start = block_.data() + block_position_;
    const Header header = DecodeHeader(std::string_view(start, remaining));
    problem_offset_ = block_file_offset_ + block_position_;
    problem_search_from_ = block_position_ + 1;
    problem_length_damaged_ = false;
    const bool known_type = header.type != 0 && header.type <= kMaxRecordType;
    if (header.length > remaining - kHeaderSize) {
      problem_reason_ = eof_ ? "the file ends inside a fragment"
                             : "fragment length runs past the end of its block";
      if (eof_ && known_type && header.length <= kBlockSize - block_position_ - kHeaderSize) {
        // An intact header whose payload the end of the file cut off: what a
        // crash leaves. Its payload is not searched (user data), unless the
        // CRC shows the length field itself is damaged.
        const std::string_view payload(start + kHeaderSize, remaining - kHeaderSize);
        problem_length_damaged_ = ShorterPrefixMatchesCrc(header, payload, payload.size());
        problem_search_from_ = block_.size();
      }
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
      // Damage inside the fragment (or a torn write at its end). Its payload
      // is not searched (user data), unless the length field is damaged.
      const std::string_view payload(start + kHeaderSize, header.length);
      problem_length_damaged_ = ShorterPrefixMatchesCrc(header, payload, header.length);
      problem_search_from_ = block_position_ + kHeaderSize + header.length;
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
          return Damaged(Status::Corruption("log record without its last fragment",
                                            "record at " + OffsetText(record_start) +
                                                ", next record at " + OffsetText(fragment.offset)),
                         record_start);
        }
        last_record_offset_ = fragment.offset;
        return std::optional<std::string_view>(fragment.payload);
      case RecordType::kFirst:
        if (in_record) {
          return Damaged(Status::Corruption("log record without its last fragment",
                                            "record at " + OffsetText(record_start) +
                                                ", next record at " + OffsetText(fragment.offset)),
                         record_start);
        }
        in_record = true;
        record_start = fragment.offset;
        record_.assign(fragment.payload);
        break;
      case RecordType::kMiddle:
      case RecordType::kLast:
        if (!in_record) {
          return Damaged(Status::Corruption("log fragment without the start of its record",
                                            OffsetText(fragment.offset)),
                         fragment.offset);
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
  // When the damaged fragment's header is intact, the search starts after
  // its payload: the payload is user data and may contain bytes that form a
  // valid fragment (a log file stored as a value). Searching it made a torn
  // record read as corruption (found by fuzzing, Phase 2.7; ADR 0004).
  // Whether its length field is damaged is decided by its CRC instead.
  std::uint64_t found_at = 0;
  bool found = false;
  int byte_scanned_blocks = 0;
  if (problem_length_damaged_) {
    return Damaged(Status::Corruption("log corrupted: fragment length field damaged",
                                      "at " + OffsetText(problem_offset_) +
                                          " (a shorter length matches its checksum)"),
                   tail_offset);
  }
  std::size_t from = problem_search_from_;
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
    return Damaged(Status::Corruption("log corrupted: " + problem_reason_,
                                      "at " + OffsetText(problem_offset_) +
                                          "; valid log data follows at " + OffsetText(found_at)),
                   tail_offset);
  }
  ignored_tail_ = TornTail{.offset = tail_offset, .reason = problem_reason_};
  return Finish(Status::OK());
}

Result<std::optional<std::string_view>> Reader::Damaged(Status corruption,
                                                        std::uint64_t tail_offset) {
  if (mode_ == Mode::kStrict) {
    return Finish(std::move(corruption));
  }
  ignored_tail_ =
      TornTail{.offset = tail_offset, .reason = corruption.message(), .valid_data_dropped = true};
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
