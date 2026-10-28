// forgedb_sst_dump: prints what a table file holds and checks every byte of
// it that can be checked.
//
//   forgedb_sst_dump [--blocks] [--entries] <file.sst>...
//
// For each file: the footer, the index, the filter (size, probes, bits per
// key), the number of entries, puts and deletions, the key and sequence
// ranges. --blocks lists every data block; --entries every entry (keys and
// values escaped, values cut at 100 bytes).
//
// Checks: the footer; every block's CRC (the index, the filter, each data
// block); that data blocks are where the index says, back to back; that keys
// are well-formed internal keys in strictly increasing order; that each index
// key is its block's last key; that the filter matches every key. A damaged
// data block is reported and the dump goes on with the next one.
//
// Exit code: 0 if no damage was found, 1 if some was, 2 for a usage error or
// a file that could not be read at all.
#pragma once

#include <filesystem>
#include <ostream>
#include <span>
#include <string>
#include <string_view>

#include "forgedb/status.h"

#include "env/file_system.h"

namespace forgedb::tools {

struct SstDumpOptions {
  bool blocks = false;   // list every data block
  bool entries = false;  // list every entry
};

// Dumps one table to `out`. OK if nothing was found damaged; Corruption
// naming the first problem otherwise (the report lists all of them); the
// file system's error if the file could not be read.
[[nodiscard]] Status DumpTable(FileSystem& fs, const std::filesystem::path& path,
                               std::string_view display_name, const SstDumpOptions& options,
                               std::ostream& out);

// The whole tool: `args` are the arguments after the program name, in UTF-8.
[[nodiscard]] int RunSstDump(std::span<const std::string> args, FileSystem& fs, std::ostream& out,
                             std::ostream& err);

}  // namespace forgedb::tools
