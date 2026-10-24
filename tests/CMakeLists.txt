find_package(GTest CONFIG REQUIRED)
include(GoogleTest)

# One executable for all unit tests; model, crash, corruption and stress
# suites get their own executables in later phases.
add_executable(forgedb_unit_tests
  unit/arena_test.cpp
  unit/check_test.cpp
  unit/coding_test.cpp
  unit/crc32c_test.cpp
  unit/db_test.cpp
  unit/file_system_conformance_test.cpp
  unit/internal_key_test.cpp
  unit/memtable_test.cpp
  unit/result_test.cpp
  unit/skip_list_test.cpp
  unit/status_test.cpp
  unit/version_test.cpp
  unit/win_file_system_test.cpp
  unit/write_batch_test.cpp
)
target_link_libraries(forgedb_unit_tests PRIVATE forgedb::forgedb GTest::gtest_main)
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
target_link_libraries(forgedb_model_tests PRIVATE forgedb::forgedb GTest::gtest_main)
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
