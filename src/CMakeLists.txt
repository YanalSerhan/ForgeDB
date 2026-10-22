# The core library. Rules:
#  - Zero third-party dependencies.
#  - Only files under env/ may include <windows.h> or call Win32 APIs.
#  - Internal headers are included relative to this directory, e.g. "util/coding.h".

configure_file(
  "${PROJECT_SOURCE_DIR}/include/forgedb/version.h.in"
  "${PROJECT_BINARY_DIR}/include/forgedb/version.h"
  @ONLY)

add_library(forgedb STATIC
  db/version.cpp
  util/check.cpp
  util/coding.cpp
  util/crc32c.cpp
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
