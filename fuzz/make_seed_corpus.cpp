// Writes the fuzzers' seed corpus (TODO 2.7), so no binary files live in git:
//
//   forgedb_fuzz_seeds <dir>
//
// creates, under <dir>:
//   log_reader/     real logs written by log::Writer, intact and damaged, plus
//                   round-trip descriptions (see fuzz_log_reader.cpp)
//   write_batch/    encoded write batches
//   log_reader-work/, write_batch-work/   empty: libFuzzer adds new inputs here
//   artifacts/      empty: crashing inputs go here
// Existing files are overwritten; nothing else is touched.
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

#include "forgedb/write_batch.h"

#include "db/internal_key.h"
#include "db/write_batch_internal.h"
#include "env/mem_file_system.h"
#include "log/log_format.h"
#include "log/log_writer.h"

namespace {

using forgedb::WriteBatch;
using forgedb::WriteBatchInternal;

bool WriteSeed(const std::filesystem::path& path, std::string_view contents) {
  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  out.write(contents.data(), static_cast<std::streamsize>(contents.size()));
  return static_cast<bool>(out);
}

WriteBatch Batch(std::uint64_t sequence, std::size_t puts, std::size_t value_size) {
  WriteBatch batch;
  for (std::size_t i = 0; i < puts; ++i) {
    batch.Put("key" + std::to_string(i),
              std::string(value_size, static_cast<char>('a' + (i % 26))));
  }
  batch.Delete("gone");
  WriteBatchInternal::SetSequence(batch, sequence);
  return batch;
}

// A log file holding `records`, as log::Writer writes it.
std::string LogOf(const std::vector<std::string>& records) {
  forgedb::MemFileSystem fs;
  {
    const std::unique_ptr<forgedb::WritableFile> file =
        std::move(fs.NewWritableFile("log")).value();
    forgedb::log::Writer writer(file.get());
    for (const std::string& record : records) {
      if (!writer.AddRecord(record).ok()) {
        return {};
      }
    }
    static_cast<void>(file->Close());
  }
  const std::unique_ptr<forgedb::SequentialFile> file =
      std::move(fs.NewSequentialFile("log")).value();
  std::string contents;
  std::string buffer(65536, '\0');
  while (true) {
    const std::size_t read = std::move(file->Read(buffer)).value();
    if (read == 0) {
      return contents;
    }
    contents.append(buffer.data(), read);
  }
}

std::string Contents(const WriteBatch& batch) {
  return std::string(WriteBatchInternal::Contents(batch));
}

}  // namespace

int main(int argc, char** argv) {
  const std::span<char*> args(argv, static_cast<std::size_t>(argc));
  if (args.size() != 2) {
    std::fputs("usage: forgedb_fuzz_seeds <dir>\n", stderr);
    return 2;
  }
  const std::filesystem::path root(args[1]);
  std::error_code error;
  for (const char* sub :
       {"log_reader", "log_reader-work", "write_batch", "write_batch-work", "artifacts"}) {
    std::filesystem::create_directories(root / sub, error);
    if (error) {
      const std::string message =
          "cannot create " + (root / sub).string() + ": " + error.message() + "\n";
      std::fputs(message.c_str(), stderr);
      return 1;
    }
  }

  // Write batches: empty, small, mixed, large, near the sequence limit.
  const std::vector<std::string> batches = {
      Contents(Batch(1, 0, 0)),
      Contents(Batch(1, 1, 5)),
      Contents(Batch(42, 5, 30)),
      Contents(Batch(7, 2, 5000)),
      Contents(Batch(forgedb::kMaxSequenceNumber - 1, 3, 1)),
  };

  // Logs (mode byte 0x00): intact, multi-block, empty records, torn, damaged.
  const std::string small_log = LogOf({batches[1], batches[2]});
  const std::string big_log = LogOf({batches[3], std::string(70000, 'z'), batches[2]});
  std::string damaged = big_log;
  damaged[damaged.size() / 2] = static_cast<char>(damaged[damaged.size() / 2] ^ 0x40);
  // A record ending 5 bytes before the end of a block, so the next one pads.
  const std::string padded_log =
      LogOf({std::string(forgedb::log::kBlockSize - forgedb::log::kHeaderSize - 5, 'p'), "after"});
  const std::vector<std::string> logs = {
      small_log,
      big_log,
      LogOf({"", "", batches[0]}),
      small_log.substr(0, small_log.size() - 3),  // torn tail
      damaged,                                    // mid-log corruption
      padded_log,
  };

  int failures = 0;
  int n = 0;
  for (const std::string& batch : batches) {
    failures += WriteSeed(root / "write_batch" / ("seed-" + std::to_string(n++)), batch) ? 0 : 1;
  }
  n = 0;
  for (const std::string& log : logs) {
    failures +=
        WriteSeed(root / "log_reader" / ("seed-" + std::to_string(n++)), std::string(1, '\0') + log)
            ? 0
            : 1;
  }
  // Round-trip descriptions (mode byte 0x01): no cut, then a cut halfway.
  // Records: "abc", 6 x 1021 'z' (crosses no block), 128 x 1021 'q' (several
  // blocks), an empty record.
  const std::string description = std::string(
      "\x03"
      "abc"
      "\x85"
      "z"
      "\xFF"
      "q"
      "\x00",
      9);
  failures += WriteSeed(root / "log_reader" / "roundtrip-whole",
                        std::string("\x01\xFF\xFF", 3) + description)
                  ? 0
                  : 1;
  failures +=
      WriteSeed(root / "log_reader" / "roundtrip-cut", std::string("\x01\x80\x00", 3) + description)
          ? 0
          : 1;
  if (failures != 0) {
    const std::string message = std::to_string(failures) + " seed files could not be written\n";
    std::fputs(message.c_str(), stderr);
    return 1;
  }
  const std::string message = "seed corpus written to " + root.string() + "\n";
  std::fputs(message.c_str(), stdout);
  return 0;
}
