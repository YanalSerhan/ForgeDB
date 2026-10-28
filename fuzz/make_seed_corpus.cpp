// Writes the fuzzers' seed corpus (TODO 2.7), so no binary files live in git:
//
//   forgedb_fuzz_seeds <dir>
//
// creates, under <dir>:
//   log_reader/     real logs written by log::Writer, intact and damaged, plus
//                   round-trip descriptions (see fuzz_log_reader.cpp)
//   write_batch/    encoded write batches
//   block/          blocks written by BlockBuilder, plus entry descriptions
//   table/          tables written by TableBuilder, plus table descriptions
//   version_edit/   encoded version edits, plus MANIFEST record lists
//   <target>-work/  empty: libFuzzer adds new inputs here
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
#include "db/version_edit.h"
#include "db/write_batch_internal.h"
#include "env/mem_file_system.h"
#include "log/log_format.h"
#include "log/log_writer.h"
#include "table/block_builder.h"
#include "table/table_builder.h"

namespace {

using forgedb::WriteBatch;
using forgedb::WriteBatchInternal;
using std::string_view_literals::operator""sv;

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

std::string InternalKey(std::string_view user_key, std::uint64_t sequence, bool deletion = false) {
  std::string key;
  forgedb::AppendInternalKey(
      key, {.user_key = user_key,
            .sequence = sequence,
            .type = deletion ? forgedb::ValueType::kDeletion : forgedb::ValueType::kValue});
  return key;
}

// A block of `count` internal keys, as the table builder writes data blocks.
std::string BlockOf(std::size_t count, std::size_t restart_interval) {
  forgedb::BlockBuilder builder(&forgedb::CompareInternalKeys, restart_interval);
  for (std::size_t i = 0; i < count; ++i) {
    builder.Add(InternalKey("key" + std::to_string(100 + i), 1000 - i, i % 4 == 3),
                "value" + std::to_string(i));
  }
  return std::string(builder.Finish());
}

// A table file of `count` entries.
std::string TableOf(std::size_t count, std::size_t block_size, int filter_bits_per_key) {
  forgedb::MemFileSystem fs;
  const std::unique_ptr<forgedb::WritableFile> file = std::move(fs.NewWritableFile("t")).value();
  forgedb::TableBuilder builder(
      {.block_size = block_size, .filter_bits_per_key = filter_bits_per_key}, file.get());
  for (std::size_t i = 0; i < count; ++i) {
    builder.Add(InternalKey("key" + std::to_string(100 + i), 1000 - i, i % 4 == 3),
                std::string(i % 50, 'v'));
  }
  if (!builder.Finish().ok() || !file->Close().ok()) {
    return {};
  }
  std::string contents(builder.FileSize(), '\0');
  const std::unique_ptr<forgedb::RandomAccessFile> read =
      std::move(fs.NewRandomAccessFile("t")).value();
  contents.resize(std::move(read->Read(0, std::span<char>(contents))).value());
  return contents;
}

std::string Encoded(const forgedb::VersionEdit& edit) {
  std::string encoded;
  edit.EncodeTo(encoded);
  return encoded;
}

// A length byte, then the record (fuzz_version_edit's MANIFEST mode).
std::string Record(std::string_view record) {
  return std::string(1, static_cast<char>(record.size())) + std::string(record);
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
       {"log_reader", "log_reader-work", "write_batch", "write_batch-work", "block", "block-work",
        "table", "table-work", "version_edit", "version_edit-work", "artifacts"}) {
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

  // Blocks (mode 0x00): restart intervals 1, 16 and the whole block; an
  // empty block. Entry descriptions (mode 0x01): interval byte, then
  // key-length, key, value-length, value.
  const std::vector<std::string> blocks = {
      std::string(1, '\0') + BlockOf(20, 1),
      std::string(1, '\0') + BlockOf(40, 16),
      std::string(1, '\0') + BlockOf(10, 100),
      std::string(1, '\0') + BlockOf(0, 16),
      std::string("\x01\x03\x01"
                  "a\x01"
                  "1\x02"
                  "ab\x01"
                  "2\x03"
                  "abc\x00"sv),
  };
  n = 0;
  for (const std::string& block : blocks) {
    failures += WriteSeed(root / "block" / ("seed-" + std::to_string(n++)), block) ? 0 : 1;
  }

  // Tables (mode 0x00): small blocks, default blocks, no filter, empty.
  // Descriptions (mode 0x01): settings, damage (none, a flipped byte, a
  // truncation), then entries.
  const std::string entries(
      "\x01"
      "a\x00\x01"
      "1"
      "\x01"
      "b\x01\x00"
      "\x01"
      "a\x00\x02"
      "22"
      "\x02"
      "cc\x00\x03"
      "333"sv);
  const std::vector<std::string> tables = {
      std::string(1, '\0') + TableOf(50, 64, 10),
      std::string(1, '\0') + TableOf(200, 4096, 10),
      std::string(1, '\0') + TableOf(30, 256, 0),
      std::string(1, '\0') + TableOf(0, 4096, 10),
      std::string("\x01\x01\xFF\xFF\x00"sv) + entries,  // 64-byte blocks, no damage
      std::string("\x01\x05\x40\x00\x10"sv) + entries,  // a flipped byte a quarter in
      std::string("\x01\x83\xC0\x00\x00"sv) + entries,  // no filter, cut at three quarters
  };
  n = 0;
  for (const std::string& table : tables) {
    failures += WriteSeed(root / "table" / ("seed-" + std::to_string(n++)), table) ? 0 : 1;
  }

  // Version edits (mode 0x00) and MANIFESTs (mode 0x01: a snapshot that adds
  // two small tables, then edits).
  forgedb::VersionEdit snapshot;
  snapshot.log_number = 5;
  snapshot.next_file_number = 9;
  snapshot.last_sequence = 100;
  snapshot.added_files.push_back({.level = 0,
                                  .file = {.number = 3,
                                           .file_size = 10,
                                           .smallest = InternalKey("a", 1),
                                           .largest = InternalKey("m", 2)}});
  snapshot.added_files.push_back({.level = 0,
                                  .file = {.number = 4,
                                           .file_size = 20,
                                           .smallest = InternalKey("c", 3),
                                           .largest = InternalKey("z", 4)}});
  forgedb::VersionEdit flush;
  flush.log_number = 7;
  flush.last_sequence = 150;
  flush.added_files.push_back({.level = 0,
                               .file = {.number = 6,
                                        .file_size = 30,
                                        .smallest = InternalKey("b", 101),
                                        .largest = InternalKey("y", 150)}});
  forgedb::VersionEdit removal;
  removal.removed_files.push_back({.level = 0, .number = 3});
  const std::vector<std::string> edits = {
      std::string(1, '\0') + Encoded(snapshot),
      std::string(1, '\0') + Encoded(flush),
      std::string(1, '\0') + Encoded(removal),
      std::string("\x01", 1) + Record(Encoded(snapshot)),
      std::string("\x01", 1) + Record(Encoded(snapshot)) + Record(Encoded(flush)) +
          Record(Encoded(removal)),
  };
  n = 0;
  for (const std::string& edit : edits) {
    failures += WriteSeed(root / "version_edit" / ("seed-" + std::to_string(n++)), edit) ? 0 : 1;
  }

  if (failures != 0) {
    const std::string message = std::to_string(failures) + " seed files could not be written\n";
    std::fputs(message.c_str(), stderr);
    return 1;
  }
  const std::string message = "seed corpus written to " + root.string() + "\n";
  std::fputs(message.c_str(), stdout);
  return 0;
}
