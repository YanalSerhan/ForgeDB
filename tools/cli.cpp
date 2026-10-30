#include "tools/cli.h"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <iomanip>
#include <ios>
#include <memory>
#include <optional>
#include <ostream>
#include <span>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "forgedb/db.h"
#include "forgedb/iterator.h"
#include "forgedb/options.h"
#include "forgedb/result.h"
#include "forgedb/stats.h"
#include "forgedb/status.h"

#include "db/db_impl.h"
#include "db/filename.h"
#include "db/version_edit.h"
#include "db/version_set.h"
#include "env/file_system.h"
#include "tools/tool_util.h"

namespace forgedb::tools {
namespace {

constexpr int kOk = 0;
constexpr int kNotFound = 1;
constexpr int kError = 2;

constexpr std::string_view kUsage =
    "usage: forgedb_cli <db-dir> put <key> <value>\n"
    "       forgedb_cli <db-dir> get <key>\n"
    "       forgedb_cli <db-dir> delete <key>\n"
    "       forgedb_cli <db-dir> scan [--from <key>] [--limit <n>]\n"
    "       forgedb_cli <db-dir> stats\n";

std::string LogName(std::uint64_t number) { return LogFileName("", number).generic_string(); }
std::string TableName(std::uint64_t number) { return TableFileName("", number).generic_string(); }
std::string ManifestName(std::uint64_t number) {
  return DescriptorFileName("", number).generic_string();
}

int Fail(std::ostream& err, const Status& status) {
  err << "forgedb_cli: " << status.ToString() << '\n';
  return kError;
}

int Usage(std::ostream& err, std::string_view problem) {
  err << "forgedb_cli: " << problem << '\n' << kUsage;
  return kError;
}

int Scan(DB& db, std::span<const std::string> args, std::ostream& out, std::ostream& err) {
  std::optional<std::string> from;
  std::optional<std::uint64_t> limit;
  for (std::size_t i = 0; i < args.size(); i += 2) {
    if (i + 1 >= args.size()) {
      return Usage(err, args[i] + " needs a value");
    }
    if (args[i] == "--from") {
      from = args[i + 1];
    } else if (args[i] == "--limit") {
      limit = ParseUnsigned(args[i + 1]);
      if (!limit) {
        return Usage(err, "--limit needs a number, not " + args[i + 1]);
      }
    } else {
      return Usage(err, "unknown scan option " + args[i]);
    }
  }
  const std::unique_ptr<Iterator> it = db.NewIterator(ReadOptions{});
  if (from) {
    it->Seek(*from);
  } else {
    it->SeekToFirst();
  }
  for (std::uint64_t n = 0; it->Valid() && (!limit || n < *limit); it->Next(), ++n) {
    out << EscapeBytes(it->key()) << '\t' << EscapeBytes(it->value()) << '\n';
  }
  if (!it->status().ok()) {
    return Fail(err, it->status());
  }
  return kOk;
}

void PrintRecovery(const RecoveryInfo& info, std::ostream& out) {
  const ManifestRecoveryInfo& manifest = info.manifest;
  out << "recovery (when this command opened the database):\n";
  out << "  MANIFEST read: "
      << (manifest.manifest_used ? ManifestName(*manifest.manifest_used) : "none (new database)");
  if (manifest.manifest_used) {
    out << ", " << Count(manifest.edits_applied, "record");
  }
  out << "; CURRENT named "
      << (manifest.current_names ? ManifestName(*manifest.current_names) : "nothing readable")
      << '\n';
  for (const std::uint64_t skipped : manifest.manifests_skipped) {
    out << "  skipped " << ManifestName(skipped) << " (its snapshot record is incomplete)\n";
  }
  if (manifest.torn_tail) {
    out << "  ignored the MANIFEST's unfinished last record at offset "
        << manifest.torn_tail->offset << ": " << manifest.torn_tail->reason << '\n';
  }
  out << "  logs replayed:";
  if (info.logs_replayed.empty()) {
    out << " none";
  }
  for (const std::uint64_t log : info.logs_replayed) {
    out << ' ' << LogName(log);
  }
  out << " (" << Count(info.records_replayed, "record") << ")\n";
  for (const RecoveryInfo::TornTail& tail : info.torn_tails) {
    out << "  " << LogName(tail.log_number) << ": ignored the tail from offset " << tail.offset
        << ": " << tail.reason;
    if (tail.valid_data_dropped) {
      out << " (WARNING: valid records after it were dropped)";
    }
    out << '\n';
  }
  out << "  recovered writes: "
      << (info.table_written ? "written to " + TableName(*info.table_written) : "none") << '\n';
  out << "  started " << LogName(info.new_log_number) << " and "
      << ManifestName(info.new_manifest_number) << '\n';
  out << "  deleted " << Count(info.files_deleted.size(), "obsolete file");
  for (const std::string& name : info.files_deleted) {
    out << ' ' << name;
  }
  out << '\n';
}

// "12.345 ms".
std::string Milliseconds(std::uint64_t microseconds) {
  std::ostringstream text;
  text << microseconds / 1000 << '.' << std::setw(3) << std::setfill('0') << microseconds % 1000
       << " ms";
  return text.str();
}

std::string Stall(const StallStats& stall) {
  return std::to_string(stall.count) + " (" + Milliseconds(stall.microseconds) + ")";
}

void Stats(DBImpl& db, std::ostream& out) {
  const std::shared_ptr<const Version> version = db.CurrentVersion();
  out << "last sequence number: " << db.LastSequence() << '\n';
  out << "tables: " << version->NumFiles() << '\n';
  for (std::size_t level = 0; level < version->files.size(); ++level) {
    const std::vector<FileMetaData>& files = version->files[level];
    if (files.empty()) {
      continue;
    }
    std::uint64_t bytes = 0;
    for (const FileMetaData& file : files) {
      bytes += file.file_size;
    }
    out << "level " << level << ": " << Count(files.size(), "table") << ", " << bytes << " bytes\n";
    for (const FileMetaData& file : files) {
      out << "  " << TableName(file.number) << "  " << file.file_size << " bytes  "
          << FormatInternalKey(file.smallest) << " .. " << FormatInternalKey(file.largest) << '\n';
    }
  }
  PrintActivity(db.GetStats(), out);
  PrintRecovery(db.GetRecoveryInfo(), out);
}

}  // namespace

void PrintActivity(const DBStats& stats, std::ostream& out) {
  out << "since this open:\n";
  out << "  user writes: " << stats.user_bytes << " bytes (log: " << stats.log_bytes << " bytes)\n";
  out << "  flushes: " << stats.flushes << " (" << stats.flush_bytes_written << " bytes written, "
      << Milliseconds(stats.flush_microseconds) << ")\n";
  bool any_compaction = false;
  for (std::size_t level = 0; level < stats.levels.size(); ++level) {
    const CompactionStats& compactions = stats.levels[level].compactions;
    if (compactions.count == 0 && compactions.trivial_moves == 0) {
      continue;
    }
    any_compaction = true;
    out << "  compactions level " << level << " -> " << level + 1 << ": " << compactions.count
        << " (+" << Count(compactions.trivial_moves, "trivial move") << "), read "
        << compactions.bytes_read << " bytes, wrote " << compactions.bytes_written << " bytes, "
        << Milliseconds(compactions.microseconds) << '\n';
  }
  if (!any_compaction) {
    out << "  compactions: none\n";
  }
  out << "  write amplification: ";
  if (const std::optional<double> amplification = stats.WriteAmplification()) {
    out << std::fixed << std::setprecision(2) << *amplification << std::defaultfloat << " ("
        << stats.TableBytesWritten() << " table bytes / " << stats.user_bytes << " user bytes)\n";
  } else {
    out << "n/a, no writes (" << stats.TableBytesWritten() << " table bytes written)\n";
  }
  out << "  stalls: memtable full " << Stall(stats.memtable_full) << ", level-0 slowdown "
      << Stall(stats.level0_slowdown) << ", level-0 stop " << Stall(stats.level0_stop) << '\n';
}

int RunCli(std::span<const std::string> args, std::shared_ptr<FileSystem> fs, std::ostream& out,
           std::ostream& err) {
  if (args.size() < 2) {
    return Usage(err, "missing arguments");
  }
  const std::filesystem::path dir = PathFromUtf8(args[0]);
  const std::string& command = args[1];
  const std::span<const std::string> rest = args.subspan(2);

  // How many arguments each command takes (scan: options, checked later).
  std::optional<std::size_t> arity;
  if (command == "put") {
    arity = 2;
  } else if (command == "get" || command == "delete") {
    arity = 1;
  } else if (command == "stats") {
    arity = 0;
  } else if (command != "scan") {
    return Usage(err, "unknown command " + command);
  }
  if (arity && rest.size() != *arity) {
    return Usage(err, "wrong number of arguments for " + command);
  }
  const bool writes = command == "put" || command == "delete";
  if (!writes) {
    // Reading commands do not create a database (a mistyped path would).
    if (const Result<std::vector<std::string>> children = fs->GetChildren(dir); !children.ok()) {
      return Fail(err, Status::NotFound("no database at " + args[0], children.status().ToString()));
    }
  }

  Result<std::unique_ptr<DB>> opened = OpenDB(Options{}, dir, std::move(fs));
  if (!opened.ok()) {
    return Fail(err, opened.status());
  }
  DB& db = **opened;
  const WriteOptions synced{.sync = true};

  if (command == "put") {
    const Status status = db.Put(synced, rest[0], rest[1]);
    return status.ok() ? kOk : Fail(err, status);
  }
  if (command == "delete") {
    const Status status = db.Delete(synced, rest[0]);
    return status.ok() ? kOk : Fail(err, status);
  }
  if (command == "get") {
    const Result<std::string> value = db.Get(ReadOptions{}, rest[0]);
    if (value.status().code() == Status::Code::kNotFound) {
      err << "forgedb_cli: not found: " << EscapeBytes(rest[0]) << '\n';
      return kNotFound;
    }
    if (!value.ok()) {
      return Fail(err, value.status());
    }
    out << *value << '\n';
    return kOk;
  }
  if (command == "scan") {
    return Scan(db, rest, out, err);
  }
  Stats(dynamic_cast<DBImpl&>(db), out);
  return kOk;
}

}  // namespace forgedb::tools
