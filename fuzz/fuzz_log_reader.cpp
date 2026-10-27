// Fuzz target (TODO 2.7): the write-ahead log reader on untrusted bytes.
//
// The first input byte picks a mode:
//
//   even  The rest is a log file as found on disk. The reader must not crash,
//         read out of bounds (ASan) or loop forever. Every call returns a
//         record, the end of the log, or Corruption; the end and errors are
//         sticky; records never add up to more bytes than the file. Each
//         record is also decoded as a write batch, as recovery would.
//
//   odd   The rest describes records (see ParseRecords). They are written with
//         log::Writer, the file is cut at a chosen offset (possibly not at
//         all), and read back. The reader must return exactly the records that
//         were complete before the cut, then end cleanly: a cut is always a
//         torn tail (ADR 0004), never Corruption and never different data.
//
// A broken promise aborts (Expect), which libFuzzer reports as a crash and
// saves the input that caused it.
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <optional>
#include <source_location>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "forgedb/write_batch.h"

#include "db/write_batch_internal.h"
#include "env/mem_file_system.h"
#include "log/log_reader.h"
#include "log/log_writer.h"

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

using forgedb::MemFileSystem;
using forgedb::Status;

constexpr std::size_t kMaxRecords = 64;
constexpr std::size_t kMaxTotalBytes = std::size_t{1} << 20;

void WriteFile(MemFileSystem& fs, const char* name, std::string_view contents) {
  const std::unique_ptr<forgedb::WritableFile> file = std::move(fs.NewWritableFile(name)).value();
  Expect(file->Append(contents).ok());
  Expect(file->Close().ok());
}

class CountingHandler final : public forgedb::WriteBatch::Handler {
 public:
  void Put(std::string_view /*key*/, std::string_view /*value*/) override { ++operations; }
  void Delete(std::string_view /*key*/) override { ++operations; }
  std::uint64_t operations = 0;  // NOLINT(misc-non-private-member-variables-in-classes)
};

// What recovery does with a record: decode it as a write batch.
void DecodeAsBatch(std::string_view record) {
  forgedb::WriteBatch batch;
  if (!forgedb::WriteBatchInternal::SetContents(batch, record).ok()) {
    return;
  }
  CountingHandler handler;
  if (batch.Iterate(handler).ok()) {
    Expect(handler.operations == batch.Count());
  } else {
    Expect(handler.operations == 0);  // all or nothing
  }
}

void ReadRawLog(std::string_view file) {
  MemFileSystem fs;
  WriteFile(fs, "log", file);
  forgedb::log::Reader reader(std::move(fs.NewSequentialFile("log")).value());
  std::size_t records = 0;
  std::size_t total = 0;
  while (true) {
    const forgedb::Result<std::optional<std::string_view>> record = reader.ReadRecord();
    if (!record.ok()) {
      const Status error = record.status();
      Expect(error.code() == Status::Code::kCorruption);
      const forgedb::Result<std::optional<std::string_view>> again = reader.ReadRecord();
      Expect(!again.ok() && again.status() == error);  // sticky
      return;
    }
    if (!record->has_value()) {
      const forgedb::Result<std::optional<std::string_view>> again = reader.ReadRecord();
      Expect(again.ok() && !again->has_value());  // the end is sticky
      return;
    }
    const std::string_view payload = **record;
    total += payload.size();
    ++records;
    // Every record costs at least one 7-byte fragment header.
    Expect(total <= file.size());
    Expect(records * 7 <= file.size());
    DecodeAsBatch(payload);
  }
}

// The odd mode's input: two bytes choose the cut (0xFFFF: no cut), then each
// record is one header byte h followed by its bytes:
//   h < 0x80   a record of the next h input bytes (fewer if the input ends)
//   h >= 0x80  a record of ((h & 0x7F) + 1) * 1021 copies of the next byte,
//              up to 130 KiB, so records cross 32 KiB blocks
struct Plan {
  std::uint16_t cut = 0xFFFF;
  std::vector<std::string> records;
};

Plan ParseRecords(std::string_view input) {
  Plan plan;
  if (input.size() >= 2) {
    plan.cut = static_cast<std::uint16_t>((static_cast<unsigned char>(input[0]) << 8U) |
                                          static_cast<unsigned char>(input[1]));
    input.remove_prefix(2);
  }
  std::size_t total = 0;
  while (!input.empty() && plan.records.size() < kMaxRecords) {
    const auto header = static_cast<unsigned char>(input.front());
    input.remove_prefix(1);
    std::string record;
    if (header < 0x80) {
      const std::size_t size = std::min<std::size_t>(header, input.size());
      record.assign(input.substr(0, size));
      input.remove_prefix(size);
    } else {
      const char fill = input.empty() ? 'x' : input.front();
      if (!input.empty()) {
        input.remove_prefix(1);
      }
      record.assign(static_cast<std::size_t>((header & 0x7FU) + 1U) * 1021, fill);
    }
    total += record.size();
    if (total > kMaxTotalBytes) {
      break;
    }
    plan.records.push_back(std::move(record));
  }
  return plan;
}

void RoundTrip(std::string_view input) {
  const Plan plan = ParseRecords(input);
  MemFileSystem fs;
  std::vector<std::uint64_t> ends;  // file size after each record
  {
    const std::unique_ptr<forgedb::WritableFile> file =
        std::move(fs.NewWritableFile("log")).value();
    forgedb::log::Writer writer(file.get());
    for (const std::string& record : plan.records) {
      Expect(writer.AddRecord(record).ok());
      ends.push_back(std::move(fs.GetFileSize("log")).value());
    }
    Expect(file->Close().ok());
  }
  std::string written;
  {
    const std::unique_ptr<forgedb::SequentialFile> file =
        std::move(fs.NewSequentialFile("log")).value();
    std::string buffer(65536, '\0');
    while (true) {
      const std::size_t read = std::move(file->Read(buffer)).value();
      if (read == 0) {
        break;
      }
      written.append(buffer.data(), read);
    }
  }
  const std::size_t cut = plan.cut == 0xFFFF
                              ? written.size()
                              : static_cast<std::size_t>(written.size() * plan.cut / 0xFFFFU);
  WriteFile(fs, "cut", std::string_view(written).substr(0, cut));

  forgedb::log::Reader reader(std::move(fs.NewSequentialFile("cut")).value());
  std::size_t expected = 0;
  while (expected < ends.size() && ends[expected] <= cut) {
    ++expected;
  }
  for (std::size_t i = 0; i < expected; ++i) {
    const forgedb::Result<std::optional<std::string_view>> record = reader.ReadRecord();
    Expect(record.ok() && record->has_value() && **record == plan.records[i]);
  }
  // Then the end: a cut is a torn tail, never Corruption, and nothing follows
  // the complete records.
  const forgedb::Result<std::optional<std::string_view>> end = reader.ReadRecord();
  Expect(end.ok() && !end->has_value());
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
    ReadRawLog(rest);
  } else {
    RoundTrip(rest);
  }
  return 0;
}
