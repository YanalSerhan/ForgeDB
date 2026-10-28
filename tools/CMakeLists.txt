# Command-line tools (TODO 3.8). The logic is in a library, so the unit tests
# can run it against an in-memory file system; the executables are thin
# wmain()s that convert the arguments to UTF-8 and use the Windows file system.
#
#   forgedb_sst_dump   prints and checks a table file (tools/sst_dump.h)
#   forgedb_cli        put, get, delete, scan, stats (tools/cli.h)

add_library(forgedb_tools STATIC
  cli.cpp
  sst_dump.cpp
  tool_util.cpp
)
target_link_libraries(forgedb_tools PUBLIC forgedb::forgedb)
# Includes are "tools/...", and the tools see the library's internals.
target_include_directories(forgedb_tools PUBLIC
  "${PROJECT_SOURCE_DIR}" "${PROJECT_SOURCE_DIR}/src")
forgedb_target_defaults(forgedb_tools)

foreach(tool IN ITEMS sst_dump cli)
  add_executable(forgedb_${tool} ${tool}_main.cpp)
  target_link_libraries(forgedb_${tool} PRIVATE forgedb_tools)
  forgedb_target_defaults(forgedb_${tool})
endforeach()
