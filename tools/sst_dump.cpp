#include "tools/sst_dump.h"

#include <algorithm>
#include <compare>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <ostream>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "forgedb/iterator.h"
#include "forgedb/result.h"
#include "forgedb/status.h"

#include "db/internal_key.h"
#include "env/file_system.h"
#include "table/block.h"
#include "table/block_format.h"
#include "table/bloom_filter.h"
#include "table/table_format.h"
#include "tools/tool_util.h"

namespace forgedb::tools {
namespace {

constexpr std::size_t kMaxValueShown = 100;

std::string Describe(const BlockHandle& handle) {
  return "offset " + std::to_string(handle.offset) + ", " + std::to_string(handle.size) + " bytes";
}

// Collects the problems found, printing each as it is found.
class Damage {
 public:
  explicit Damage(std::ostream* out) : out_(out) {}

  void Report(const std::string& problem) {
    *out_ << "  DAMAGED: " << problem << '\n';
    if (first_.ok()) {
      first_ = Status::Corruption(problem);
    }
    ++count_;
  }
  [[nodiscard]] const Status& FirstProblem() const noexcept { return first_; }
  [[nodiscard]] std::uint64_t Problems() const noexcept { return count_; }

 private:
  std::ostream* out_;
  Status first_;
  std::uint64_t count_ = 0;
};

// What the walk over the data blocks found.
struct Totals {
  std::uint64_t data_blocks = 0;
  std::uint64_t data_bytes = 0;  // contents and trailers
  std::uint64_t entries = 0;
  std::uint64_t puts = 0;
  std::uint64_t deletions = 0;
  std::uint64_t user_keys = 0;  // distinct
  std::uint64_t filter_misses = 0;
  std::optional<SequenceNumber> min_sequence;
  std::optional<SequenceNumber> max_sequence;
  std::string smallest;  // internal keys
  std::string largest;
};

class TableDumper {
 public:
  TableDumper(const RandomAccessFile& file, const SstDumpOptions& options, std::ostream& out)
      : file_(&file), options_(options), out_(&out), damage_(&out) {}

  Status Run(std::uint64_t file_size) {
    if (file_size < kTableFooterSize) {
      damage_.Report("file too short to be a table (" + std::to_string(file_size) + " bytes)");
      return damage_.FirstProblem();
    }
    std::string footer_bytes(kTableFooterSize, '\0');
    FORGEDB_ASSIGN_OR_RETURN(const std::size_t read, file_->Read(file_size - kTableFooterSize,
                                                                 std::span<char>(footer_bytes)));
    if (read != kTableFooterSize) {
      damage_.Report("short read of the footer");
      return damage_.FirstProblem();
    }
    const Result<TableFooter> footer = DecodeTableFooter(footer_bytes, file_size);
    if (!footer.ok()) {
      damage_.Report("footer: " + footer.status().ToString());
      return damage_.FirstProblem();
    }
    *out_ << "footer: format version " << kTableFormatVersion << ", index block at "
          << Describe(footer->index) << ", "
          << (footer->filter.size == 0 ? "no filter"
                                       : "filter block at " + Describe(footer->filter))
          << '\n';

    const std::uint64_t footer_start = file_size - kTableFooterSize;
    Result<std::string> index_contents = ReadBlockContents(*file_, footer->index, footer_start);
    if (!index_contents.ok()) {
      damage_.Report("index block: " + index_contents.status().ToString());
      return damage_.FirstProblem();
    }
    Result<Block> index = Block::Open(std::move(index_contents).value());
    if (!index.ok()) {
      damage_.Report("index block: " + index.status().ToString());
      return damage_.FirstProblem();
    }

    std::uint64_t data_end = footer->index.offset;
    if (footer->filter.size != 0) {
      data_end = footer->filter.offset;
      Result<std::string> filter = ReadBlockContents(*file_, footer->filter, footer->index.offset);
      if (filter.ok()) {
        filter_ = std::move(filter).value();
      } else {
        damage_.Report("filter block: " + filter.status().ToString());
      }
    }

    WalkDataBlocks(*index, data_end);
    PrintSummary();
    return damage_.FirstProblem();
  }

