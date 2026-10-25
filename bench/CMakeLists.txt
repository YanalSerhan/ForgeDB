# Google Benchmark microbenchmarks. Numbers are only meaningful from the
# msvc-release preset (the other presets that build this exist to compile and
# lint it).
find_package(benchmark CONFIG REQUIRED)

add_executable(forgedb_microbench
  crc32c_bench.cpp
  db_write_bench.cpp
  memtable_bench.cpp
  skip_list_bench.cpp
)
target_link_libraries(forgedb_microbench PRIVATE forgedb::forgedb benchmark::benchmark_main)
target_include_directories(forgedb_microbench PRIVATE "${PROJECT_SOURCE_DIR}/src")
forgedb_target_defaults(forgedb_microbench)
