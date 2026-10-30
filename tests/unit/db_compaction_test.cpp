// Leveled compaction (TODO 4.3; docs/adr/0008): choosing inputs, the drop
// rules with and without snapshots, when input tables are deleted, write
// stalls, errors, and shutdown during a compaction.
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>

#include <gtest/gtest.h>

#include <map>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include "forgedb/db.h"
#include "forgedb/iterator.h"
#include "forgedb/options.h"
#include "forgedb/result.h"
#include "forgedb/snapshot.h"
#include "forgedb/status.h"

#include "db/compaction.h"
#include "db/db_impl.h"
#include "db/filename.h"
#include "db/internal_key.h"
#include "db/version_edit.h"
#include "db/version_set.h"
#include "env/file_system.h"
#include "env/mem_file_system.h"
#include "support/fault_injection_file_system.h"
#include "support/gated_file_system.h"
#include "table/table.h"

namespace forgedb {
namespace {

std::string InternalKey(std::string_view user_key, SequenceNumber sequence,
                        ValueType type = ValueType::kValue) {
  std::string key;
  AppendInternalKey(key, {.user_key = user_key, .sequence = sequence, .type = type});
  return key;
}

FileMetaData File(std::uint64_t number, std::uint64_t size, std::string_view first,
                  std::string_view last) {
  return {.number = number,
          .file_size = size,
          .smallest = InternalKey(first, 100),
          .largest = InternalKey(last, 1)};
}

std::vector<std::uint64_t> Numbers(const std::vector<FileMetaData>& files) {
  std::vector<std::uint64_t> numbers;
  numbers.reserve(files.size());
  for (const FileMetaData& file : files) {
    numbers.push_back(file.number);
  }
  return numbers;
}

// --- Choosing what to compact (db/compaction.h) --------------------------------------------

// The compaction, or (after reporting a failure) an empty one.
Compaction Must(std::optional<Compaction> compaction) {
  if (!compaction) {
    ADD_FAILURE() << "no compaction chosen";
    Compaction empty;
    empty.version = std::make_shared<const Version>();
    return empty;
  }
  return std::move(compaction).value_or(Compaction{});
}

Options SmallLevels() {
  Options options;
  options.level0_compaction_trigger = 4;
  options.max_bytes_for_level_base = 1000;
  return options;
}

TEST(CompactionPickTest, LevelTargetsGrowTenfold) {
  const Options options = SmallLevels();
  EXPECT_EQ(MaxBytesForLevel(options, 1), 1000U);
  EXPECT_EQ(MaxBytesForLevel(options, 2), 10000U);
  EXPECT_EQ(MaxBytesForLevel(options, 5), 10000000U);
}

TEST(CompactionPickTest, ScoresAndNothingToDo) {
  const Options options = SmallLevels();
  Version version;
  CompactionPointers pointers;
  EXPECT_FALSE(NeedsCompaction(version, options));
  EXPECT_FALSE(PickCompaction(std::make_shared<const Version>(version), options, pointers));

  version.files[0] = {File(1, 10, "a", "b"), File(2, 10, "a", "b"), File(3, 10, "c", "d")};
  version.files[2] = {File(4, 9000, "a", "z")};
  EXPECT_DOUBLE_EQ(CompactionScore(version, options, 0), 0.75);
  EXPECT_DOUBLE_EQ(CompactionScore(version, options, 2), 0.9);
  EXPECT_DOUBLE_EQ(CompactionScore(version, options, 6), 0.0) << "the last level never";
  EXPECT_FALSE(NeedsCompaction(version, options));

  version.files[6] = {File(5, 1U << 30, "a", "z")};
  EXPECT_FALSE(NeedsCompaction(version, options)) << "however big the last level gets";
}

TEST(CompactionPickTest, LevelZeroTakesEveryTableAndTheOverlappingNextLevel) {
  const Options options = SmallLevels();
  Version version;
  version.files[0] = {File(14, 10, "m", "p"), File(13, 10, "c", "f"), File(12, 10, "a", "d"),
                      File(11, 10, "e", "g")};
  version.files[1] = {File(5, 10, "a", "b"), File(6, 10, "c", "d"), File(7, 10, "h", "k"),
                      File(8, 10, "o", "q"), File(9, 10, "r", "s")};
  CompactionPointers pointers;
  const Compaction compaction =
      Must(PickCompaction(std::make_shared<const Version>(version), options, pointers));
  EXPECT_EQ(compaction.level, 0);
  EXPECT_EQ(Numbers(compaction.inputs[0]), (std::vector<std::uint64_t>{14, 13, 12, 11}));
  // a..p overlaps 5, 6, 7 ("h".."k" lies inside it) and 8; not 9.
  EXPECT_EQ(Numbers(compaction.inputs[1]), (std::vector<std::uint64_t>{5, 6, 7, 8}));
  EXPECT_FALSE(compaction.IsTrivialMove());
}

TEST(CompactionPickTest, DeeperLevelsGoRoundRobin) {
  const Options options = SmallLevels();
  Version version;
  version.files[1] = {File(1, 600, "a", "c"), File(2, 600, "d", "f"), File(3, 600, "g", "i")};
  version.files[2] = {File(4, 10, "b", "e")};
  const auto shared = std::make_shared<const Version>(version);
  CompactionPointers pointers;
  std::vector<std::uint64_t> picked;
  for (int i = 0; i < 4; ++i) {
    const Compaction compaction = Must(PickCompaction(shared, options, pointers));
    EXPECT_EQ(compaction.level, 1);
    ASSERT_EQ(compaction.inputs[0].size(), 1U);
    picked.push_back(compaction.inputs[0].front().number);
  }
  EXPECT_EQ(picked, (std::vector<std::uint64_t>{1, 2, 3, 1}));

  pointers = {};
  const Compaction first = Must(PickCompaction(shared, options, pointers));
  EXPECT_EQ(Numbers(first.inputs[1]), (std::vector<std::uint64_t>{4}));
  static_cast<void>(PickCompaction(shared, options, pointers));
  const Compaction third = Must(PickCompaction(shared, options, pointers));
  EXPECT_TRUE(third.IsTrivialMove()) << "g..i overlaps nothing in level 2";
}

TEST(CompactionPickTest, TheHighestScoreWins) {
  const Options options = SmallLevels();
  Version version;
  version.files[0] = {File(1, 10, "a", "b"), File(2, 10, "a", "b"), File(3, 10, "a", "b"),
                      File(4, 10, "a", "b")};      // score 1
  version.files[3] = {File(5, 300000, "a", "z")};  // score 3
  CompactionPointers pointers;
  const Compaction compaction =
      Must(PickCompaction(std::make_shared<const Version>(version), options, pointers));
  EXPECT_EQ(compaction.level, 3);
}

TEST(CompactionPickTest, BaseLevelForAKey) {
  Version version;
  version.files[1] = {File(1, 10, "a", "z")};
  version.files[2] = {File(2, 10, "a", "c")};
  version.files[4] = {File(3, 10, "m", "p")};
  CompactionPointers pointers;
  const Compaction compaction =
      Must(CompactionForLevel(std::make_shared<const Version>(version), 1, pointers));
  // Output level 2; levels 3 to 6 are below it.
  EXPECT_TRUE(compaction.IsBaseLevelForKey("b")) << "level 2 is the output level itself";
  EXPECT_FALSE(compaction.IsBaseLevelForKey("n")) << "level 4 holds it";
  EXPECT_TRUE(compaction.IsBaseLevelForKey("x"));
}

// --- The DB --------------------------------------------------------------------------------

std::string Key(int i) {
  const std::string digits = std::to_string(i);
  return "key" + std::string(digits.size() < 6 ? 6 - digits.size() : 0, '0') + digits;
}

std::string Get(DB& db, std::string_view key, std::shared_ptr<const Snapshot> snapshot = nullptr) {
  Result<std::string> value = db.Get({.snapshot = std::move(snapshot)}, key);
  if (value.ok()) {
    return std::move(value).value();
  }
  return value.status().code() == Status::Code::kNotFound ? "NOT_FOUND"
                                                          : "ERROR: " + value.status().ToString();
}

using State = std::map<std::string, std::string>;

State Scan(DB& db, std::shared_ptr<const Snapshot> snapshot = nullptr) {
  State state;
  const std::unique_ptr<Iterator> it = db.NewIterator({.snapshot = std::move(snapshot)});
  for (it->SeekToFirst(); it->Valid(); it->Next()) {
    state.emplace(it->key(), it->value());
  }
  EXPECT_TRUE(it->status().ok()) << it->status().ToString();
  return state;
}

DBImpl& Impl(DB& db) { return dynamic_cast<DBImpl&>(db); }

Options NoAutomaticCompaction() {
  Options options;
  options.memtable_bytes = std::size_t{16} << 10;
  options.level0_compaction_trigger = 1 << 30;
  options.level0_slowdown_writes_trigger = 1 << 30;
  options.level0_stop_writes_trigger = 1 << 30;
  return options;
}

// Tiny levels: data reaches deep levels after a few thousand writes.
Options TinyLevels() {
  Options options;
  options.memtable_bytes = std::size_t{8} << 10;
  options.level0_compaction_trigger = 2;
  options.max_bytes_for_level_base = std::size_t{24} << 10;
  options.target_file_size = std::size_t{4} << 10;
  options.block_size = 512;
  return options;
}

class CompactionTest : public testing::Test {
 protected:
  std::unique_ptr<DB> Open(const Options& options) {
    Result<std::unique_ptr<DB>> db = OpenDB(options, "db", fs_);
    EXPECT_TRUE(db.ok()) << db.status().ToString();
    return db.ok() ? std::move(db).value() : nullptr;
  }

