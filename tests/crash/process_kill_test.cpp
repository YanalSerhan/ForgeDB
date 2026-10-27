// Process-kill test (TODO 2.6): a real crash on Windows. The parent starts
// forgedb_crash_child, which writes keys with sync = true and records each
// acknowledged key in a side file; after a random wait the parent kills it
// with TerminateProcess, reopens the database and checks:
//
//   - every key in the side file is in the database, with its exact value;
//   - the child's keys form a prefix 0..m-1 (no holes), with at most one more
//     batch than the side file shows (acknowledged, killed before recording);
//   - keys from every earlier run are all still there.
//
// The random wait also kills the child while it opens the database, during
// recovery. This covers process crashes only: data the OS has accepted
// survives them. Power loss is covered by the fault-injection model
// (crash_torture_test.cpp), not here.
//
// Run-time overrides:
//   FORGEDB_KILL_RUNS=500   kill runs (default: see CMakeLists.txt)
//   FORGEDB_KILL_SEED=7     seed for the waits and the child's batch sizes
#include <windows.h>

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <filesystem>

#include <gtest/gtest.h>

#include <map>
#include <memory>
#include <random>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include "forgedb/db.h"
#include "forgedb/iterator.h"
#include "forgedb/options.h"

#include "crash/process_kill_common.h"
#include "env/unique_handle.h"
#include "env/win_file_system.h"
#include "support/test_env.h"

#ifndef FORGEDB_KILL_TEST_RUNS
#define FORGEDB_KILL_TEST_RUNS 20
#endif

