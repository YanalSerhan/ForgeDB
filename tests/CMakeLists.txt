find_package(GTest CONFIG REQUIRED)
include(GoogleTest)

# Test-only helpers shared by the test executables: the fault-injection file
# system (TODO 2.4) and environment-variable overrides. Not part of the engine.
add_library(forgedb_test_support STATIC
  support/fault_injection_file_system.cpp
  support/test_env.cpp
)
target_link_libraries(forgedb_test_support PUBLIC forgedb::forgedb)
target_include_directories(forgedb_test_support PUBLIC
  "${PROJECT_SOURCE_DIR}/src" "${CMAKE_CURRENT_SOURCE_DIR}")
forgedb_target_defaults(forgedb_test_support)

# One executable for all unit tests; the model, crash and corruption suites
# have their own executables below.
add_executable(forgedb_unit_tests
  unit/arena_test.cpp
  unit/block_test.cpp
  unit/bloom_filter_test.cpp
  unit/check_test.cpp
  unit/coding_test.cpp
  unit/crc32c_test.cpp
  unit/db_flush_test.cpp
  unit/db_recovery_test.cpp
  unit/db_test.cpp
  unit/db_write_test.cpp
  unit/fault_injection_file_system_test.cpp
  unit/filename_test.cpp
  unit/file_system_conformance_test.cpp
  unit/hash_test.cpp
  unit/internal_key_test.cpp
  unit/log_test.cpp
  unit/lru_cache_test.cpp
  unit/memtable_test.cpp
  unit/merging_iterator_test.cpp
  unit/result_test.cpp
  unit/skip_list_test.cpp
  unit/status_test.cpp
  unit/table_cache_test.cpp
  unit/table_test.cpp
  unit/tools_test.cpp
  unit/version_edit_test.cpp
  unit/version_set_test.cpp
  unit/version_test.cpp
  unit/win_file_system_test.cpp
  unit/write_batch_test.cpp
)
target_link_libraries(forgedb_unit_tests PRIVATE
  forgedb::forgedb forgedb_test_support forgedb_tools GTest::gtest_main)
# Unit tests exercise internal classes, so they see src/ like the library does.
target_include_directories(forgedb_unit_tests PRIVATE "${PROJECT_SOURCE_DIR}/src")
forgedb_target_defaults(forgedb_unit_tests)

# PRE_TEST: list the tests when ctest runs, not at build time, so a build
# never depends on being able to start the test executable.
gtest_discover_tests(forgedb_unit_tests DISCOVERY_MODE PRE_TEST)

# Model tests (TODO 1.7): a seeded random workload checked against std::map.
# The harness (model/model_harness.*) is shared by the model tests of every
# phase. Optimized builds run the Phase 1 exit gate, 1,000,000 operations per
# seed; debug and sanitizer builds run 100,000, since they are 20-50x slower.
# FORGEDB_MODEL_OPS and FORGEDB_MODEL_SEED override at run time.
add_executable(forgedb_model_tests
  model/memory_db_model_test.cpp
  model/model_harness.cpp
  model/model_harness_test.cpp
)
target_link_libraries(forgedb_model_tests PRIVATE
  forgedb::forgedb forgedb_test_support GTest::gtest_main)
target_include_directories(forgedb_model_tests PRIVATE
  "${PROJECT_SOURCE_DIR}/src" "${CMAKE_CURRENT_SOURCE_DIR}")
if(CMAKE_BUILD_TYPE STREQUAL "Release" AND NOT FORGEDB_ASAN)
  set(forgedb_model_operations 1000000)
else()
  set(forgedb_model_operations 100000)
endif()
target_compile_definitions(forgedb_model_tests PRIVATE
  FORGEDB_MODEL_TEST_OPERATIONS=${forgedb_model_operations})
forgedb_target_defaults(forgedb_model_tests)
gtest_discover_tests(forgedb_model_tests DISCOVERY_MODE PRE_TEST)

