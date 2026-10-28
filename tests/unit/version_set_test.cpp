// VersionSet: MANIFEST writing, and which MANIFEST recovery trusts (ADR 0007).
#include "db/version_set.h"

#include <cstddef>
#include <cstdint>
#include <filesystem>

#include <gtest/gtest.h>

#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "forgedb/result.h"
#include "forgedb/status.h"

#include "db/filename.h"
#include "db/internal_key.h"
#include "db/version_edit.h"
#include "env/file_system.h"
#include "env/mem_file_system.h"
#include "log/log_writer.h"

namespace forgedb {
namespace {

std::string Key(std::string_view user_key, SequenceNumber sequence) {
  std::string key;
  AppendInternalKey(key, {.user_key = user_key, .sequence = sequence, .type = ValueType::kValue});
  return key;
}

class VersionSetTest : public testing::Test {
 protected:
  void SetUp() override { ASSERT_TRUE(fs_.CreateDir(db_).ok()); }

  std::vector<std::string> Children() { return std::move(fs_.GetChildren(db_)).value(); }

  void WriteFile(const std::filesystem::path& path, std::string_view contents) {
    const std::unique_ptr<WritableFile> file = std::move(fs_.NewWritableFile(path)).value();
    ASSERT_TRUE(file->Append(contents).ok());
    ASSERT_TRUE(file->Close().ok());
  }

  std::string ReadFile(const std::filesystem::path& path) {
    const std::unique_ptr<SequentialFile> file = std::move(fs_.NewSequentialFile(path)).value();
    std::string contents(1 << 16, '\0');
    contents.resize(std::move(file->Read(std::span<char>(contents))).value());
    return contents;
  }

  // A table file of `size` bytes (the VersionSet only checks its size).
  FileMetaData MakeTable(std::uint64_t number, std::uint64_t size, std::string_view first,
                         std::string_view last) {
    WriteFile(TableFileName(db_, number), std::string(size, 't'));
    return {
        .number = number, .file_size = size, .smallest = Key(first, 1), .largest = Key(last, 1)};
  }

  // A recovered VersionSet over the current directory.
  struct Recovered {
    std::unique_ptr<VersionSet> versions;
    ManifestRecoveryInfo info;
    Status status;
  };
  Recovered Recover() {
    Recovered r{.versions = std::make_unique<VersionSet>(&fs_, db_), .info = {}, .status = {}};
    r.status = r.versions->Recover(Children(), r.info);
    return r;
  }

  // A fresh database with one table (number 1), log 2 and MANIFEST 3.
  void CreateDatabase() {
    const Recovered r = Recover();
    ASSERT_TRUE(r.status.ok());
    VersionEdit edit;
    edit.added_files.push_back(
        {.level = 0, .file = MakeTable(r.versions->NewFileNumber(), 100, "a", "m")});
    edit.log_number = r.versions->NewFileNumber();
    edit.last_sequence = 10;
    ASSERT_TRUE(r.versions->StartNewManifest(edit).ok());
    ASSERT_EQ(r.versions->ManifestNumber(), 3U);
  }