 private:
  void WalkDataBlocks(const Block& index, std::uint64_t data_end) {
    if (options_.blocks || options_.entries) {
      *out_ << "data blocks:\n";
    }
    std::uint64_t expected_offset = 0;
    std::uint64_t index_entries = 0;
    const std::unique_ptr<Iterator> it = index.NewIterator(&CompareInternalKeys);
    for (it->SeekToFirst(); it->Valid(); it->Next()) {
      ++index_entries;
      const std::optional<BlockHandle> handle = DecodeBlockHandle(it->value());
      if (!handle) {
        damage_.Report("index entry " + std::to_string(index_entries) + ": malformed block handle");
        continue;
      }
      if (handle->offset != expected_offset) {
        damage_.Report("data block " + std::to_string(index_entries) + " at offset " +
                       std::to_string(handle->offset) + ", expected " +
                       std::to_string(expected_offset));
      }
      expected_offset = handle->offset + handle->size + kBlockTrailerSize;
      DumpDataBlock(index_entries, *handle, it->key(), data_end);
    }
    if (!it->status().ok()) {
      damage_.Report("index block: " + it->status().ToString());
    }
    if (it->status().ok() && expected_offset != data_end) {
      damage_.Report("data blocks end at offset " + std::to_string(expected_offset) +
                     ", but the next block starts at " + std::to_string(data_end));
    }
    *out_ << "index: " << Count(index_entries, "entry", "entries") << '\n';
  }

  void DumpDataBlock(std::uint64_t number, const BlockHandle& handle, std::string_view index_key,
                     std::uint64_t data_end) {
    const std::string name = "data block " + std::to_string(number) + " (" + Describe(handle) + ")";
    Result<std::string> contents = ReadBlockContents(*file_, handle, data_end);
    if (!contents.ok()) {
      damage_.Report(name + ": " + contents.status().ToString());
      return;
    }
    Result<Block> block = Block::Open(std::move(contents).value());
    if (!block.ok()) {
      damage_.Report(name + ": " + block.status().ToString());
      return;
    }
    ++totals_.data_blocks;
    totals_.data_bytes += handle.size + kBlockTrailerSize;

    std::uint64_t entries = 0;
    std::string first_key;
    std::string last_key;
    const std::unique_ptr<Iterator> it = block->NewIterator(&CompareInternalKeys);
    for (it->SeekToFirst(); it->Valid(); it->Next()) {
      if (entries == 0) {
        first_key.assign(it->key());
      }
      last_key.assign(it->key());
      ++entries;
      CheckEntry(name, it->key(), it->value());
    }
    if (!it->status().ok()) {
      damage_.Report(name + ": " + it->status().ToString());
    } else if (entries == 0) {
      damage_.Report(name + ": no entries");
    } else if (last_key != index_key) {
      damage_.Report(name + ": its last key is not the index key " + FormatInternalKey(index_key));
    }
    if (options_.blocks) {
      *out_ << "  block " << number << ": " << Describe(handle) << ", "
            << Count(entries, "entry", "entries");
      if (entries > 0) {
        *out_ << ", " << FormatInternalKey(first_key) << " .. " << FormatInternalKey(last_key);
      }
      *out_ << '\n';
    }
    *out_ << entry_lines_;
    entry_lines_.clear();
  }

  void CheckEntry(const std::string& block_name, std::string_view key, std::string_view value) {
    const std::optional<ParsedInternalKey> parsed = ParseInternalKey(key);
    if (options_.entries) {
      entry_lines_ += "    " + FormatInternalKey(key);
      if (parsed && parsed->type == ValueType::kValue) {
        entry_lines_ += " = '" + EscapeBytes(value, kMaxValueShown) + "'";
      }
      entry_lines_ += '\n';
    }
    if (!parsed) {
      damage_.Report(block_name + ": " + FormatInternalKey(key));
      return;
    }
    if (totals_.entries > 0 &&
        CompareInternalKeys(totals_.largest, key) != std::strong_ordering::less) {
      damage_.Report(block_name + ": " + FormatInternalKey(key) + " is not after " +
                     FormatInternalKey(totals_.largest));
    }
    if (totals_.entries == 0 || ExtractUserKey(totals_.largest) != parsed->user_key) {
      ++totals_.user_keys;
      if (filter_ && !BloomFilterMayMatch(*filter_, parsed->user_key)) {
        ++totals_.filter_misses;
        damage_.Report("the filter does not match key '" + EscapeBytes(parsed->user_key) + "'");
      }
    }
    if (totals_.entries == 0) {
      totals_.smallest.assign(key);
    }
    totals_.largest.assign(key);
    ++totals_.entries;
    ++(parsed->type == ValueType::kDeletion ? totals_.deletions : totals_.puts);
    if (!totals_.min_sequence || parsed->sequence < *totals_.min_sequence) {
      totals_.min_sequence = parsed->sequence;
    }
    if (!totals_.max_sequence || parsed->sequence > *totals_.max_sequence) {
      totals_.max_sequence = parsed->sequence;
    }
  }