# Crash tests.
#  - Crash torture (TODO 2.5): random crashes on the fault-injection file
#    system, checking recovery after each. FORGEDB_CRASH_CYCLES and
#    FORGEDB_CRASH_SEED override at run time (long runs: locally).
#  - Process kill (TODO 2.6): real crashes. The test starts
#    forgedb_crash_child (built next to it), kills it with TerminateProcess
#    and checks the database. FORGEDB_KILL_RUNS and FORGEDB_KILL_SEED override.
# Optimized builds run more cycles and kills than debug and sanitizer builds.
add_executable(forgedb_crash_child
  crash/crash_child.cpp
)
target_link_libraries(forgedb_crash_child PRIVATE forgedb::forgedb)
target_include_directories(forgedb_crash_child PRIVATE
  "${PROJECT_SOURCE_DIR}/src" "${CMAKE_CURRENT_SOURCE_DIR}")
forgedb_target_defaults(forgedb_crash_child)

add_executable(forgedb_crash_tests
  crash/crash_torture_test.cpp
  crash/process_kill_test.cpp
)
add_dependencies(forgedb_crash_tests forgedb_crash_child)
target_link_libraries(forgedb_crash_tests PRIVATE
  forgedb::forgedb forgedb_test_support GTest::gtest_main)
if(CMAKE_BUILD_TYPE STREQUAL "Release" AND NOT FORGEDB_ASAN)
  set(forgedb_crash_cycles 400)
  set(forgedb_kill_runs 50)
else()
  set(forgedb_crash_cycles 100)
  set(forgedb_kill_runs 20)
endif()
target_compile_definitions(forgedb_crash_tests PRIVATE
  FORGEDB_CRASH_TEST_CYCLES=${forgedb_crash_cycles}
  FORGEDB_KILL_TEST_RUNS=${forgedb_kill_runs})
forgedb_target_defaults(forgedb_crash_tests)
gtest_discover_tests(forgedb_crash_tests DISCOVERY_MODE PRE_TEST)

# Corruption suite (TODO 3.9): damage one file of a database (tables, logs,
# MANIFESTs: bit flips, random or zeroed runs, truncation), reopen, and check
# that every answer is right or a Corruption status. FORGEDB_CORRUPTION_TRIALS
# and FORGEDB_CORRUPTION_SEED override at run time.
add_executable(forgedb_corruption_tests
  corruption/corruption_test.cpp
)
target_link_libraries(forgedb_corruption_tests PRIVATE
  forgedb::forgedb forgedb_test_support GTest::gtest_main)
if(CMAKE_BUILD_TYPE STREQUAL "Release" AND NOT FORGEDB_ASAN)
  set(forgedb_corruption_trials 500)
else()
  set(forgedb_corruption_trials 100)
endif()
target_compile_definitions(forgedb_corruption_tests PRIVATE
  FORGEDB_CORRUPTION_TEST_TRIALS=${forgedb_corruption_trials})
forgedb_target_defaults(forgedb_corruption_tests)
gtest_discover_tests(forgedb_corruption_tests DISCOVERY_MODE PRE_TEST)

# ASan canary (sanitizer builds only): a program that overflows a heap buffer
# on purpose. The test passes only if ASan's report appears, which proves the
# sanitizer is actually active. Exit code is ignored; the report is the signal.
if(FORGEDB_ASAN)
  add_executable(forgedb_asan_canary sanitizer/asan_canary.cpp)
  forgedb_target_defaults(forgedb_asan_canary)
  add_test(NAME AsanCanary.ReportsHeapBufferOverflow COMMAND forgedb_asan_canary)
  set_tests_properties(AsanCanary.ReportsHeapBufferOverflow PROPERTIES
    PASS_REGULAR_EXPRESSION "ERROR: AddressSanitizer: heap-buffer-overflow")
endif()
