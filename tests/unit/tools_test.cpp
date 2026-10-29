// The command-line tools (TODO 3.8), run against an in-memory file system.
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <filesystem>

#include <gtest/gtest.h>

#include <memory>
#include <optional>
#include <span>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "forgedb/db.h"
#include "forgedb/options.h"
#include "forgedb/result.h"
#include "forgedb/status.h"

#include "db/db_impl.h"
#include "db/filename.h"
#include "db/internal_key.h"
#include "env/file_system.h"
#include "env/mem_file_system.h"
#include "table/table_builder.h"
#include "tools/cli.h"
#include "tools/sst_dump.h"
#include "tools/tool_util.h"

namespace forgedb::tools {
namespace {

// Whether `text` contains `part`; the failure message shows the text.
testing::AssertionResult Has(const std::string& text, std::string_view part) {
  if (text.find(part) != std::string::npos) {
    return testing::AssertionSuccess() << "'" << part << "' is in:\n" << text;
  }
  return testing::AssertionFailure() << "'" << part << "' is not in:\n" << text;
}

// --- tool_util -------------------------------------------------------------------

TEST(ToolUtilTest, EscapeBytes) {
  EXPECT_EQ(EscapeBytes("plain text 123"), "plain text 123");
  EXPECT_EQ(EscapeBytes(std::string_view("a\0b", 3)), "a\\x00b");
  EXPECT_EQ(EscapeBytes("tab\there\nnew\rret\\slash"), "tab\\there\\nnew\\rret\\\\slash");
  EXPECT_EQ(EscapeBytes("caf\xC3\xA9 \x7F\xFF"), "caf\\xC3\\xA9 \\x7F\\xFF");
  EXPECT_EQ(EscapeBytes("abcdef", 3), "abc...(6 bytes)");
  EXPECT_EQ(EscapeBytes("abc", 3), "abc");
  EXPECT_EQ(EscapeBytes(""), "");
}

TEST(ToolUtilTest, FormatInternalKey) {
  std::string key;
  AppendInternalKey(key, {.user_key = "k\x01", .sequence = 42, .type = ValueType::kValue});
  EXPECT_EQ(FormatInternalKey(key), "'k\\x01' @ 42 put");
  key.clear();
  AppendInternalKey(key, {.user_key = "", .sequence = 7, .type = ValueType::kDeletion});
  EXPECT_EQ(FormatInternalKey(key), "'' @ 7 del");
  EXPECT_EQ(FormatInternalKey("short"), "malformed key 'short'");
}

TEST(ToolUtilTest, Count) {
  EXPECT_EQ(Count(0, "table"), "0 tables");
  EXPECT_EQ(Count(1, "table"), "1 table");
  EXPECT_EQ(Count(2, "entry", "entries"), "2 entries");
  EXPECT_EQ(Count(1, "entry", "entries"), "1 entry");
}

TEST(ToolUtilTest, ParseUnsigned) {
  EXPECT_EQ(ParseUnsigned("0"), 0U);
  EXPECT_EQ(ParseUnsigned("12345"), 12345U);
  EXPECT_EQ(ParseUnsigned("18446744073709551615"), UINT64_MAX);
  for (const std::string_view bad : {"", "-1", "+1", " 1", "1 ", "1x", "18446744073709551616"}) {
    EXPECT_EQ(ParseUnsigned(bad), std::nullopt) << bad;
  }
}

TEST(ToolUtilTest, PathFromUtf8KeepsNonAsciiNames) {
  const std::filesystem::path path = PathFromUtf8("dir/caf\xC3\xA9.sst");
  EXPECT_TRUE(path.filename().u8string() == u8"caf\u00e9.sst");
}

// --- forgedb_sst_dump ---------------------------------------------------------------

std::string Key(int i) { return "key" + std::to_string(1000 + i); }

class SstDumpTest : public testing::Test {
 protected:
  // A table of `count` keys (every fifth a deletion) with small blocks, so it
  // has several data blocks.
  void BuildTable(int count, int filter_bits_per_key = 10) {
    const std::unique_ptr<WritableFile> file = std::move(fs_.NewWritableFile(path_)).value();
    TableBuilder builder({.block_size = 256, .filter_bits_per_key = filter_bits_per_key},
                         file.get());
    for (int i = 0; i < count; ++i) {
      std::string key;
      const ValueType type = i % 5 == 4 ? ValueType::kDeletion : ValueType::kValue;
      AppendInternalKey(
          key,
          {.user_key = Key(i), .sequence = static_cast<SequenceNumber>(100 + i), .type = type});
      builder.Add(key, type == ValueType::kValue ? "value-" + std::to_string(i) : "");
    }
    ASSERT_TRUE(builder.Finish().ok());
    ASSERT_TRUE(file->Close().ok());
  }