  const std::filesystem::path db_{"db"};
  MemFileSystem fs_;
};

TEST_F(VersionSetTest, NewDatabase) {
  const Recovered r = Recover();
  ASSERT_TRUE(r.status.ok()) << r.status.ToString();
  EXPECT_EQ(r.info.manifest_used, std::nullopt);
  EXPECT_EQ(r.versions->Current()->NumFiles(), 0U);
  EXPECT_EQ(r.versions->LogNumber(), 0U);
  EXPECT_EQ(r.versions->LastSequence(), 0U);
}

TEST_F(VersionSetTest, StartNewManifestThenRecover) {
  CreateDatabase();
  EXPECT_EQ(ReadFile(CurrentFileName(db_)), "MANIFEST-000003\n");
  const Recovered r = Recover();
  ASSERT_TRUE(r.status.ok()) << r.status.ToString();
  EXPECT_EQ(r.info.manifest_used, 3U);
  EXPECT_EQ(r.info.current_names, 3U);
  EXPECT_EQ(r.info.edits_applied, 1U);
  EXPECT_EQ(r.versions->LogNumber(), 2U);
  EXPECT_EQ(r.versions->LastSequence(), 10U);
  ASSERT_EQ(r.versions->Current()->files[0].size(), 1U);
  EXPECT_EQ(r.versions->Current()->files[0][0].number, 1U);
  EXPECT_EQ(r.versions->NewFileNumber(), 4U);
}

TEST_F(VersionSetTest, LogAndApplyIsRecovered) {
  CreateDatabase();
  {
    const Recovered r = Recover();
    ASSERT_TRUE(r.status.ok());
    VersionEdit snapshot;
    snapshot.log_number = r.versions->NewFileNumber();         // 4
    ASSERT_TRUE(r.versions->StartNewManifest(snapshot).ok());  // MANIFEST-5
    VersionEdit flush;
    flush.added_files.push_back(
        {.level = 0, .file = MakeTable(r.versions->NewFileNumber(), 50, "c", "z")});
    flush.log_number = r.versions->NewFileNumber();
    flush.last_sequence = 20;
    ASSERT_TRUE(r.versions->LogAndApply(flush).ok());
    VersionEdit removal;
    removal.removed_files.push_back({.level = 0, .number = 1});
    ASSERT_TRUE(r.versions->LogAndApply(removal).ok());
  }
  const Recovered r = Recover();
  ASSERT_TRUE(r.status.ok()) << r.status.ToString();
  EXPECT_EQ(r.info.manifest_used, 5U);
  EXPECT_EQ(r.info.edits_applied, 3U);
  EXPECT_EQ(r.versions->LogNumber(), 7U);
  EXPECT_EQ(r.versions->LastSequence(), 20U);
  ASSERT_EQ(r.versions->Current()->files[0].size(), 1U);
  EXPECT_EQ(r.versions->Current()->files[0][0].number, 6U);
}

// A MANIFEST longer than one 32 KiB log block: the edits after the snapshot
// must still be placed by the log's block rules. (Found by the corruption
// suite: the writer for later edits started counting at 0 after the
// snapshot, so the first record to cross a block boundary was unreadable.)
TEST_F(VersionSetTest, ManifestsSpanningManyLogBlocks) {
  constexpr int kEdits = 500;  // ~230 bytes each: about 3.5 blocks
  const std::string long_key(200, 'k');
  {
    const Recovered r = Recover();
    ASSERT_TRUE(r.status.ok());
    VersionEdit snapshot;
    snapshot.added_files.push_back(
        {.level = 0, .file = MakeTable(r.versions->NewFileNumber(), 1, long_key, long_key)});
    ASSERT_TRUE(r.versions->StartNewManifest(snapshot).ok());
    for (int i = 0; i < kEdits; ++i) {
      VersionEdit flush;
      flush.added_files.push_back(
          {.level = 0,
           .file = MakeTable(r.versions->NewFileNumber(), 1, long_key + std::to_string(i),
                             long_key + std::to_string(i))});
      flush.last_sequence = static_cast<SequenceNumber>(i + 1);
      ASSERT_TRUE(r.versions->LogAndApply(flush).ok()) << i;
    }
    ASSERT_GT(
        std::move(fs_.GetFileSize(DescriptorFileName(db_, r.versions->ManifestNumber()))).value(),
        3U * 32768U);
  }
  const Recovered r = Recover();
  ASSERT_TRUE(r.status.ok()) << r.status.ToString();
  EXPECT_EQ(r.info.edits_applied, std::uint64_t{kEdits} + 1);
  EXPECT_FALSE(r.info.torn_tail.has_value());
  EXPECT_EQ(r.versions->Current()->files[0].size(), std::size_t{kEdits} + 1);
  EXPECT_EQ(r.versions->LastSequence(), std::uint64_t{kEdits});
}

TEST_F(VersionSetTest, LevelZeroIsNewestFirst) {
  const Recovered r = Recover();
  ASSERT_TRUE(r.status.ok());
  VersionEdit edit;
  edit.added_files.push_back({.level = 0, .file = MakeTable(5, 10, "a", "b")});
  edit.added_files.push_back({.level = 0, .file = MakeTable(9, 10, "a", "b")});
  edit.added_files.push_back({.level = 0, .file = MakeTable(7, 10, "a", "b")});
  ASSERT_TRUE(r.versions->StartNewManifest(edit).ok());
  const auto& level0 = r.versions->Current()->files[0];
  ASSERT_EQ(level0.size(), 3U);
  EXPECT_EQ(level0[0].number, 9U);
  EXPECT_EQ(level0[1].number, 7U);
  EXPECT_EQ(level0[2].number, 5U);
}

// --- Which MANIFEST is trusted (ADR 0007) ------------------------------------

TEST_F(VersionSetTest, AnUnfinishedEditAtTheEndIsIgnored) {
  CreateDatabase();
  {
    const Recovered r = Recover();
    ASSERT_TRUE(r.status.ok());
    ASSERT_TRUE(r.versions->StartNewManifest(VersionEdit{}).ok());  // MANIFEST-4
    VersionEdit flush;
    flush.added_files.push_back(
        {.level = 0, .file = MakeTable(r.versions->NewFileNumber(), 50, "c", "z")});
    ASSERT_TRUE(r.versions->LogAndApply(flush).ok());
  }
  const std::string manifest = ReadFile(DescriptorFileName(db_, 4));
  WriteFile(DescriptorFileName(db_, 4), std::string_view(manifest).substr(0, manifest.size() - 5));
  const Recovered r = Recover();
  ASSERT_TRUE(r.status.ok()) << r.status.ToString();
  EXPECT_EQ(r.info.manifest_used, 4U);
  EXPECT_TRUE(r.info.torn_tail.has_value());
  EXPECT_EQ(r.versions->Current()->NumFiles(), 1U) << "the torn edit's file is not listed";
}

TEST_F(VersionSetTest, ANewerManifestWithAnIncompleteSnapshotIsSkipped) {
  CreateDatabase();
  // A crash while MANIFEST-8 was being written, before CURRENT was switched.
  const std::string complete = ReadFile(DescriptorFileName(db_, 3));
  WriteFile(DescriptorFileName(db_, 8), std::string_view(complete).substr(0, 10));
  WriteFile(DescriptorFileName(db_, 9), "");
  const Recovered r = Recover();
  ASSERT_TRUE(r.status.ok()) << r.status.ToString();
  EXPECT_EQ(r.info.manifest_used, 3U);
  EXPECT_EQ(r.info.manifests_skipped, (std::vector<std::uint64_t>{9, 8}));
  EXPECT_EQ(r.versions->NewFileNumber(), 10U) << "above every number in the directory";
}

TEST_F(VersionSetTest, ANewerCompleteManifestWinsOverCurrent) {
  CreateDatabase();
  {
    const Recovered r = Recover();
    ASSERT_TRUE(r.status.ok());
    ASSERT_TRUE(r.versions->StartNewManifest(VersionEdit{}).ok());  // MANIFEST-4
  }
  // A power loss undid the rename: CURRENT names MANIFEST-3 again.
  WriteFile(CurrentFileName(db_), "MANIFEST-000003\n");
  const Recovered r = Recover();
  ASSERT_TRUE(r.status.ok()) << r.status.ToString();
  EXPECT_EQ(r.info.current_names, 3U);
  EXPECT_EQ(r.info.manifest_used, 4U);
}

TEST_F(VersionSetTest, MissingOrUnreadableCurrentIsOnlyAHint) {
  CreateDatabase();
  ASSERT_TRUE(fs_.RemoveFile(CurrentFileName(db_)).ok());
  Recovered r = Recover();
  ASSERT_TRUE(r.status.ok()) << r.status.ToString();
  EXPECT_EQ(r.info.manifest_used, 3U);
  EXPECT_EQ(r.info.current_names, std::nullopt);

  WriteFile(CurrentFileName(db_), "garbage");
  r = Recover();
  ASSERT_TRUE(r.status.ok()) << r.status.ToString();
  EXPECT_EQ(r.info.manifest_used, 3U);
  EXPECT_EQ(r.info.current_names, std::nullopt);
}

TEST_F(VersionSetTest, CurrentNamingAnIncompleteManifestIsCorruption) {
  CreateDatabase();
  WriteFile(DescriptorFileName(db_, 8), "");
  WriteFile(CurrentFileName(db_), "MANIFEST-000008\n");
  const Recovered r = Recover();
  EXPECT_EQ(r.status.code(), Status::Code::kCorruption) << r.status.ToString();
  EXPECT_NE(r.status.ToString().find("CURRENT names an incomplete MANIFEST"), std::string::npos);
}

TEST_F(VersionSetTest, CurrentWithoutAnyManifestIsCorruption) {
  WriteFile(CurrentFileName(db_), "MANIFEST-000003\n");
  EXPECT_EQ(Recover().status.code(), Status::Code::kCorruption);
}

TEST_F(VersionSetTest, DamageInsideTheManifestIsCorruption) {
  CreateDatabase();
  {
    const Recovered r = Recover();
    ASSERT_TRUE(r.status.ok());
    ASSERT_TRUE(r.versions->StartNewManifest(VersionEdit{}).ok());  // MANIFEST-4
    VersionEdit flush;
    flush.added_files.push_back(
        {.level = 0, .file = MakeTable(r.versions->NewFileNumber(), 50, "c", "z")});
    ASSERT_TRUE(r.versions->LogAndApply(flush).ok());
  }
  std::string manifest = ReadFile(DescriptorFileName(db_, 4));
  manifest[12] = static_cast<char>(manifest[12] ^ 0x01);  // in the snapshot; the edit follows
  WriteFile(DescriptorFileName(db_, 4), manifest);
  const Recovered r = Recover();
  EXPECT_EQ(r.status.code(), Status::Code::kCorruption);
  EXPECT_NE(r.status.ToString().find("MANIFEST-000004"), std::string::npos) << r.status.ToString();
}

TEST_F(VersionSetTest, AFirstRecordThatIsNotASnapshotIsCorruption) {
  const std::unique_ptr<WritableFile> file =
      std::move(fs_.NewWritableFile(DescriptorFileName(db_, 5))).value();
  log::Writer writer(file.get());
  VersionEdit partial;
  partial.log_number = 1;  // no next file number or last sequence
  std::string record;
  partial.EncodeTo(record);
  ASSERT_TRUE(writer.AddRecord(record).ok());
  ASSERT_TRUE(file->Close().ok());
  const Recovered r = Recover();
  EXPECT_EQ(r.status.code(), Status::Code::kCorruption);
  EXPECT_NE(r.status.ToString().find("not a full snapshot"), std::string::npos);
}

// The file-number counter must never wrap around to numbers in use: numbers
// from 2^63 on, in the MANIFEST or in a file name, are refused.
TEST_F(VersionSetTest, ExhaustedFileNumbersAreRefused) {
  {
    const std::unique_ptr<WritableFile> file =
        std::move(fs_.NewWritableFile(DescriptorFileName(db_, 5))).value();
    log::Writer writer(file.get());
    VersionEdit snapshot;
    snapshot.log_number = 1;
    snapshot.next_file_number = std::uint64_t{1} << 63U;
    snapshot.last_sequence = 0;
    std::string record;
    snapshot.EncodeTo(record);
    ASSERT_TRUE(writer.AddRecord(record).ok());
    ASSERT_TRUE(file->Close().ok());
  }
  Recovered r = Recover();
  EXPECT_EQ(r.status.code(), Status::Code::kNotSupported) << r.status.ToString();

  ASSERT_TRUE(fs_.RemoveFile(DescriptorFileName(db_, 5)).ok());
  WriteFile(TableFileName(db_, std::uint64_t{1} << 63U), "");  // an unlisted leftover
  r = Recover();
  EXPECT_EQ(r.status.code(), Status::Code::kNotSupported) << r.status.ToString();

  ASSERT_TRUE(fs_.RemoveFile(TableFileName(db_, std::uint64_t{1} << 63U)).ok());
  WriteFile(TableFileName(db_, (std::uint64_t{1} << 63U) - 1), "");
  r = Recover();
  EXPECT_TRUE(r.status.ok()) << r.status.ToString();
}

TEST_F(VersionSetTest, AMissingOrResizedTableIsCorruption) {
  CreateDatabase();
  WriteFile(TableFileName(db_, 1), std::string(99, 't'));  // listed with 100 bytes
  Recovered r = Recover();
  EXPECT_EQ(r.status.code(), Status::Code::kCorruption);
  EXPECT_NE(r.status.ToString().find("size differs"), std::string::npos) << r.status.ToString();

  ASSERT_TRUE(fs_.RemoveFile(TableFileName(db_, 1)).ok());
  r = Recover();
  EXPECT_EQ(r.status.code(), Status::Code::kCorruption);
  EXPECT_NE(r.status.ToString().find("missing"), std::string::npos) << r.status.ToString();
}

TEST_F(VersionSetTest, InconsistentEditsAreCorruption) {
  const Recovered r = Recover();
  ASSERT_TRUE(r.status.ok());
  ASSERT_TRUE(r.versions->StartNewManifest(VersionEdit{}).ok());
  VersionEdit removal;
  removal.removed_files.push_back({.level = 0, .number = 42});
  EXPECT_EQ(r.versions->LogAndApply(removal).code(), Status::Code::kCorruption);
  VersionEdit twice;
  const FileMetaData table = MakeTable(43, 10, "a", "b");
  twice.added_files.push_back({.level = 0, .file = table});
  twice.added_files.push_back({.level = 1, .file = table});
  EXPECT_EQ(r.versions->LogAndApply(twice).code(), Status::Code::kCorruption);
}

}  // namespace
}  // namespace forgedb
