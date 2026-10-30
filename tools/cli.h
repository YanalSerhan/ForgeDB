// forgedb_cli: a database from the command line.
//
//   forgedb_cli <db-dir> put <key> <value>
//   forgedb_cli <db-dir> get <key>
//   forgedb_cli <db-dir> delete <key>
//   forgedb_cli <db-dir> scan [--from <key>] [--limit <n>]
//   forgedb_cli <db-dir> stats
//
// Keys and values are taken as given (UTF-8 bytes). put and delete are
// synced writes, and create the database if it does not exist; the other
// commands need an existing directory. get prints the value as it is;
// scan prints "key<TAB>value" lines, escaped (tools/tool_util.h); stats
// prints the tables per level, the last sequence number, what this open of
// the database did (DB::GetStats: the counters start at the open, so they
// show the work the command's own open set off, such as a compaction), and
// what recovery found.
//
// Every command opens the database, so it runs recovery: writes left in the
// log become a level-0 table, which compaction later merges into the deeper
// levels.
//
// Exit code: 0 on success, 1 if get found no value, 2 for a usage error or a
// database error (the message goes to the error stream).
#pragma once

#include <memory>
#include <ostream>
#include <span>
#include <string>

#include "forgedb/stats.h"

#include "env/file_system.h"

namespace forgedb::tools {

// The whole tool: `args` are the arguments after the program name, in UTF-8.
[[nodiscard]] int RunCli(std::span<const std::string> args, std::shared_ptr<FileSystem> fs,
                         std::ostream& out, std::ostream& err);

// The "since this open" part of stats: bytes written, flushes, compactions
// per level, write amplification, stalls.
void PrintActivity(const DBStats& stats, std::ostream& out);

}  // namespace forgedb::tools
