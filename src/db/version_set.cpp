#include "db/version_set.h"

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
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
#include "log/log_reader.h"
#include "log/log_writer.h"

namespace forgedb {
namespace {

std::string ManifestName(std::uint64_t number) {
  return DescriptorFileName("", number).generic_string();
}

// Adds the MANIFEST's name to a corruption message.
Status InManifest(const Status& status, std::uint64_t number) {
  if (status.code() != Status::Code::kCorruption) {
    return status;
  }
  return Status::Corruption(ManifestName(number) + ": " + status.message());
}

// CURRENT holds "MANIFEST-NNNNNN\n". nullopt if it is missing or unreadable;
// it is only a hint (ADR 0007).
std::optional<std::uint64_t> ReadCurrentHint(FileSystem& fs, const std::filesystem::path& db) {
  Result<std::unique_ptr<SequentialFile>> file = fs.NewSequentialFile(CurrentFileName(db));
  if (!file.ok()) {
    return std::nullopt;
  }
  std::string contents(64, '\0');
  const Result<std::size_t> read = (*file)->Read(std::span<char>(contents));
  if (!read.ok()) {
    return std::nullopt;
  }
  contents.resize(*read);
  if (!contents.ends_with('\n')) {
    return std::nullopt;
  }
  contents.pop_back();
  const std::optional<ParsedFileName> parsed = ParseFileName(contents);
  if (!parsed || parsed->type != FileType::kDescriptor) {
    return std::nullopt;
  }
  return parsed->number;
}

}  // namespace

std::size_t Version::NumFiles() const noexcept {
  std::size_t n = 0;
  for (const std::vector<FileMetaData>& level : files) {
    n += level.size();
  }
  return n;
}

VersionSet::VersionSet(FileSystem* fs, std::filesystem::path db_dir)
    : fs_(fs), db_dir_(std::move(db_dir)) {
  Install(std::make_shared<const Version>());
}

VersionSet::~VersionSet() {
  if (manifest_file_ != nullptr) {
    static_cast<void>(manifest_file_->Close());  // every edit was synced when it was applied
  }
}

Result<std::shared_ptr<Version>> VersionSet::Apply(const Version& base, const VersionEdit& edit) {
  auto version = std::make_shared<Version>(base);
  for (const VersionEdit::RemovedFile& removed : edit.removed_files) {
    std::vector<FileMetaData>& files = version->files.at(static_cast<std::size_t>(removed.level));
    const auto it = std::ranges::find(files, removed.number, &FileMetaData::number);
    if (it == files.end()) {
      return Status::Corruption(
          "version edit removes a file that is not there",
          "file " + std::to_string(removed.number) + " at level " + std::to_string(removed.level));
    }
    files.erase(it);
  }
  for (const VersionEdit::AddedFile& added : edit.added_files) {
    for (const std::vector<FileMetaData>& files : version->files) {
      if (std::ranges::find(files, added.file.number, &FileMetaData::number) != files.end()) {
        return Status::Corruption("version edit adds a file that is already there",
                                  "file " + std::to_string(added.file.number));
      }
    }
    version->files.at(static_cast<std::size_t>(added.level)).push_back(added.file);
  }
  std::ranges::sort(version->files[0], std::ranges::greater{}, &FileMetaData::number);
  for (std::size_t level = 1; level < version->files.size(); ++level) {
    std::ranges::sort(version->files[level], [](const FileMetaData& a, const FileMetaData& b) {
      return CompareInternalKeys(a.smallest, b.smallest) < 0;
    });
  }
  return version;
}

void VersionSet::AdoptScalars(const VersionEdit& edit) noexcept {
  if (edit.log_number) {
    log_number_ = *edit.log_number;
  }
  if (edit.next_file_number) {
    next_file_number_.store(std::max(next_file_number_.load(), *edit.next_file_number));
  }
  if (edit.last_sequence) {
    last_sequence_ = *edit.last_sequence;
  }
}

VersionEdit VersionSet::Snapshot(const Version& version) const {
  VersionEdit snapshot;
  snapshot.log_number = log_number_;
  snapshot.next_file_number = next_file_number_.load();
  snapshot.last_sequence = last_sequence_;
  for (std::size_t level = 0; level < version.files.size(); ++level) {
    for (const FileMetaData& file : version.files[level]) {
      snapshot.added_files.push_back({.level = static_cast<int>(level), .file = file});
    }
  }
  return snapshot;
}

Result<bool> VersionSet::ReadManifest(std::uint64_t number, ManifestRecoveryInfo& info) {
  FORGEDB_ASSIGN_OR_RETURN(std::unique_ptr<SequentialFile> file,
                           fs_->NewSequentialFile(DescriptorFileName(db_dir_, number)));
  log::Reader reader(std::move(file));  // strict: a MANIFEST is never point-in-time

  const Result<std::optional<std::string_view>> first = reader.ReadRecord();
  if (!first.ok()) {
    return InManifest(first.status(), number);
  }
  if (!first->has_value()) {
    // Empty, or the snapshot record was cut short: this MANIFEST was being
    // written when the process stopped, and CURRENT was never switched to it.
    return false;
  }
  Result<VersionEdit> snapshot = VersionEdit::Decode(**first);
  if (!snapshot.ok()) {
    return InManifest(snapshot.status(), number);
  }
  if (!snapshot->HasAllScalars()) {
    return InManifest(Status::Corruption("first record is not a full snapshot"), number);
  }

  std::shared_ptr<const Version> version;
  {
    Result<std::shared_ptr<Version>> applied = Apply(Version{}, *snapshot);
    if (!applied.ok()) {
      return InManifest(applied.status(), number);
    }
    version = std::move(applied).value();
  }
  AdoptScalars(*snapshot);
  info.edits_applied = 1;

  while (true) {
    const Result<std::optional<std::string_view>> record = reader.ReadRecord();
    if (!record.ok()) {
      return InManifest(record.status(), number);
    }
    if (!record->has_value()) {
      break;
    }
    Result<VersionEdit> edit = VersionEdit::Decode(**record);
    if (!edit.ok()) {
      return InManifest(edit.status(), number);
    }
    Result<std::shared_ptr<Version>> applied = Apply(*version, *edit);
    if (!applied.ok()) {
      return InManifest(applied.status(), number);
    }
    version = std::move(applied).value();
    AdoptScalars(*edit);
    ++info.edits_applied;
  }
  info.torn_tail = reader.IgnoredTail();
  Install(std::move(version));
  manifest_number_ = number;
  return true;
}

Status VersionSet::Recover(const std::vector<std::string>& children, ManifestRecoveryInfo& info) {
  std::vector<std::uint64_t> manifests;
  bool has_current = false;
  std::uint64_t highest_number = 0;
  for (const std::string& child : children) {
    const std::optional<ParsedFileName> parsed = ParseFileName(child);
    if (!parsed) {
      continue;
    }
    highest_number = std::max(highest_number, parsed->number);
    if (parsed->type == FileType::kDescriptor) {
      manifests.push_back(parsed->number);
    } else if (parsed->type == FileType::kCurrent) {
      has_current = true;
    }
  }
  std::ranges::sort(manifests, std::ranges::greater{});
  info.current_names = has_current ? ReadCurrentHint(*fs_, db_dir_) : std::nullopt;

  // The newest MANIFEST with a complete snapshot wins (ADR 0007).
  for (const std::uint64_t number : manifests) {
    FORGEDB_ASSIGN_OR_RETURN(const bool complete, ReadManifest(number, info));
    if (complete) {
      info.manifest_used = number;
      break;
    }
    info.manifests_skipped.push_back(number);
  }
  if (info.current_names && std::ranges::find(info.manifests_skipped, *info.current_names) !=
                                info.manifests_skipped.end()) {
    // CURRENT is switched only after its MANIFEST is synced, so this is damage.
    return Status::Corruption("CURRENT names an incomplete MANIFEST",
                              ManifestName(*info.current_names));
  }
  if (!info.manifest_used && has_current) {
    return Status::Corruption("CURRENT exists but no MANIFEST is complete");
  }

  // Every table the MANIFEST lists must be there, whole.
  for (const std::vector<FileMetaData>& level : current_->files) {
    for (const FileMetaData& file : level) {
      const std::filesystem::path path = TableFileName(db_dir_, file.number);
      const Result<std::uint64_t> size = fs_->GetFileSize(path);
      if (!size.ok()) {
        return Status::Corruption(
            "table listed in the MANIFEST is missing or unreadable",
            path.filename().generic_string() + ": " + size.status().ToString());
      }
      if (*size != file.file_size) {
        return Status::Corruption("table size differs from the MANIFEST",
                                  path.filename().generic_string() + ": " + std::to_string(*size) +
                                      " bytes, expected " + std::to_string(file.file_size));
      }
    }
  }
  // Never reuse a number found in the directory (e.g. a table left by a
  // flush that crashed before its edit was written). Numbers from 2^63 on
  // never come from ForgeDB (a file per nanosecond would take 290 years), so
  // they are refused: the counter can then never wrap around to numbers in
  // use.
  constexpr std::uint64_t kFileNumberLimit = std::uint64_t{1} << 63U;
  if (highest_number >= kFileNumberLimit || next_file_number_ >= kFileNumberLimit) {
    return Status::NotSupported("file numbers exhausted",
                                "a file number or the MANIFEST's next file number is 2^63 or more");
  }
  next_file_number_.store(std::max(next_file_number_.load(), highest_number + 1));
  return Status::OK();
}

Status VersionSet::StartNewManifest(const VersionEdit& edit) {
  FORGEDB_ASSIGN_OR_RETURN(std::shared_ptr<Version> version, Apply(*current_, edit));
  AdoptScalars(edit);
  const std::uint64_t number = NewFileNumber();  // before the snapshot records the counter

  FORGEDB_ASSIGN_OR_RETURN(std::unique_ptr<WritableFile> file,
                           fs_->NewWritableFile(DescriptorFileName(db_dir_, number)));
  // The same writer appends every later edit: it tracks where the current
  // 32 KiB block ends (a writer started after the snapshot would place
  // fragments across block boundaries once the MANIFEST passes 32 KiB).
  log::Writer writer(file.get());
  std::string record;
  Snapshot(*version).EncodeTo(record);
  FORGEDB_RETURN_IF_ERROR(writer.AddRecord(record));
  FORGEDB_RETURN_IF_ERROR(file->Sync());

  // Switch CURRENT (ADR 0002 rule 5). Recovery does not depend on the rename
  // surviving a power loss (ADR 0007): the new MANIFEST is complete and synced.
  const std::filesystem::path temp = CurrentTempFileName(db_dir_);
  {
    FORGEDB_ASSIGN_OR_RETURN(const std::unique_ptr<WritableFile> current,
                             fs_->NewWritableFile(temp));
    FORGEDB_RETURN_IF_ERROR(current->Append(ManifestName(number) + "\n"));
    FORGEDB_RETURN_IF_ERROR(current->Sync());
    FORGEDB_RETURN_IF_ERROR(current->Close());
  }
  FORGEDB_RETURN_IF_ERROR(fs_->RenameReplace(temp, CurrentFileName(db_dir_)));

  if (manifest_file_ != nullptr) {
    static_cast<void>(manifest_file_->Close());
  }
  manifest_file_ = std::move(file);
  manifest_log_.emplace(writer);  // points at the file, which moved without changing address
  manifest_number_ = number;
  Install(std::move(version));
  return Status::OK();
}

Status VersionSet::LogAndApply(VersionEdit edit) {
  if (!manifest_log_) {
    return Status::Internal("LogAndApply before StartNewManifest");
  }
  if (!edit.log_number) {
    edit.log_number = log_number_;
  }
  if (!edit.last_sequence) {
    edit.last_sequence = last_sequence_;
  }
  // A file number the leader takes for a new log right after this may be
  // missing from the record; recovery also takes numbers above every file in
  // the directory, so it is never reused.
  edit.next_file_number = next_file_number_.load();
  FORGEDB_ASSIGN_OR_RETURN(std::shared_ptr<Version> version, Apply(*current_, edit));

  std::string record;
  edit.EncodeTo(record);
  FORGEDB_RETURN_IF_ERROR(manifest_log_->AddRecord(record));
  FORGEDB_RETURN_IF_ERROR(manifest_file_->Sync());
  AdoptScalars(edit);
  Install(std::move(version));
  return Status::OK();
}

void VersionSet::Install(std::shared_ptr<const Version> version) {
  installed_.emplace_back(version);
  current_ = std::move(version);
}

std::vector<std::uint64_t> VersionSet::LiveFiles() {
  std::vector<std::uint64_t> live;
  std::erase_if(installed_, [&live](const std::weak_ptr<const Version>& weak) {
    const std::shared_ptr<const Version> version = weak.lock();
    if (version == nullptr) {
      return true;  // released by every reader: forget it
    }
    for (const std::vector<FileMetaData>& level : version->files) {
      for (const FileMetaData& file : level) {
        live.push_back(file.number);
      }
    }
    return false;
  });
  std::ranges::sort(live);
  const auto duplicates = std::ranges::unique(live);
  live.erase(duplicates.begin(), duplicates.end());
  return live;
}

}  // namespace forgedb
