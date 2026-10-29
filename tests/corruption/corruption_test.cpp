// Corruption suite (TODO 3.9): damage the files of a database the way a bad
// disk, a bad copy or a stray write would, and check that ForgeDB never
// answers wrongly and never crashes.
//
// Each trial builds a database on MemFileSystem (random writes and deletes,
// small memtables and levels so there are many flushes and compactions, a
// clean reopen in the middle, random table settings) and closes it cleanly.
// Then it damages one file of the type under test: bit flips, a run of
// random bytes, a run of zeros, or a truncation. Then it reopens the
// database. The rule:
//
//   - The open either fails with Corruption, or succeeds with the state after
//     the first S writes, for some S. S may be less than all of them only if
//     the damaged file is the newest log, and then recovery must say so (a
//     torn tail in RecoveryInfo); damage anywhere else loses nothing. The one
//     exception: a newest log truncated exactly at a record boundary ends
//     cleanly, like a log whose last writes never reached the disk, so
//     nothing can tell (README, known limitations).
//   - After a successful open, every Get and every iterator returns exactly
//     the state after S writes (compared with std::map), or stops with
//     Corruption. Corruption is allowed only if a table was damaged: a
//     table's blocks are read and checked only when a read needs them.
//   - Never another error, never a wrong value, a missing key or an extra
//     one; never a crash (the sanitizer builds also catch memory errors).
//   - A second open finds the same state.
//
// Run-time overrides:
//   FORGEDB_CORRUPTION_TRIALS=5000   trials per file type (default: tests/CMakeLists.txt)
//   FORGEDB_CORRUPTION_SEED=17       run only this trial (to reproduce a failure)
#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <filesystem>

#include <gtest/gtest.h>

#include <map>
#include <memory>
#include <optional>
#include <random>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "forgedb/db.h"
#include "forgedb/iterator.h"
#include "forgedb/options.h"
#include "forgedb/result.h"
#include "forgedb/status.h"

#include "db/db_impl.h"
#include "db/filename.h"
#include "env/file_system.h"
#include "env/mem_file_system.h"
#include "support/test_env.h"

#ifndef FORGEDB_CORRUPTION_TEST_TRIALS
#define FORGEDB_CORRUPTION_TEST_TRIALS 200
#endif