  void PrintSummary() {
    *out_ << "data: " << Count(totals_.data_blocks, "block") << " readable, " << totals_.data_bytes
          << " bytes\n";
    *out_ << "entries: " << totals_.entries << " (" << Count(totals_.puts, "put") << ", "
          << Count(totals_.deletions, "deletion") << "), "
          << Count(totals_.user_keys, "distinct user key") << '\n';
    if (totals_.entries > 0) {
      *out_ << "keys: " << FormatInternalKey(totals_.smallest) << " .. "
            << FormatInternalKey(totals_.largest) << '\n';
    }
    if (totals_.min_sequence && totals_.max_sequence) {
      *out_ << "sequence numbers: " << *totals_.min_sequence << " .. " << *totals_.max_sequence
            << '\n';
    }
    if (filter_) {
      *out_ << "filter: " << filter_->size() << " bytes";
      if (filter_->size() >= 2) {
        const std::uint64_t bits = (filter_->size() - 1) * 8;
        *out_ << ", " << bits << " bits, "
              << static_cast<unsigned>(static_cast<unsigned char>(filter_->back())) << " probes";
        if (totals_.user_keys > 0) {
          *out_ << ", " << (bits / totals_.user_keys) << " bits per key";
        }
      }
      *out_ << ", matches " << (totals_.user_keys - totals_.filter_misses) << " of "
            << totals_.user_keys << " keys\n";
    }
    if (damage_.Problems() == 0) {
      *out_ << "result: OK\n";
    } else {
      *out_ << "result: DAMAGED (" << Count(damage_.Problems(), "problem") << ")\n";
    }
  }

  const RandomAccessFile* file_;
  SstDumpOptions options_;
  std::ostream* out_;
  Damage damage_;
  std::optional<std::string> filter_;
  Totals totals_;
  std::string entry_lines_;  // --entries: the current block's, printed after its summary
};

}  // namespace

Status DumpTable(FileSystem& fs, const std::filesystem::path& path, std::string_view display_name,
                 const SstDumpOptions& options, std::ostream& out) {
  FORGEDB_ASSIGN_OR_RETURN(const std::uint64_t size, fs.GetFileSize(path));
  FORGEDB_ASSIGN_OR_RETURN(const std::unique_ptr<RandomAccessFile> file,
                           fs.NewRandomAccessFile(path));
  out << "table " << display_name << ": " << size << " bytes\n";
  TableDumper dumper(*file, options, out);
  return dumper.Run(size);
}

int RunSstDump(std::span<const std::string> args, FileSystem& fs, std::ostream& out,
               std::ostream& err) {
  SstDumpOptions options;
  std::vector<std::string> files;
  for (const std::string& arg : args) {
    if (arg == "--blocks") {
      options.blocks = true;
    } else if (arg == "--entries") {
      options.entries = true;
    } else if (arg.starts_with("--")) {
      err << "forgedb_sst_dump: unknown option " << arg << '\n';
      return 2;
    } else {
      files.push_back(arg);
    }
  }
  if (files.empty()) {
    err << "usage: forgedb_sst_dump [--blocks] [--entries] <file.sst>...\n";
    return 2;
  }
  int exit_code = 0;
  for (const std::string& name : files) {
    const Status status = DumpTable(fs, PathFromUtf8(name), name, options, out);
    if (status.code() == Status::Code::kCorruption) {
      exit_code = std::max(exit_code, 1);
    } else if (!status.ok()) {
      err << "forgedb_sst_dump: " << name << ": " << status.ToString() << '\n';
      exit_code = 2;
    }
  }
  return exit_code;
}

}  // namespace forgedb::tools
