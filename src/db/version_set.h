// Version and VersionSet: which table files make up the database, and the
// MANIFEST that records it (ADR 0007).
//
// A Version is an immutable list of table files per level, shared by
// std::shared_ptr: a read keeps the version it started with alive (and so its
// files listed) for as long as it needs it.
//
// The VersionSet holds the current version and the counters that go with it
// (log number, next file number, last sequence number), and writes every
// change to the MANIFEST before it takes effect:
//
//   - Recover() picks the MANIFEST to trust (ADR 0007): the newest
//     MANIFEST-N whose first record, a full snapshot, is complete. CURRENT is
//     only a hint, because Windows does not document that a rename survives a
//     power loss (ADR 0002). A MANIFEST whose first record is incomplete was
//     never switched to and is skipped; damage anywhere else is Corruption.
//   - StartNewManifest() writes a snapshot of the state (with an edit
//     applied) to a new MANIFEST, syncs it, then switches CURRENT: write
//     CURRENT.tmp, sync it, rename it over CURRENT. Done at every open.
//   - LogAndApply() appends an edit to the MANIFEST and syncs it, then makes
//     the result current. A file an edit adds must already be synced.
//
// Not thread-safe, except NewFileNumber(): the DB calls it from one thread at
// a time (the opening thread, then the background thread) and publishes
// Current() to readers under its own mutex; the write leader also takes file
// numbers for new logs.
#pragma once

#include <array>
#include <atomic>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "forgedb/result.h"
#include "forgedb/status.h"

#include "db/internal_key.h"
#include "db/version_edit.h"
#include "env/file_system.h"
#include "log/log_reader.h"
#include "log/log_writer.h"

namespace forgedb {

struct Version {
  // Level 0: newest file (highest number) first; files there may overlap.
  // Deeper levels: sorted by smallest key, never overlapping, and no user key
  // in two tables (ADR 0008).
  std::array<std::vector<FileMetaData>, kNumLevels> files;

  [[nodiscard]] std::size_t NumFiles() const noexcept;
};

// What VersionSet::Recover found.
struct ManifestRecoveryInfo {
  std::optional<std::uint64_t> manifest_used;      // nullopt: a new (or Phase 2) database
  std::vector<std::uint64_t> manifests_skipped;    // newer, with an incomplete first record
  std::optional<std::uint64_t> current_names;      // what CURRENT said, if it was readable
  std::uint64_t edits_applied = 0;                 // records replayed, the snapshot included
  std::optional<log::Reader::TornTail> torn_tail;  // an unfinished last edit, ignored
};

class VersionSet {
 public:
  VersionSet(FileSystem* fs, std::filesystem::path db_dir);
  ~VersionSet();
  VersionSet(const VersionSet&) = delete;
  VersionSet& operator=(const VersionSet&) = delete;
  VersionSet(VersionSet&&) = delete;
  VersionSet& operator=(VersionSet&&) = delete;

  // Loads the state from the MANIFEST to trust, given the directory listing.
  // No MANIFEST (and no CURRENT): an empty state, as for a new database.
  // Every listed table must exist with its recorded size. Also makes the next
  // file number larger than every file number in `children`.
  [[nodiscard]] Status Recover(const std::vector<std::string>& children,
                               ManifestRecoveryInfo& info);

  // Applies `edit` to the current state, writes the result as the first
  // record of a new MANIFEST, syncs it, and switches CURRENT to it.
  [[nodiscard]] Status StartNewManifest(const VersionEdit& edit);

  // Appends `edit` (its scalars filled in from the current state where unset)
  // to the MANIFEST, syncs it, and makes the result current. On error nothing
  // changes, and the MANIFEST refuses further writes (WritableFile errors
  // stick); the DB then refuses writes.
  [[nodiscard]] Status LogAndApply(VersionEdit edit);

  // A new file number (logs, tables and MANIFESTs share one counter). The
  // one member that two threads use at once: the write leader takes numbers
  // for new logs while a compaction takes them for tables.
  [[nodiscard]] std::uint64_t NewFileNumber() noexcept { return next_file_number_.fetch_add(1); }

  [[nodiscard]] std::shared_ptr<const Version> Current() const { return current_; }
  [[nodiscard]] std::uint64_t LogNumber() const noexcept { return log_number_; }
  [[nodiscard]] SequenceNumber LastSequence() const noexcept { return last_sequence_; }
  [[nodiscard]] std::uint64_t ManifestNumber() const noexcept { return manifest_number_; }

  // The numbers of the table files that the current version lists or that an
  // older version still in use lists (a read holds it), sorted. A table not
  // in this list can be deleted: no read can open it any more. Versions are
  // released by whichever thread drops the last reference; the VersionSet
  // keeps a weak_ptr to each one it installed and forgets the expired ones
  // here.
  [[nodiscard]] std::vector<std::uint64_t> LiveFiles();

 private:
  // Replays MANIFEST `number`. Returns false (no error) if its first record is
  // incomplete, so the caller tries an older one.
  [[nodiscard]] Result<bool> ReadManifest(std::uint64_t number, ManifestRecoveryInfo& info);
  // The state after `edit`: a new Version, or Corruption if the edit adds a
  // file that is already there or removes one that is not.
  [[nodiscard]] static Result<std::shared_ptr<Version>> Apply(const Version& base,
                                                              const VersionEdit& edit);
  void AdoptScalars(const VersionEdit& edit) noexcept;
  // A snapshot edit: the scalars and every file of `version`.
  [[nodiscard]] VersionEdit Snapshot(const Version& version) const;
  // Makes `version` current and remembers it for LiveFiles().
  void Install(std::shared_ptr<const Version> version);

  FileSystem* fs_;
  std::filesystem::path db_dir_;
  std::shared_ptr<const Version> current_;
  std::vector<std::weak_ptr<const Version>> installed_;  // every version made current
  std::uint64_t log_number_ = 0;
  std::atomic<std::uint64_t> next_file_number_{1};
  SequenceNumber last_sequence_ = 0;
  std::uint64_t manifest_number_ = 0;
  std::unique_ptr<WritableFile> manifest_file_;
  std::optional<log::Writer> manifest_log_;
};

}  // namespace forgedb