  // Every entry of every table the version lists, as "key@sequence put|del".
  std::vector<std::string> RawEntries(DBImpl& db) {
    std::vector<std::string> entries;
    const std::shared_ptr<const Version> version = db.CurrentVersion();
    for (const std::vector<FileMetaData>& level : version->files) {
      for (const FileMetaData& file : level) {
        Result<std::unique_ptr<Table>> table = Table::Open(
            std::move(fs_->NewRandomAccessFile(TableFileName("db", file.number))).value(),
            file.file_size, nullptr);
        if (!table.ok()) {
          ADD_FAILURE() << table.status().ToString();
          continue;
        }
        const std::unique_ptr<Iterator> it = (*table)->NewIterator();
        for (it->SeekToFirst(); it->Valid(); it->Next()) {
          const std::optional<ParsedInternalKey> key = ParseInternalKey(it->key());
          if (!key) {
            ADD_FAILURE() << "malformed key in table " << file.number;
            continue;
          }
          entries.push_back(std::string(key->user_key) + "@" + std::to_string(key->sequence) +
                            (key->type == ValueType::kDeletion ? " del" : " put"));
        }
      }
    }
    std::ranges::sort(entries);
    return entries;
  }

  std::vector<std::string> Tables() {
    std::vector<std::string> names;
    for (const std::string& child : std::move(fs_->GetChildren("db")).value()) {
      if (child.ends_with(".sst")) {
        names.push_back(child);
      }
    }
    std::ranges::sort(names);
    return names;
  }

