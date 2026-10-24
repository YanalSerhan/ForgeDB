// log::Writer: appends records to a write-ahead log file.
//
//   log::Writer writer(file.get());
//   FORGEDB_RETURN_IF_ERROR(writer.AddRecord(WriteBatchInternal::Contents(batch)));
//   if (options.sync) FORGEDB_RETURN_IF_ERROR(file->Sync());
//
// AddRecord ends with WritableFile::Flush, so a returned record has been
// handed to the operating system: it survives a crash of this process, but
// not of the OS or a power failure until the caller Syncs the file.
//
// Not thread-safe: one writer at a time (the DB's group-commit leader).
#pragma once

#include <cstddef>
#include <string_view>

#include "forgedb/status.h"

#include "env/file_system.h"
#include "log/log_format.h"

namespace forgedb::log {

class Writer {
 public:
  // Writes a new log into `file`, which must be empty and outlive the writer.
  explicit Writer(WritableFile* file) noexcept : file_(file) {}

  // Appends `record` (any size, including empty) and flushes it to the OS.
  // After an error the file's contents are unknown and the file refuses all
  // further writes (WritableFile errors are sticky), so the log is finished.
  [[nodiscard]] Status AddRecord(std::string_view record);

 private:
  [[nodiscard]] Status EmitFragment(RecordType type, std::string_view payload);

  WritableFile* file_;
  std::size_t block_offset_ = 0;  // bytes already used in the current block
};

}  // namespace forgedb::log