  std::string Contents() {
    const std::uint64_t size = std::move(fs_.GetFileSize(path_)).value();
    const std::unique_ptr<RandomAccessFile> file =
        std::move(fs_.NewRandomAccessFile(path_)).value();
    std::string contents(size, '\0');
    EXPECT_EQ(std::move(file->Read(0, std::span<char>(contents))).value(), size);
    return contents;
  }

  void Replace(std::string_view contents) {
    const std::unique_ptr<WritableFile> file = std::move(fs_.NewWritableFile(path_)).value();
    ASSERT_TRUE(file->Append(contents).ok());
    ASSERT_TRUE(file->Close().ok());
  }

  int Run(const std::vector<std::string>& args) {
    out_.str("");
    err_.str("");
    return RunSstDump(args, fs_, out_, err_);
  }

  MemFileSystem fs_;
  const std::filesystem::path path_{"000005.sst"};
  std::ostringstream out_;
  std::ostringstream err_;
};

TEST_F(SstDumpTest, SummarizesAnUndamagedTable) {
  BuildTable(100);
  ASSERT_EQ(Run({"000005.sst"}), 0) << out_.str() << err_.str();
  const std::string out = out_.str();
  EXPECT_TRUE(Has(out, "table 000005.sst: "));
  EXPECT_TRUE(Has(out, "footer: format version 1, index block at offset "));
  EXPECT_TRUE(Has(out, "entries: 100 (80 puts, 20 deletions), 100 distinct user keys\n"));
  EXPECT_TRUE(Has(out, "keys: 'key1000' @ 100 put .. 'key1099' @ 199 del\n"));
  EXPECT_TRUE(Has(out, "sequence numbers: 100 .. 199\n"));
  EXPECT_TRUE(Has(out, "probes, 10 bits per key, matches 100 of 100 keys\n"));
  EXPECT_TRUE(Has(out, "result: OK\n"));
  EXPECT_FALSE(Has(out, "DAMAGED"));
  EXPECT_FALSE(Has(out, "data blocks:"));  // only with --blocks or --entries
  EXPECT_EQ(err_.str(), "");
}

TEST_F(SstDumpTest, ListsBlocksAndEntries) {
  BuildTable(30);
  ASSERT_EQ(Run({"--blocks", "--entries", "000005.sst"}), 0) << out_.str();
  const std::string out = out_.str();
  EXPECT_TRUE(Has(out, "  block 1: offset 0, "));
  EXPECT_TRUE(Has(out, "    'key1000' @ 100 put = 'value-0'\n"));
  EXPECT_TRUE(Has(out, "    'key1004' @ 104 del\n"));
  EXPECT_TRUE(Has(out, "  block 2: "));
}

TEST_F(SstDumpTest, ATableWithoutAFilter) {
  BuildTable(10, /*filter_bits_per_key=*/0);
  ASSERT_EQ(Run({"000005.sst"}), 0) << out_.str();
  EXPECT_TRUE(Has(out_.str(), ", no filter\n"));
  EXPECT_FALSE(Has(out_.str(), "filter:"));
}

TEST_F(SstDumpTest, ADamagedDataBlockIsReportedAndTheRestIsStillRead) {
  BuildTable(100);
  ASSERT_EQ(Run({"--blocks", "000005.sst"}), 0);
  std::string contents = Contents();
  contents[10] = static_cast<char>(contents[10] ^ 1);  // inside the first data block
  Replace(contents);

  EXPECT_EQ(Run({"--blocks", "000005.sst"}), 1);
  const std::string out = out_.str();
  EXPECT_TRUE(Has(out, "DAMAGED: data block 1 (offset 0, "));
  EXPECT_TRUE(Has(out, "  block 2: ")) << "later blocks are still dumped";
  EXPECT_TRUE(Has(out, "result: DAMAGED (1 problem)\n"));
}

TEST_F(SstDumpTest, EveryByteOfTheFileIsChecked) {
  // A table small enough to damage every byte in turn: no flipped byte may
  // go unnoticed (the footer, every block and every trailer have a CRC).
  BuildTable(12);
  const std::string original = Contents();
  ASSERT_GT(original.size(), 200U);
  for (std::size_t i = 0; i < original.size(); ++i) {
    for (const int flip : {0x01, 0x80}) {
      std::string damaged = original;
      damaged[i] = static_cast<char>(static_cast<unsigned char>(damaged[i]) ^ flip);
      Replace(damaged);
      ASSERT_EQ(Run({"--entries", "000005.sst"}), 1)
          << "byte " << i << " of " << original.size() << " flipped with " << flip << "\n"
          << out_.str() << err_.str();
    }
  }
  for (std::size_t length = 0; length < original.size(); ++length) {
    Replace(std::string_view(original).substr(0, length));
    ASSERT_EQ(Run({"000005.sst"}), 1) << "truncated to " << length;
  }
}

TEST_F(SstDumpTest, UsageAndMissingFiles) {
  EXPECT_EQ(Run({}), 2);
  EXPECT_TRUE(Has(err_.str(), "usage: forgedb_sst_dump"));
  EXPECT_EQ(Run({"--bogus", "x.sst"}), 2);
  EXPECT_TRUE(Has(err_.str(), "unknown option --bogus"));
  EXPECT_EQ(Run({"missing.sst"}), 2);
  EXPECT_TRUE(Has(err_.str(), "missing.sst"));
}

TEST_F(SstDumpTest, SeveralFilesWorstExitCodeWins) {
  BuildTable(10);
  EXPECT_EQ(Run({"000005.sst", "000005.sst"}), 0);
  EXPECT_EQ(Run({"000005.sst", "missing.sst"}), 2);
}

// --- forgedb_cli --------------------------------------------------------------------

class CliTest : public testing::Test {
 protected:
  int Run(std::vector<std::string> args) {
    args.insert(args.begin(), "db");
    out_.str("");
    err_.str("");
    return RunCli(args, fs_, out_, err_);
  }