  std::shared_ptr<MemFileSystem> fs_ = std::make_shared<MemFileSystem>();
};

TEST_F(CompactionTest, AutomaticCompactionsKeepEverythingReadable) {
  State expected;
  {
    const std::unique_ptr<DB> db = Open(TinyLevels());
    for (int round = 0; round < 6; ++round) {
      for (int i = 0; i < 1000; ++i) {
        const int k = ((i * 7) + (round * 13)) % 1000;
        if ((k + round) % 5 == 0) {
          ASSERT_TRUE(db->Delete({}, Key(k)).ok());
          expected.erase(Key(k));
        } else {
          const std::string value = "v" + std::to_string(round) + std::string(k % 40, 'x');
          ASSERT_TRUE(db->Put({}, Key(k), value).ok());
          expected[Key(k)] = value;
        }
      }
    }
    Impl(*db).WaitForBackgroundWorkForTesting();
    EXPECT_GT(Impl(*db).CompactionsForTesting(), 5U);
    const std::shared_ptr<const Version> version = Impl(*db).CurrentVersion();
    int deepest = 0;
    for (int level = 0; level < kNumLevels; ++level) {
      if (!version->files.at(static_cast<std::size_t>(level)).empty()) {
        deepest = level;
      }
    }
    EXPECT_GE(deepest, 2);
    EXPECT_FALSE(NeedsCompaction(*version, TinyLevels()));
    for (int i = 0; i < 1000; ++i) {
      const auto it = expected.find(Key(i));
      ASSERT_EQ(Get(*db, Key(i)), it == expected.end() ? "NOT_FOUND" : it->second) << i;
    }
    EXPECT_EQ(Scan(*db), expected);
  }
  const std::unique_ptr<DB> db = Open(TinyLevels());
  EXPECT_EQ(Scan(*db), expected);
}

TEST_F(CompactionTest, ACompactionDropsWhatNoReaderCanSee) {
  const std::unique_ptr<DB> db = Open(NoAutomaticCompaction());
  ASSERT_TRUE(db->Put({}, "a", "1").ok());  // 1
  ASSERT_TRUE(db->Put({}, "a", "2").ok());  // 2
  ASSERT_TRUE(db->Put({}, "b", "1").ok());  // 3
  ASSERT_TRUE(db->Put({}, "c", "1").ok());  // 4
  ASSERT_TRUE(Impl(*db).FlushForTesting().ok());
  ASSERT_TRUE(db->Delete({}, "b").ok());    // 5
  ASSERT_TRUE(db->Put({}, "a", "3").ok());  // 6
  ASSERT_TRUE(Impl(*db).FlushForTesting().ok());
  EXPECT_EQ(RawEntries(Impl(*db)), (std::vector<std::string>{"a@1 put", "a@2 put", "a@6 put",
                                                             "b@3 put", "b@5 del", "c@4 put"}));

  ASSERT_TRUE(Impl(*db).CompactAllForTesting().ok());
  EXPECT_EQ(RawEntries(Impl(*db)), (std::vector<std::string>{"a@6 put", "c@4 put"}))
      << "only the newest versions; the deletion and what it deleted are gone";
  EXPECT_EQ(Impl(*db).CurrentVersion()->files[6].size(), 1U) << "all in the last level";
  EXPECT_EQ(Get(*db, "a"), "3");
  EXPECT_EQ(Get(*db, "b"), "NOT_FOUND");
}

// TODO 4.5: after a full compaction the database equals the model, and the
// tables hold exactly one entry per live key: every older version and every
// deletion is really gone.
TEST_F(CompactionTest, AFullCompactionLeavesExactlyTheModel) {
  const std::unique_ptr<DB> db = Open(TinyLevels());
  State model;
  std::uint64_t random = 12345;
  for (int i = 0; i < 6000; ++i) {
    random = (random * 6364136223846793005U) + 1442695040888963407U;
    const std::string key = Key(static_cast<int>((random >> 33U) % 700));
    if ((random >> 20U) % 4 == 0) {
      ASSERT_TRUE(db->Delete({}, key).ok());
      model.erase(key);
    } else {
      const std::string value = "v" + std::to_string(i) + std::string((random >> 40U) % 64, 'p');
      ASSERT_TRUE(db->Put({}, key, value).ok());
      model[key] = value;
    }
  }
  ASSERT_TRUE(Impl(*db).CompactAllForTesting().ok());
  EXPECT_EQ(Scan(*db), model);
  const std::vector<std::string> raw = RawEntries(Impl(*db));
  EXPECT_EQ(raw.size(), model.size()) << "one entry per live key";
  for (const std::string& entry : raw) {
    EXPECT_TRUE(entry.ends_with(" put")) << entry;
  }
  const std::shared_ptr<const Version> version = Impl(*db).CurrentVersion();
  for (int level = 0; level + 1 < kNumLevels; ++level) {
    EXPECT_TRUE(version->files.at(static_cast<std::size_t>(level)).empty()) << level;
  }
}

TEST_F(CompactionTest, ASnapshotKeepsTheVersionsItCanSee) {
  const std::unique_ptr<DB> db = Open(NoAutomaticCompaction());
  ASSERT_TRUE(db->Put({}, "a", "1").ok());                       // 1
  ASSERT_TRUE(db->Put({}, "b", "1").ok());                       // 2
  std::shared_ptr<const Snapshot> snapshot = db->GetSnapshot();  // at 2
  ASSERT_TRUE(db->Put({}, "a", "2").ok());                       // 3
  ASSERT_TRUE(db->Put({}, "a", "3").ok());                       // 4
  ASSERT_TRUE(db->Delete({}, "b").ok());                         // 5

  ASSERT_TRUE(Impl(*db).CompactAllForTesting().ok());
  // a@1 and b@2 are what the snapshot sees; b's deletion is newer than it.
  // a@3 is visible to no one either, but rule (a) looks only at the oldest
  // snapshot (ADR 0008): it goes once that snapshot is released.
  EXPECT_EQ(RawEntries(Impl(*db)),
            (std::vector<std::string>{"a@1 put", "a@3 put", "a@4 put", "b@2 put", "b@5 del"}));
  EXPECT_EQ(Get(*db, "a", snapshot), "1");
  EXPECT_EQ(Get(*db, "b", snapshot), "1");
  EXPECT_EQ(Get(*db, "a"), "3");
  EXPECT_EQ(Get(*db, "b"), "NOT_FOUND");

  // Released: the next compaction over these keys drops them. (The last
  // level is compacted only when data from above overlaps it.)
  snapshot.reset();
  ASSERT_TRUE(db->Put({}, "a0", "new").ok());
  ASSERT_TRUE(Impl(*db).CompactAllForTesting().ok());
  EXPECT_EQ(RawEntries(Impl(*db)), (std::vector<std::string>{"a0@6 put", "a@4 put"}));
}

TEST_F(CompactionTest, ADeletionStaysWhileAnOlderVersionIsDeeper) {
  const std::unique_ptr<DB> db = Open(NoAutomaticCompaction());
  ASSERT_TRUE(db->Put({}, "k", "old").ok());  // 1
  ASSERT_TRUE(Impl(*db).CompactAllForTesting().ok());
  ASSERT_EQ(Impl(*db).CurrentVersion()->files[6].size(), 1U);

  ASSERT_TRUE(db->Delete({}, "k").ok());  // 2
  ASSERT_TRUE(Impl(*db).FlushForTesting().ok());
  ASSERT_TRUE(Impl(*db).CompactLevelForTesting(0).ok());  // into level 1
  EXPECT_EQ(Impl(*db).CurrentVersion()->files[1].size(), 1U);
  EXPECT_EQ(RawEntries(Impl(*db)), (std::vector<std::string>{"k@1 put", "k@2 del"}))
      << "dropping the deletion would bring k@1 back";
  EXPECT_EQ(Get(*db, "k"), "NOT_FOUND");

  ASSERT_TRUE(Impl(*db).CompactAllForTesting().ok());
  EXPECT_TRUE(RawEntries(Impl(*db)).empty()) << "met in the last level: both gone";
  EXPECT_EQ(Get(*db, "k"), "NOT_FOUND");
}

TEST_F(CompactionTest, InputTablesAreDeletedAfterTheNextRecordAndWhenNoReadUsesThem) {
  const std::unique_ptr<DB> db = Open(NoAutomaticCompaction());
  for (int i = 0; i < 2; ++i) {
    ASSERT_TRUE(db->Put({}, Key(i), "v").ok());
    ASSERT_TRUE(Impl(*db).FlushForTesting().ok());
  }
  const std::vector<std::string> inputs = Tables();
  ASSERT_EQ(inputs.size(), 2U);
  std::unique_ptr<Iterator> reader = db->NewIterator({});  // holds the version with the inputs

  ASSERT_TRUE(Impl(*db).CompactLevelForTesting(0).ok());
  EXPECT_EQ(Impl(*db).CurrentVersion()->files[1].size(), 1U);
  EXPECT_EQ(Tables().size(), 3U) << "the edit that removed the inputs is the MANIFEST's last";

  ASSERT_TRUE(db->Put({}, Key(2), "v").ok());
  ASSERT_TRUE(Impl(*db).FlushForTesting().ok());  // another record
  EXPECT_EQ(Tables().size(), 4U) << "the reader's version still lists the inputs";
  int count = 0;
  for (reader->SeekToFirst(); reader->Valid(); reader->Next()) {
    ++count;
  }
  EXPECT_EQ(count, 2) << "and it still reads them";
  EXPECT_TRUE(reader->status().ok());

  reader.reset();
  ASSERT_TRUE(db->Put({}, Key(3), "v").ok());
  ASSERT_TRUE(Impl(*db).FlushForTesting().ok());  // the next background job deletes them
  for (const std::string& input : inputs) {
    EXPECT_FALSE(*fs_->FileExists(std::filesystem::path("db") / input)) << input;
  }
}

// ADR 0008: if the compaction's edit, the MANIFEST's last record, is damaged
// later, recovery ignores it and needs the input tables, which are kept.
TEST_F(CompactionTest, ADamagedLastCompactionRecordLosesNothing) {
  {
    const std::unique_ptr<DB> db = Open(NoAutomaticCompaction());
    for (int i = 0; i < 2; ++i) {
      for (int k = 0; k < 50; ++k) {
        ASSERT_TRUE(db->Put({}, Key(k), "v" + std::to_string(i)).ok());
      }
      ASSERT_TRUE(Impl(*db).FlushForTesting().ok());
    }
    ASSERT_TRUE(Impl(*db).CompactLevelForTesting(0).ok());
  }
  // Cut the MANIFEST's last record (the compaction edit) short.
  std::filesystem::path manifest;
  for (const std::string& child : std::move(fs_->GetChildren("db")).value()) {
    if (child.starts_with("MANIFEST-")) {
      manifest = std::filesystem::path("db") / child;
    }
  }
  const std::uint64_t size = std::move(fs_->GetFileSize(manifest)).value();
  std::string contents(size, '\0');
  ASSERT_EQ(
      std::move(
          std::move(fs_->NewRandomAccessFile(manifest)).value()->Read(0, std::span<char>(contents)))
          .value(),
      size);
  {
    const std::unique_ptr<WritableFile> file = std::move(fs_->NewWritableFile(manifest)).value();
    ASSERT_TRUE(file->Append(std::string_view(contents).substr(0, size - 3)).ok());
    ASSERT_TRUE(file->Close().ok());
  }
  const std::unique_ptr<DB> db = Open(NoAutomaticCompaction());
  ASSERT_NE(db, nullptr);
  EXPECT_TRUE(Impl(*db).GetRecoveryInfo().manifest.torn_tail.has_value());
  for (int k = 0; k < 50; ++k) {
    ASSERT_EQ(Get(*db, Key(k)), "v1") << k;
  }
}

TEST_F(CompactionTest, ASnapshotSeesAFrozenViewThroughCompactions) {
  const std::unique_ptr<DB> db = Open(TinyLevels());
  for (int i = 0; i < 500; ++i) {
    ASSERT_TRUE(db->Put({}, Key(i), "old" + std::to_string(i)).ok());
  }
  const std::shared_ptr<const Snapshot> snapshot = db->GetSnapshot();
  const State frozen = Scan(*db);
  for (int round = 0; round < 8; ++round) {
    for (int i = 0; i < 500; ++i) {
      if (i % 4 == round % 4) {
        ASSERT_TRUE(db->Delete({}, Key(i)).ok());
      } else {
        ASSERT_TRUE(db->Put({}, Key(i), "new" + std::to_string(round)).ok());
      }
    }
  }
  Impl(*db).WaitForBackgroundWorkForTesting();
  EXPECT_GT(Impl(*db).CompactionsForTesting(), 5U);
  EXPECT_EQ(Scan(*db, snapshot), frozen);
  for (int i = 0; i < 500; ++i) {
    ASSERT_EQ(Get(*db, Key(i), snapshot), "old" + std::to_string(i)) << i;
  }
}

// --- Write stalls ------------------------------------------------------------------------------

// Puts keys from `first` on a thread until `count` are written or one fails.
class Writer {
 public:
  Writer(DB* db, int first, int count)
      : thread_([this, db, first, count] {
          for (int i = first; i < first + count; ++i) {
            if (!db->Put({}, Key(i), std::string(200, 'w')).ok()) {
              failed_.store(true);
              break;
            }
            written_.fetch_add(1);
          }
        }) {}
  ~Writer() { Join(); }
  Writer(const Writer&) = delete;
  Writer& operator=(const Writer&) = delete;
  Writer(Writer&&) = delete;
  Writer& operator=(Writer&&) = delete;