namespace forgedb {
namespace {

using State = std::map<std::string, std::string>;
using Rng = std::mt19937_64;

constexpr int kKeySpace = 300;
const std::filesystem::path kDb = "db";

std::uint64_t Uniform(Rng& rng, std::uint64_t low, std::uint64_t high) {
  return std::uniform_int_distribution<std::uint64_t>(low, high)(rng);
}

std::string Key(std::uint64_t i) {
  const std::string digits = std::to_string(i);
  return "key" + std::string(digits.size() < 5 ? 5 - digits.size() : 0, '0') + digits;
}

// --- The database and its history --------------------------------------------------

// One write: write i has sequence number i + 1.
struct Write {
  std::string key;
  std::optional<std::string> value;  // nullopt: a deletion
};

State StateAfter(const std::vector<Write>& history, std::uint64_t writes) {
  State state;
  for (std::uint64_t i = 0; i < writes; ++i) {
    const Write& write = history[i];
    if (write.value) {
      state[write.key] = *write.value;
    } else {
      state.erase(write.key);
    }
  }
  return state;
}

Options RandomOptions(Rng& rng) {
  Options options;
  options.memtable_bytes = Uniform(rng, 4, 16) << 10U;
  constexpr std::array<std::size_t, 3> kBlockSizes = {256, 1024, 4096};
  options.block_size = kBlockSizes.at(Uniform(rng, 0, 2));
  options.filter_bits_per_key = Uniform(rng, 0, 3) == 0 ? 0 : 10;
  options.block_cache_bytes = Uniform(rng, 0, 1) == 0 ? 0 : std::size_t{64} << 10U;
  // Small levels, so tables are spread over several levels (Phase 4.3).
  options.level0_compaction_trigger = static_cast<int>(Uniform(rng, 2, 4));
  options.max_bytes_for_level_base = Uniform(rng, 16, 64) << 10U;
  options.target_file_size = Uniform(rng, 4, 16) << 10U;
  return options;
}

// Writes the history to a new database: a clean reopen somewhere in the
// middle, then a clean close.
void BuildDatabase(Rng& rng, const std::shared_ptr<FileSystem>& fs, const Options& options,
                   std::vector<Write>& history) {
  const std::uint64_t writes = Uniform(rng, 200, 1000);
  const std::uint64_t reopen_at = Uniform(rng, 0, writes);
  // Flushes are made synchronous (a DBImpl test hook), so a seed builds the
  // same files every time.
  Result<std::unique_ptr<DB>> db = OpenDB(options, kDb, fs);
  ASSERT_TRUE(db.ok()) << db.status().ToString();
  dynamic_cast<DBImpl&>(**db).SetSynchronousFlushForTesting(true);
  for (std::uint64_t i = 0; i < writes; ++i) {
    if (i == reopen_at) {
      db = Status::Internal("closed");
      db = OpenDB(options, kDb, fs);
      ASSERT_TRUE(db.ok()) << db.status().ToString();
      dynamic_cast<DBImpl&>(**db).SetSynchronousFlushForTesting(true);
    }
    Write write{.key = Key(Uniform(rng, 0, kKeySpace - 1)), .value = std::nullopt};
    if (Uniform(rng, 0, 4) != 0) {
      write.value = "v" + std::to_string(i + 1) + "-" + std::string(Uniform(rng, 0, 200), 'x');
      ASSERT_TRUE((*db)->Put({}, write.key, *write.value).ok());
    } else {
      ASSERT_TRUE((*db)->Delete({}, write.key).ok());
    }
    history.push_back(std::move(write));
  }
}

// --- Damage --------------------------------------------------------------------------

std::string ReadAll(FileSystem& fs, const std::filesystem::path& path) {
  const std::uint64_t size = std::move(fs.GetFileSize(path)).value();
  const std::unique_ptr<RandomAccessFile> file = std::move(fs.NewRandomAccessFile(path)).value();
  std::string contents(size, '\0');
  EXPECT_EQ(std::move(file->Read(0, std::span<char>(contents))).value(), size);
  return contents;
}

void Overwrite(FileSystem& fs, const std::filesystem::path& path, std::string_view contents) {
  const std::unique_ptr<WritableFile> file = std::move(fs.NewWritableFile(path)).value();
  ASSERT_TRUE(file->Append(contents).ok());
  ASSERT_TRUE(file->Close().ok());
}

struct Damaged {
  std::string file;
  bool newest_log = false;
  bool truncated = false;
  std::string how;
};

// Damages one non-empty file of `type`; nullopt if there is none.
std::optional<Damaged> DamageOneFile(Rng& rng, FileSystem& fs, FileType type) {
  std::vector<std::pair<std::uint64_t, std::string>> candidates;
  std::uint64_t newest_log = 0;
  for (const std::string& child : std::move(fs.GetChildren(kDb)).value()) {
    const std::optional<ParsedFileName> parsed = ParseFileName(child);
    if (!parsed || parsed->type != type) {
      continue;
    }
    newest_log = std::max(newest_log, parsed->number);
    if (std::move(fs.GetFileSize(kDb / child)).value() > 0) {
      candidates.emplace_back(parsed->number, child);
    }
  }
  if (candidates.empty()) {
    return std::nullopt;
  }
  const auto& [number, name] = candidates[Uniform(rng, 0, candidates.size() - 1)];
  const std::filesystem::path path = kDb / name;
  std::string contents = ReadAll(fs, path);
  const std::uint64_t size = contents.size();
  Damaged damaged{.file = name,
                  .newest_log = type == FileType::kLog && number == newest_log,
                  .truncated = false,
                  .how = {}};

  switch (Uniform(rng, 0, 3)) {
    case 0: {
      const std::uint64_t flips = Uniform(rng, 1, 4);
      for (std::uint64_t i = 0; i < flips; ++i) {
        const std::uint64_t at = Uniform(rng, 0, size - 1);
        contents[at] = static_cast<char>(contents[at] ^ (1U << Uniform(rng, 0, 7)));
      }
      damaged.how = std::to_string(flips) + " bit flips";
      break;
    }
    case 1:
    case 2: {
      const bool zeros = Uniform(rng, 0, 1) == 0;
      const std::uint64_t at = Uniform(rng, 0, size - 1);
      const std::uint64_t length = std::min(Uniform(rng, 1, zeros ? 4096 : 64), size - at);
      for (std::uint64_t i = at; i < at + length; ++i) {
        contents[i] = zeros ? '\0' : static_cast<char>(Uniform(rng, 0, 255));
      }
      damaged.how = std::to_string(length) + (zeros ? " zero bytes" : " random bytes") + " at " +
                    std::to_string(at);
      break;
    }
    default: {
      const std::uint64_t length = Uniform(rng, 0, size - 1);
      contents.resize(length);
      damaged.truncated = true;
      damaged.how = "truncated from " + std::to_string(size) + " to " + std::to_string(length);
      break;
    }
  }
  Overwrite(fs, path, contents);
  return damaged;
}

// --- Checking reads ------------------------------------------------------------------

struct ReadCheck {
  std::uint64_t corruption_reads = 0;  // reads that stopped with Corruption
};

// Every Get, a full scan and some seeks against `expected`. A read may stop
// with Corruption only if `corruption_allowed`.
void CheckReads(Rng& rng, DB& db, const State& expected, bool corruption_allowed,
                ReadCheck& check) {
  const auto acceptable_error = [&](const Status& status) {
    if (corruption_allowed && status.code() == Status::Code::kCorruption) {
      ++check.corruption_reads;
      return true;
    }
    return false;
  };

  for (int i = 0; i < kKeySpace; ++i) {
    const std::string key = Key(static_cast<std::uint64_t>(i));
    const Result<std::string> value = db.Get(ReadOptions{}, key);
    const auto it = expected.find(key);
    if (value.ok()) {
      ASSERT_NE(it, expected.end()) << key << " was deleted or never written, read " << *value;
      ASSERT_EQ(*value, it->second) << key;
    } else if (value.status().code() == Status::Code::kNotFound) {
      ASSERT_EQ(it, expected.end()) << key << " is missing; expected " << it->second;
    } else {
      ASSERT_TRUE(acceptable_error(value.status())) << key << ": " << value.status().ToString();
    }
  }

  // A full scan: a prefix of the expected entries, all of them unless it
  // stopped with an error.
  {
    const std::unique_ptr<Iterator> it = db.NewIterator(ReadOptions{});
    auto want = expected.begin();
    for (it->SeekToFirst(); it->Valid(); it->Next(), ++want) {
      ASSERT_NE(want, expected.end()) << "extra key " << it->key();
      ASSERT_EQ(it->key(), want->first);
      ASSERT_EQ(it->value(), want->second) << want->first;
    }
    if (!it->status().ok()) {
      ASSERT_TRUE(acceptable_error(it->status())) << it->status().ToString();
    } else {
      ASSERT_EQ(want, expected.end()) << "the scan ended before " << want->first;
    }
  }

  for (int i = 0; i < 20; ++i) {
    const std::string target = Key(Uniform(rng, 0, kKeySpace));
    const std::unique_ptr<Iterator> it = db.NewIterator(ReadOptions{});
    auto want = expected.lower_bound(target);
    it->Seek(target);
    for (int n = 0; n < 5 && it->Valid(); ++n, it->Next(), ++want) {
      ASSERT_NE(want, expected.end()) << "extra key " << it->key() << " after seeking " << target;
      ASSERT_EQ(it->key(), want->first) << "after seeking " << target;
      ASSERT_EQ(it->value(), want->second);
    }
    if (!it->status().ok()) {
      ASSERT_TRUE(acceptable_error(it->status())) << it->status().ToString();
    } else if (!it->Valid()) {
      ASSERT_EQ(want, expected.end()) << "seek to " << target << " ended early";
    }
  }
}

// --- The trials ----------------------------------------------------------------------

struct Totals {
  std::uint64_t trials = 0;
  std::uint64_t refused = 0;    // the open failed with Corruption
  std::uint64_t intact = 0;     // opened with every write
  std::uint64_t prefix = 0;     // opened without the newest log's last writes (reported)
  std::uint64_t clean_cut = 0;  // ... cut at a record boundary: nothing to report
  std::uint64_t corruption_reads = 0;
};

void RunTrial(std::uint64_t seed, FileType type, Totals& totals) {
  Rng rng(seed);
  const auto fs = std::make_shared<MemFileSystem>();
  const Options options = RandomOptions(rng);
  std::vector<Write> history;
  BuildDatabase(rng, fs, options, history);
  if (testing::Test::HasFatalFailure()) {
    return;
  }

  const std::optional<Damaged> damaged = DamageOneFile(rng, *fs, type);
  if (!damaged) {
    return;  // no such file (e.g. no table yet): nothing to damage
  }
  SCOPED_TRACE(damaged->file + ": " + damaged->how);
  ++totals.trials;

  Result<std::unique_ptr<DB>> db = OpenDB(options, kDb, fs);
  if (!db.ok()) {
    ASSERT_EQ(db.status().code(), Status::Code::kCorruption) << db.status().ToString();
    ++totals.refused;
    return;
  }
  auto& impl = dynamic_cast<DBImpl&>(**db);
  const std::uint64_t recovered = impl.LastSequence();
  ASSERT_LE(recovered, history.size());
  if (recovered < history.size()) {
    ASSERT_TRUE(damaged->newest_log) << "lost writes " << recovered + 1 << " to " << history.size()
                                     << ", but the newest log was not damaged";
    if (impl.GetRecoveryInfo().torn_tails.empty()) {
      ASSERT_TRUE(damaged->truncated) << "lost writes were not reported";
      ++totals.clean_cut;
    } else {
      ++totals.prefix;
    }
  } else {
    ++totals.intact;
  }

  const State expected = StateAfter(history, recovered);
  const bool table_damaged = type == FileType::kTable;
  ReadCheck check;
  CheckReads(rng, **db, expected, table_damaged, check);
  if (testing::Test::HasFatalFailure()) {
    return;
  }
  totals.corruption_reads += check.corruption_reads;

  // Open again: the same state.
  db = Status::Internal("closed");
  db = OpenDB(options, kDb, fs);
  ASSERT_TRUE(db.ok()) << "second open: " << db.status().ToString();
  ASSERT_EQ(dynamic_cast<DBImpl&>(**db).LastSequence(), recovered);
  CheckReads(rng, **db, expected, table_damaged, check);
}

void RunTrials(FileType type, std::uint64_t base_seed, const char* name) {
  Totals totals;
  const std::optional<std::string> one_seed = GetEnv("FORGEDB_CORRUPTION_SEED");
  const std::uint64_t trials =
      GetEnvNumber("FORGEDB_CORRUPTION_TRIALS", FORGEDB_CORRUPTION_TEST_TRIALS);
  for (std::uint64_t t = 0; t < trials; ++t) {
    const std::uint64_t seed =
        one_seed ? GetEnvNumber("FORGEDB_CORRUPTION_SEED", 0) : base_seed + t;
    SCOPED_TRACE("FORGEDB_CORRUPTION_SEED=" + std::to_string(seed));
    RunTrial(seed, type, totals);
    if (testing::Test::HasFatalFailure()) {
      return;  // no summary: it would claim no wrong answers
    }
    if (one_seed) {
      break;
    }
  }
  std::printf(  // NOLINT(cppcoreguidelines-pro-type-vararg)
      "[ damage   ] %s: %llu trials: %llu opens refused (Corruption), %llu intact, "
      "%llu without the newest log's last writes (reported), %llu cut at a record boundary "
      "(not detectable); %llu reads stopped with Corruption; 0 wrong answers\n",
      name, static_cast<unsigned long long>(totals.trials),
      static_cast<unsigned long long>(totals.refused),
      static_cast<unsigned long long>(totals.intact),
      static_cast<unsigned long long>(totals.prefix),
      static_cast<unsigned long long>(totals.clean_cut),
      static_cast<unsigned long long>(totals.corruption_reads));
  EXPECT_GT(totals.trials, 0U);
}

TEST(CorruptionTest, DamagedTables) { RunTrials(FileType::kTable, 1'000'000, "tables"); }
TEST(CorruptionTest, DamagedLogs) { RunTrials(FileType::kLog, 2'000'000, "logs"); }
TEST(CorruptionTest, DamagedManifests) { RunTrials(FileType::kDescriptor, 3'000'000, "MANIFESTs"); }

}  // namespace
}  // namespace forgedb