namespace forgedb {
namespace {

using process_kill::KeyFor;
using process_kill::kMaxBatchKeys;
using process_kill::ValueFor;

// A fresh database every so many runs: Phase 2 never deletes logs, and every
// open replays all of them.
constexpr std::uint64_t kRunsPerDatabase = 25;

std::filesystem::path ChildPath() {
  std::wstring buffer(32768, L'\0');
  const DWORD length =
      ::GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
  buffer.resize(length);
  return std::filesystem::path(buffer).parent_path() / L"forgedb_crash_child.exe";
}

std::wstring Quoted(const std::filesystem::path& path) { return L"\"" + path.native() + L"\""; }

// Opens the database, retrying while it is Busy: Windows releases the dead
// child's lock a moment after the process ends (ADR 0002, F10).
Result<std::unique_ptr<DB>> OpenAfterKill(const std::filesystem::path& db_dir) {
  for (int attempt = 0;; ++attempt) {
    Result<std::unique_ptr<DB>> db = DB::Open(Options{}, db_dir);
    if (db.ok() || db.status().code() != Status::Code::kBusy || attempt == 500) {
      return db;
    }
    ::Sleep(10);
  }
}

// The complete lines of the side file. A line cut off by the kill has no
// newline yet and is not counted.
std::vector<std::string> CompleteLines(const std::filesystem::path& path) {
  std::vector<std::string> lines;
  WinFileSystem fs;
  const Result<std::unique_ptr<SequentialFile>> file = fs.NewSequentialFile(path);
  if (!file.ok()) {
    return lines;  // killed before the child created it
  }
  std::string contents;
  std::string buffer(65536, '\0');
  while (true) {
    const Result<std::size_t> read = (*file)->Read(buffer);
    if (!read.ok() || *read == 0) {
      break;
    }
    contents.append(buffer.data(), *read);
  }
  std::size_t start = 0;
  for (std::size_t newline = contents.find('\n'); newline != std::string::npos;
       newline = contents.find('\n', start)) {
    lines.push_back(contents.substr(start, newline - start));
    start = newline + 1;
  }
  return lines;
}

// Keys per run in the database, after checking every value.
std::map<std::uint64_t, std::vector<std::uint64_t>> KeysByRun(DB& db, std::string* problem) {
  std::map<std::uint64_t, std::vector<std::uint64_t>> keys;
  const std::unique_ptr<Iterator> it = db.NewIterator(ReadOptions{});
  for (it->SeekToFirst(); it->Valid(); it->Next()) {
    const std::string key(it->key());
    // "r0007-000123"
    const std::uint64_t run = std::stoull(key.substr(1, key.find('-') - 1));
    const std::uint64_t n = std::stoull(key.substr(key.find('-') + 1));
    if (it->value() != ValueFor(key) && problem->empty()) {
      *problem = "wrong value for " + key + " (" + std::to_string(it->value().size()) +
                 " bytes, expected " + std::to_string(ValueFor(key).size()) + ")";
    }
    keys[run].push_back(n);
  }
  if (!it->status().ok() && problem->empty()) {
    *problem = "iterator: " + it->status().ToString();
  }
  return keys;
}

TEST(ProcessKillTest, AcknowledgedSyncedWritesSurviveTerminateProcess) {
  const std::uint64_t runs = GetEnvNumber("FORGEDB_KILL_RUNS", FORGEDB_KILL_TEST_RUNS);
  const std::uint64_t seed = GetEnvNumber("FORGEDB_KILL_SEED", 1);
  const std::filesystem::path child = ChildPath();
  std::error_code error;
  ASSERT_TRUE(std::filesystem::exists(child, error)) << child.string();

  std::random_device random_device;
  const std::filesystem::path root = std::filesystem::temp_directory_path(error) /
                                     ("forgedb-kill-" + std::to_string(::GetCurrentProcessId()) +
                                      "-" + std::to_string(random_device()));
  ASSERT_TRUE(std::filesystem::create_directories(root, error)) << root.string();

  std::mt19937_64 rng(seed);
  std::filesystem::path db_dir;
  std::map<std::uint64_t, std::uint64_t> survived;  // run -> keys in the DB after its kill
  std::uint64_t acknowledged_total = 0;
  std::uint64_t killed_before_first_write = 0;
  std::uint64_t unrecorded_batches = 0;

  for (std::uint64_t run = 0; run < runs; ++run) {
    SCOPED_TRACE(testing::Message()
                 << "run " << run << ", seed " << seed << ", files in " << root.string());
    if (run % kRunsPerDatabase == 0) {
      db_dir = root / ("db-" + std::to_string(run / kRunsPerDatabase));
      survived.clear();
    }
    const std::filesystem::path side_file = root / ("acknowledged-" + std::to_string(run) + ".txt");

    // 1. Start the child.
    std::wstring command = Quoted(child) + L" " + Quoted(db_dir) + L" " + Quoted(side_file) + L" " +
                           std::to_wstring(run) + L" " + std::to_wstring(rng());
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION info{};
    ASSERT_NE(::CreateProcessW(nullptr, command.data(), nullptr, nullptr, FALSE, 0, nullptr,
                               nullptr, &startup, &info),
              FALSE)
        << "CreateProcessW failed: " << ::GetLastError();
    const UniqueHandle process(info.hProcess);
    const UniqueHandle thread(info.hThread);

    // 2. Kill it at a random moment: a quarter of the time early, while it
    //    starts and recovers; otherwise in the middle of its writes.
    const auto wait_ms = static_cast<DWORD>(rng() % 4 == 0 ? rng() % 30 : 30 + (rng() % 270));
    if (::WaitForSingleObject(process.Get(), wait_ms) == WAIT_OBJECT_0) {
      DWORD code = 0;
      ::GetExitCodeProcess(process.Get(), &code);
      FAIL() << "the child exited on its own with code " << code << " (see its stderr above)";
    }
    ASSERT_NE(::TerminateProcess(process.Get(), 99), FALSE)
        << "TerminateProcess failed: " << ::GetLastError();
    ASSERT_EQ(::WaitForSingleObject(process.Get(), INFINITE), WAIT_OBJECT_0);

    // 3. Reopen and check.
    const std::vector<std::string> lines = CompleteLines(side_file);
    for (std::size_t i = 0; i < lines.size(); ++i) {
      ASSERT_EQ(lines[i], KeyFor(run, i)) << "side file line " << i;
    }
    Result<std::unique_ptr<DB>> opened = OpenAfterKill(db_dir);
    ASSERT_TRUE(opened.ok()) << "reopening after the kill: " << opened.status().ToString();
    const std::unique_ptr<DB> db = std::move(opened).value();
    std::string problem;
    const std::map<std::uint64_t, std::vector<std::uint64_t>> keys = KeysByRun(*db, &problem);
    ASSERT_EQ(problem, "");

    const auto mine = keys.find(run);
    const std::vector<std::uint64_t> none;
    const std::vector<std::uint64_t>& numbers = mine == keys.end() ? none : mine->second;
    for (std::size_t i = 0; i < numbers.size(); ++i) {
      ASSERT_EQ(numbers[i], i) << "keys of this run are not a prefix (a hole)";
    }
    ASSERT_GE(numbers.size(), lines.size()) << "an acknowledged synced write was lost";
    ASSERT_LE(numbers.size(), lines.size() + kMaxBatchKeys)
        << "more keys than acknowledged plus one batch";
    for (const auto& [earlier, count] : survived) {
      const auto found = keys.find(earlier);
      ASSERT_EQ(found == keys.end() ? 0U : found->second.size(), count)
          << "keys of run " << earlier << " changed";
    }
    survived[run] = numbers.size();
    acknowledged_total += lines.size();
    killed_before_first_write += numbers.empty() ? 1U : 0U;
    unrecorded_batches += numbers.size() > lines.size() ? 1U : 0U;
  }

  std::printf(  // NOLINT(cppcoreguidelines-pro-type-vararg)
      "[ stats    ] %llu kills: %llu acknowledged keys checked, %llu kills before the first "
      "write, %llu kills between a write and its side-file line\n",
      static_cast<unsigned long long>(runs), static_cast<unsigned long long>(acknowledged_total),
      static_cast<unsigned long long>(killed_before_first_write),
      static_cast<unsigned long long>(unrecorded_batches));
  if (runs >= 20) {
    EXPECT_GT(acknowledged_total, 0U) << "the children wrote nothing before being killed";
  }
  if (!HasFailure()) {
    std::filesystem::remove_all(root, error);
  }
}

}  // namespace
}  // namespace forgedb