  // Waits until no write has finished for 300 ms: the writer is held up.
  void WaitUntilStuck() {
    int last = -1;
    while (written_.load() != last) {
      last = written_.load();
      std::this_thread::sleep_for(std::chrono::milliseconds(300));
    }
  }
  void Join() {
    if (thread_.joinable()) {
      thread_.join();
    }
  }
  [[nodiscard]] int Written() const { return written_.load(); }
  [[nodiscard]] bool Failed() const { return failed_.load(); }

 private:
  std::atomic<int> written_{0};
  std::atomic<bool> failed_{false};
  std::thread thread_;
};

TEST_F(CompactionTest, WritesSlowDownThenStopWhileLevelZeroIsFull) {
  Options options;
  options.memtable_bytes = std::size_t{8} << 10;
  options.level0_compaction_trigger = 2;
  options.level0_slowdown_writes_trigger = 3;
  options.level0_stop_writes_trigger = 4;
  const std::unique_ptr<DB> db = Open(options);
  Impl(*db).SetCompactionsPausedForTesting(true);

  Writer writer(db.get(), 0, 2000);
  writer.WaitUntilStuck();
  ASSERT_LT(writer.Written(), 2000) << "it cannot finish while compactions are paused";
  EXPECT_EQ(Impl(*db).CurrentVersion()->files[0].size(), 4U) << "the stop trigger";
  EXPECT_GT(Impl(*db).GetStats().level0_slowdown.count, 0U) << "delayed from 3 tables on";
  EXPECT_EQ(Impl(*db).GetStats().level0_stop.count, 0U) << "still waiting: counted when it ends";

  Impl(*db).SetCompactionsPausedForTesting(false);
  writer.Join();
  EXPECT_FALSE(writer.Failed());
  EXPECT_EQ(writer.Written(), 2000);
  EXPECT_GE(Impl(*db).GetStats().level0_stop.count, 1U);
  EXPECT_GT(Impl(*db).CompactionsForTesting(), 0U);
  for (int i = 0; i < 2000; i += 97) {
    ASSERT_EQ(Get(*db, Key(i)), std::string(200, 'w')) << i;
  }
}

TEST(CompactionOptionsTest, TriggersMustBeOrdered) {
  const auto rejected = [](void (*change)(Options&)) {
    Options options;
    change(options);
    return OpenDB(options, "db", std::make_shared<MemFileSystem>()).status().code() ==
           Status::Code::kInvalidArgument;
  };
  EXPECT_TRUE(rejected([](Options& o) { o.level0_compaction_trigger = 0; }));
  EXPECT_TRUE(rejected([](Options& o) { o.level0_slowdown_writes_trigger = 3; }));
  EXPECT_TRUE(rejected([](Options& o) { o.level0_stop_writes_trigger = 7; }));
  EXPECT_TRUE(rejected([](Options& o) { o.max_bytes_for_level_base = 0; }));
  EXPECT_TRUE(rejected([](Options& o) { o.target_file_size = 0; }));
  EXPECT_FALSE(rejected([](Options& o) {
    o.level0_compaction_trigger = 5;
    o.level0_slowdown_writes_trigger = 5;
    o.level0_stop_writes_trigger = 5;
  }));
}

// --- Errors and shutdown -----------------------------------------------------------------------

// TODO 4.5: a compaction that fails stops writes with a sticky error; the
// data stays readable, and a reopen recovers it.
TEST(CompactionErrorTest, AFailedCompactionStopsWritesAndLosesNothing) {
  Options options;
  options.memtable_bytes = std::size_t{8} << 10;
  options.level0_compaction_trigger = 2;
  const auto fs = std::make_shared<FaultInjectionFileSystem>(std::make_shared<MemFileSystem>());
  int written = 0;
  {
    const std::unique_ptr<DB> db = std::move(OpenDB(options, "db", fs)).value();
    Impl(*db).SetSynchronousFlushForTesting(true);  // the file operations in a fixed order
    while (Impl(*db).FlushesForTesting() < 1) {
      ASSERT_TRUE(db->Put({}, Key(written), std::string(100, 'v')).ok());
      ++written;
    }
    // The second flush: the old log's sync, the table's, the MANIFEST's; then
    // the compaction it triggers: its output table's sync fails.
    fs->FailNth(FaultInjectionFileSystem::Op::kSync, 4);
    Status status;
    while (status.ok()) {
      status = db->Put({}, Key(written), std::string(100, 'v'));
      written += status.ok() ? 1 : 0;
    }
    EXPECT_EQ(Impl(*db).FlushesForTesting(), 2U);
    EXPECT_EQ(Impl(*db).CompactionsForTesting(), 0U);
    EXPECT_FALSE(db->Put({}, "later", "x").ok()) << "sticky";
    for (int i = 0; i < written; ++i) {
      ASSERT_EQ(Get(*db, Key(i)), std::string(100, 'v')) << "reads go on: " << i;
    }
  }
  fs->ClearFaults();
  const std::unique_ptr<DB> db = std::move(OpenDB(options, "db", fs)).value();
  for (int i = 0; i < written; ++i) {
    ASSERT_EQ(Get(*db, Key(i)), std::string(100, 'v')) << i;
  }
  Impl(*db).WaitForBackgroundWorkForTesting();
  EXPECT_GT(Impl(*db).CompactionsForTesting(), 0U) << "it compacts after the reopen";
}

// Found by the crash torture test under Wine (timing-dependent there): when
// syncing the compaction's MANIFEST edit fails, the edit may still be in the
// file (appended and flushed, only not synced), and a crash keeps it. Its
// output tables must then still exist: a failed edit is not undone by
// deleting what it lists (the next open deletes them if it is not there).
TEST(CompactionErrorTest, AFailedManifestSyncKeepsTheOutputs) {
  Options options;
  options.memtable_bytes = std::size_t{8} << 10;
  options.level0_compaction_trigger = 2;
  {
    const auto fs = std::make_shared<FaultInjectionFileSystem>(std::make_shared<MemFileSystem>());
    int written = 0;
    {
      const std::unique_ptr<DB> db = std::move(OpenDB(options, "db", fs)).value();
      Impl(*db).SetSynchronousFlushForTesting(true);
      while (Impl(*db).FlushesForTesting() < 1) {
        ASSERT_TRUE(db->Put({}, Key(written), std::string(100, 'v')).ok());
        ++written;
      }
      // The second flush syncs the old log, its table and the MANIFEST; the
      // compaction then syncs its output table, then its MANIFEST edit: fail
      // that one.
      fs->FailNth(FaultInjectionFileSystem::Op::kSync, 5);
      Status status;
      while (status.ok()) {
        status = db->Put({}, Key(written), std::string(100, 'v'));
        written += status.ok() ? 1 : 0;
      }
      ASSERT_EQ(Impl(*db).CompactionsForTesting(), 0U);
    }
    // A process crash: what was flushed to the OS survives, the edit too.
    ASSERT_TRUE(fs->SimulateCrash(FaultInjectionFileSystem::CrashKind::kProcessCrash).ok());
    fs->ClearFaults();
    const Result<std::unique_ptr<DB>> db = OpenDB(options, "db", fs);
    ASSERT_TRUE(db.ok()) << db.status().ToString();
    for (int i = 0; i < written; ++i) {
      ASSERT_EQ(Get(**db, Key(i)), std::string(100, 'v')) << i;
    }
  }
}

TEST(CompactionShutdownTest, ShutdownAbandonsARunningCompaction) {
  Options options;
  options.memtable_bytes = std::size_t{16} << 10;
  options.level0_compaction_trigger = 2;
  options.target_file_size = std::size_t{2} << 10;  // many outputs
  Gate gate;
  const auto base = std::make_shared<MemFileSystem>();
  const auto fs = std::make_shared<GatedFileSystem>(base, &gate);
  std::unique_ptr<DB> db = std::move(OpenDB(options, "db", fs)).value();
  gate.Close(/*free_passes=*/2);  // the two flushes pass; the compaction's first output waits
  int written = 0;
  while (Impl(*db).FlushesForTesting() < 2) {
    ASSERT_TRUE(db->Put({}, Key(written), std::string(20, 'v')).ok());
    ++written;
  }
  gate.WaitForArrival();
  const std::vector<std::string> listed = [&] {
    std::vector<std::string> names;
    for (const FileMetaData& file : Impl(*db).CurrentVersion()->files[0]) {
      names.push_back(TableFileName("", file.number).string());
    }
    std::ranges::sort(names);
    return names;
  }();

  std::atomic<bool> closed{false};
  std::thread closer([&] {
    db.reset();
    closed.store(true);
  });
  std::this_thread::sleep_for(std::chrono::milliseconds(100));
  EXPECT_FALSE(closed.load()) << "waits for the compaction to reach a check";
  gate.Open();
  closer.join();

  // Abandoned: its outputs are gone, the inputs are untouched.
  std::vector<std::string> tables;
  for (const std::string& child : std::move(base->GetChildren("db")).value()) {
    if (child.ends_with(".sst")) {
      tables.push_back(child);
    }
  }
  std::ranges::sort(tables);
  EXPECT_EQ(tables, listed);

  db = std::move(OpenDB(options, "db", fs)).value();
  for (int i = 0; i < written; ++i) {
    ASSERT_EQ(Get(*db, Key(i)), std::string(20, 'v')) << i;
  }
}

}  // namespace
}  // namespace forgedb
