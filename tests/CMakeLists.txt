find_package(GTest CONFIG REQUIRED)
include(GoogleTest)

# One executable for all unit tests; model, crash, corruption and stress
# suites get their own executables in later phases.
add_executable(forgedb_unit_tests
  unit/version_test.cpp
)
target_link_libraries(forgedb_unit_tests PRIVATE forgedb::forgedb GTest::gtest_main)
# Unit tests exercise internal classes, so they see src/ like the library does.
target_include_directories(forgedb_unit_tests PRIVATE "${PROJECT_SOURCE_DIR}/src")
forgedb_target_defaults(forgedb_unit_tests)

# PRE_TEST: list the tests when ctest runs, not at build time, so a build
# never depends on being able to start the test executable.
gtest_discover_tests(forgedb_unit_tests DISCOVERY_MODE PRE_TEST)
