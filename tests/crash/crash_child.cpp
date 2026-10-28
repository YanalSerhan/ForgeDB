// The child process of the process-kill test (TODO 2.6). It writes keys with
// sync = true, forever, and after each acknowledged write appends its keys to
// a side file. The parent kills it at a random moment with TerminateProcess
// and then checks that every key in the side file is in the database.
//
//   forgedb_crash_child <db dir> <side file> <run> <seed>
//
// Keys are process_kill::KeyFor(run, 0), KeyFor(run, 1), ... in order, one to
// kMaxBatchKeys per write. The side file gets one line per key, appended and
// flushed to the OS after the write is acknowledged: a process kill cannot
// lose flushed bytes, so every complete line is a key the DB acknowledged.
//
// Exit codes (the parent fails the test on any exit; the child should only
// ever be killed): 2 usage, 3 open failed, 4 a write failed, 5 side file.
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <memory>
#include <optional>
#include <random>
#include <span>
#include <string>
#include <string_view>

#include "forgedb/db.h"
#include "forgedb/options.h"
#include "forgedb/write_batch.h"

#include "crash/process_kill_common.h"
#include "env/win_file_system.h"

namespace {

// A decimal number, digits only (no exceptions: see tests/support/test_env.cpp).
std::optional<std::uint64_t> ParseNumber(std::wstring_view text) {
  if (text.empty()) {
    return std::nullopt;
  }
  std::uint64_t value = 0;
  for (const wchar_t c : text) {
    if (c < L'0' || c > L'9') {
      return std::nullopt;
    }
    const auto digit = static_cast<std::uint64_t>(c - L'0');
    if (value > (UINT64_MAX - digit) / 10) {
      return std::nullopt;  // more than 64 bits
    }
    value = (value * 10) + digit;
  }
  return value;
}

int Fail(int code, const char* what, const forgedb::Status& status) {
  const std::string message =
      std::string("forgedb_crash_child: ") + what + ": " + status.ToString() + "\n";
  std::fputs(message.c_str(), stderr);
  return code;
}

int Run(const std::filesystem::path& db_dir, const std::filesystem::path& side_file,
        std::uint64_t run, std::uint64_t seed) {
  using forgedb::process_kill::KeyFor;
  using forgedb::process_kill::kMaxBatchKeys;
  using forgedb::process_kill::ValueFor;

  // A small memtable, so kills also land during flushes (Phase 3.9).
  forgedb::Options options;
  options.memtable_bytes = std::size_t{64} << 10;
  forgedb::Result<std::unique_ptr<forgedb::DB>> opened = forgedb::DB::Open(options, db_dir);
  if (!opened.ok()) {
    return Fail(3, "open", opened.status());
  }
  const std::unique_ptr<forgedb::DB> db = std::move(opened).value();

  forgedb::WinFileSystem fs;
  const forgedb::Result<std::unique_ptr<forgedb::WritableFile>> side =
      fs.NewWritableFile(side_file);
  if (!side.ok()) {
    return Fail(5, "side file", side.status());
  }

  std::mt19937_64 rng(seed);
  for (std::uint64_t next = 0;;) {
    const std::uint64_t keys = rng() % 5 == 0 ? 2 + (rng() % (kMaxBatchKeys - 1)) : 1;
    forgedb::WriteBatch batch;
    std::string lines;
    for (std::uint64_t i = 0; i < keys; ++i, ++next) {
      const std::string key = KeyFor(run, next);
      batch.Put(key, ValueFor(key));
      lines += key + "\n";
    }
    if (const forgedb::Status status = db->Write({.sync = true}, batch); !status.ok()) {
      return Fail(4, "write", status);
    }
    // Acknowledged: record it. Flush hands the bytes to the OS, which keeps
    // them when this process is killed.
    if (const forgedb::Status status = (*side)->Append(lines); !status.ok()) {
      return Fail(5, "side file append", status);
    }
    if (const forgedb::Status status = (*side)->Flush(); !status.ok()) {
      return Fail(5, "side file flush", status);
    }
  }
}

}  // namespace

// The wide-character entry point, so paths with any characters arrive intact.
int wmain(int argc, wchar_t** argv) {  // NOLINT(misc-use-internal-linkage): the entry point
  if (argc != 5) {
    std::fputs("usage: forgedb_crash_child <db dir> <side file> <run> <seed>\n", stderr);
    return 2;
  }
  const std::span<wchar_t*> args(argv, static_cast<std::size_t>(argc));
  const std::optional<std::uint64_t> run = ParseNumber(args[3]);
  const std::optional<std::uint64_t> seed = ParseNumber(args[4]);
  if (!run || !seed) {
    std::fputs("forgedb_crash_child: <run> and <seed> must be numbers\n", stderr);
    return 2;
  }
  return Run(args[1], args[2], *run, *seed);
}