  std::shared_ptr<MemFileSystem> fs_ = std::make_shared<MemFileSystem>();
  std::ostringstream out_;
  std::ostringstream err_;
};

TEST_F(CliTest, PutGetDelete) {
  EXPECT_EQ(Run({"put", "apple", "red"}), 0) << err_.str();
  EXPECT_EQ(Run({"put", "banana", "yellow"}), 0);
  EXPECT_EQ(Run({"get", "apple"}), 0);
  EXPECT_EQ(out_.str(), "red\n");
  EXPECT_EQ(Run({"delete", "apple"}), 0);
  EXPECT_EQ(Run({"get", "apple"}), 1);
  EXPECT_EQ(out_.str(), "");
  EXPECT_TRUE(Has(err_.str(), "not found: apple"));
  EXPECT_EQ(Run({"get", "banana"}), 0);
  EXPECT_EQ(out_.str(), "yellow\n");
}

TEST_F(CliTest, ScanWithFromAndLimit) {
  for (const std::string key : {"a", "b", "c", "d"}) {
    ASSERT_EQ(Run({"put", key, "value of " + key}), 0);
  }
  ASSERT_EQ(Run({"put", "e\tx", "line\nbreak"}), 0);
  EXPECT_EQ(Run({"scan"}), 0);
  EXPECT_EQ(out_.str(),
            "a\tvalue of a\nb\tvalue of b\nc\tvalue of c\nd\tvalue of d\ne\\tx\tline\\nbreak\n");
  EXPECT_EQ(Run({"scan", "--from", "b", "--limit", "2"}), 0);
  EXPECT_EQ(out_.str(), "b\tvalue of b\nc\tvalue of c\n");
  EXPECT_EQ(Run({"scan", "--limit", "0"}), 0);
  EXPECT_EQ(out_.str(), "");
  EXPECT_EQ(Run({"scan", "--from", "zzz"}), 0);
  EXPECT_EQ(out_.str(), "");
}

TEST_F(CliTest, StatsShowsTablesAndRecovery) {
  ASSERT_EQ(Run({"put", "k1", "v1"}), 0);
  ASSERT_EQ(Run({"put", "k2", "v2"}), 0);  // this open turns k1's log into a table
  ASSERT_EQ(Run({"stats"}), 0) << err_.str();
  const std::string out = out_.str();
  EXPECT_TRUE(Has(out, "last sequence number: 2\n"));
  EXPECT_TRUE(Has(out, "tables: 2\n"));
  EXPECT_TRUE(Has(out, "level 0: 2 tables, "));
  EXPECT_TRUE(Has(out, "'k2' @ 2 put .. 'k2' @ 2 put\n"));
  EXPECT_TRUE(Has(out, "recovery (when this command opened the database):\n"));
  EXPECT_TRUE(Has(out, "  MANIFEST read: MANIFEST-"));
  EXPECT_TRUE(Has(out, "(1 record)\n")) << "k2's log was replayed";
  EXPECT_TRUE(Has(out, "  recovered writes: written to "));
  EXPECT_FALSE(Has(out, "WARNING"));
}

TEST_F(CliTest, StatsReportsATornLogTail) {
  ASSERT_EQ(Run({"put", "k1", "v1"}), 0);
  ASSERT_EQ(Run({"put", "k2", "v2"}), 0);
  // Cut the newest log's last record short, as a crash would.
  std::optional<std::uint64_t> newest;
  for (const std::string& child : std::move(fs_->GetChildren("db")).value()) {
    if (const std::optional<ParsedFileName> parsed = ParseFileName(child);
        parsed && parsed->type == FileType::kLog) {
      newest = std::max(newest.value_or(0), parsed->number);
    }
  }
  ASSERT_TRUE(newest.has_value());
  const std::filesystem::path log = LogFileName("db", newest.value_or(0));
  const std::uint64_t size = std::move(fs_->GetFileSize(log)).value();
  std::string contents(size, '\0');
  {
    const std::unique_ptr<RandomAccessFile> file = std::move(fs_->NewRandomAccessFile(log)).value();
    ASSERT_EQ(std::move(file->Read(0, std::span<char>(contents))).value(), size);
  }
  {
    const std::unique_ptr<WritableFile> file = std::move(fs_->NewWritableFile(log)).value();
    ASSERT_TRUE(file->Append(std::string_view(contents).substr(0, size - 2)).ok());
    ASSERT_TRUE(file->Close().ok());
  }
  ASSERT_EQ(Run({"stats"}), 0) << err_.str();
  EXPECT_TRUE(Has(out_.str(), ": ignored the tail from offset 0: "));
  EXPECT_EQ(Run({"get", "k2"}), 1) << "the torn write is gone";
  EXPECT_EQ(Run({"get", "k1"}), 0);
}

TEST_F(CliTest, ReadingCommandsNeedAnExistingDatabase) {
  for (const std::vector<std::string>& args :
       {std::vector<std::string>{"get", "k"}, {"scan"}, {"stats"}}) {
    EXPECT_EQ(Run(args), 2) << args[0];
    EXPECT_TRUE(Has(err_.str(), "no database at db")) << args[0];
  }
  EXPECT_FALSE(*fs_->FileExists("db")) << "nothing was created";
}

TEST_F(CliTest, UsageErrors) {
  EXPECT_EQ(RunCli({}, fs_, out_, err_), 2);
  EXPECT_TRUE(Has(err_.str(), "usage: forgedb_cli"));
  EXPECT_EQ(Run({"frobnicate"}), 2);
  EXPECT_TRUE(Has(err_.str(), "unknown command frobnicate"));
  EXPECT_EQ(Run({"put", "only-a-key"}), 2);
  EXPECT_TRUE(Has(err_.str(), "wrong number of arguments for put"));
  EXPECT_EQ(Run({"get"}), 2);
  EXPECT_EQ(Run({"stats", "extra"}), 2);
  ASSERT_EQ(Run({"put", "k", "v"}), 0);
  EXPECT_EQ(Run({"scan", "--limit"}), 2);
  EXPECT_TRUE(Has(err_.str(), "--limit needs a value"));
  EXPECT_EQ(Run({"scan", "--limit", "ten"}), 2);
  EXPECT_EQ(Run({"scan", "--sideways", "x"}), 2);
}

TEST_F(CliTest, ADatabaseThatIsAlreadyOpenIsBusy) {
  ASSERT_EQ(Run({"put", "k", "v"}), 0);
  const Result<std::unique_ptr<DB>> db = OpenDB(Options{}, "db", fs_);
  ASSERT_TRUE(db.ok());
  EXPECT_EQ(Run({"get", "k"}), 2);
  EXPECT_TRUE(Has(err_.str(), "Busy"));
}

}  // namespace
}  // namespace forgedb::tools
