// Fuzz target (TODO 3.9): version edits and the MANIFEST, from untrusted bytes.
//
// The first input byte picks a mode:
//
//   even  The rest is one encoded VersionEdit. Decode must not crash; errors
//         are Corruption; a decoded edit encodes and decodes back to itself.
//
//   odd   The rest is split into records (a length byte, then that many
//         bytes) and written as a MANIFEST with log::Writer, so the records
//         pass their checksums and reach the decoder and VersionSet. A table
//         file of the recorded size is created for every file the records
//         add (up to 4 KiB). VersionSet::Recover must not crash and fails
//         only with Corruption (or NotSupported for file numbers from 2^63
//         on, which ForgeDB never writes). If it succeeds, the Version is consistent
//         (no file twice; level 0 newest first; new file numbers above every
//         file), and a new MANIFEST written from it recovers to the same
//         state.
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "forgedb/result.h"
#include "forgedb/status.h"

#include "db/filename.h"
#include "db/version_edit.h"
#include "db/version_set.h"
#include "env/file_system.h"
#include "env/mem_file_system.h"
#include "fuzz_expect.h"
#include "log/log_writer.h"

namespace {

using forgedb::FileMetaData;
using forgedb::Status;
using forgedb::Version;
using forgedb::VersionEdit;
using forgedb::VersionSet;
using forgedb::fuzz::Expect;

constexpr std::uint64_t kMaxTableBytes = 4096;
const std::filesystem::path kDb = "db";

bool IsCorruption(const Status& status) { return status.code() == Status::Code::kCorruption; }

void DecodeOne(std::string_view input) {
  const forgedb::Result<VersionEdit> edit = VersionEdit::Decode(input);
  if (!edit.ok()) {
    Expect(IsCorruption(edit.status()));
    return;
  }
  std::string encoded;
  edit->EncodeTo(encoded);
  const forgedb::Result<VersionEdit> again = VersionEdit::Decode(encoded);
  Expect(again.ok() && *again == *edit);
}

void WriteFile(forgedb::FileSystem& fs, const std::filesystem::path& path,
               std::string_view contents) {
  const std::unique_ptr<forgedb::WritableFile> file = std::move(fs.NewWritableFile(path)).value();
  Expect(file->Append(contents).ok());
  Expect(file->Close().ok());
}

// Everything a Version and its counters say, for comparing two recoveries.
struct State {
  std::vector<std::vector<FileMetaData>> files;
  std::uint64_t log_number = 0;
  std::uint64_t last_sequence = 0;
  friend bool operator==(const State&, const State&) = default;
};

State StateOf(const VersionSet& versions) {
  const Version& version = *versions.Current();
  return {.files = {version.files.begin(), version.files.end()},
          .log_number = versions.LogNumber(),
          .last_sequence = versions.LastSequence()};
}

void CheckConsistent(VersionSet& versions) {
  const Version& version = *versions.Current();
  std::vector<std::uint64_t> numbers;
  for (const std::vector<FileMetaData>& level : version.files) {
    for (const FileMetaData& file : level) {
      numbers.push_back(file.number);
    }
  }
  std::ranges::sort(numbers);
  Expect(std::ranges::adjacent_find(numbers) == numbers.end());
  Expect(std::ranges::is_sorted(version.files[0], std::ranges::greater{}, &FileMetaData::number));
  Expect(versions.LastSequence() <= forgedb::kMaxSequenceNumber);
  const std::uint64_t next = versions.NewFileNumber();
  Expect(numbers.empty() || next > numbers.back());
  Expect(next > versions.ManifestNumber());
}

void RecoverManifest(std::string_view input) {
  std::vector<std::string> records;
  while (!input.empty()) {
    const auto size = static_cast<unsigned char>(input.front());
    input.remove_prefix(1);
    const std::string_view record = input.substr(0, std::min<std::size_t>(size, input.size()));
    input.remove_prefix(record.size());
    records.emplace_back(record);
  }

  forgedb::MemFileSystem fs;
  Expect(fs.CreateDir(kDb).ok());
  {
    const std::unique_ptr<forgedb::WritableFile> file =
        std::move(fs.NewWritableFile(forgedb::DescriptorFileName(kDb, 1))).value();
    forgedb::log::Writer writer(file.get());
    for (const std::string& record : records) {
      Expect(writer.AddRecord(record).ok());
    }
    Expect(file->Close().ok());
  }
  WriteFile(fs, forgedb::CurrentFileName(kDb), "MANIFEST-000001\n");
  // The tables the records add, so recovery can get past its size check.
  for (const std::string& record : records) {
    const forgedb::Result<VersionEdit> edit = VersionEdit::Decode(record);
    if (!edit.ok()) {
      continue;
    }
    for (const VersionEdit::AddedFile& added : edit->added_files) {
      if (added.file.file_size <= kMaxTableBytes && added.file.number > 1) {
        WriteFile(fs, forgedb::TableFileName(kDb, added.file.number),
                  std::string(added.file.file_size, 't'));
      }
    }
  }

  State recovered;
  {
    VersionSet versions(&fs, kDb);
    forgedb::ManifestRecoveryInfo info;
    const Status status = versions.Recover(std::move(fs.GetChildren(kDb)).value(), info);
    if (!status.ok()) {
      Expect(IsCorruption(status) || status.code() == Status::Code::kNotSupported);
      return;
    }
    CheckConsistent(versions);
    recovered = StateOf(versions);
    // A new MANIFEST holding a snapshot of this state.
    Expect(versions.StartNewManifest(VersionEdit{}).ok());
    Expect(StateOf(versions) == recovered);
  }
  VersionSet versions(&fs, kDb);
  forgedb::ManifestRecoveryInfo info;
  Expect(versions.Recover(std::move(fs.GetChildren(kDb)).value(), info).ok());
  Expect(info.manifest_used.has_value() && *info.manifest_used > 1);
  Expect(StateOf(versions) == recovered);
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
    DecodeOne(rest);
  } else {
    RecoverManifest(rest);
  }
  return 0;
}
