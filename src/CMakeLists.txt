# The core library. Rules:
#  - Zero third-party dependencies.
#  - Only files under env/ may include <windows.h> or call Win32 APIs.
#  - Internal headers are included relative to this directory, e.g. "util/coding.h".

configure_file(
  "${PROJECT_SOURCE_DIR}/include/forgedb/version.h.in"
  "${PROJECT_BINARY_DIR}/include/forgedb/version.h"
  @ONLY)

add_library(forgedb STATIC
  db/db_impl.cpp
  db/db_iter.cpp
  db/filename.cpp
  db/internal_key.cpp
  db/version.cpp
  db/write_batch.cpp
  env/mem_file_system.cpp
  env/win_file_system.cpp
  env/win_util.cpp
  log/log_reader.cpp
  log/log_writer.cpp
  memtable/arena.cpp
  memtable/memtable.cpp
  table/block.cpp
  table/block_builder.cpp
  table/block_format.cpp
  table/bloom_filter.cpp
  util/check.cpp
  util/coding.cpp
  util/crc32c.cpp
  util/hash.cpp
  util/status.cpp
)
add_library(forgedb::forgedb ALIAS forgedb)

target_include_directories(forgedb
  PUBLIC
    "$<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>"
    "$<BUILD_INTERFACE:${PROJECT_BINARY_DIR}/include>"
  PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}")

forgedb_target_defaults(forgedb)
