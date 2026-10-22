# ForgeDB: crash-safe embedded LSM-tree key-value storage engine (C++20, Windows only).
cmake_minimum_required(VERSION 3.25)

# -----------------------------------------------------------------------------
# Build options
#
# Declared BEFORE project() on purpose: they decide which vcpkg manifest
# features get installed, and the vcpkg toolchain file runs its install step
# inside project(). The core library never depends on any of these packages;
# they exist only for tests, benchmarks and fuzzing.
# -----------------------------------------------------------------------------
option(FORGEDB_BUILD_TESTS "Build the GoogleTest suites (vcpkg feature 'tests')" ON)
option(FORGEDB_BUILD_BENCH "Build the Google Benchmark microbenchmarks (vcpkg feature 'microbench')" OFF)
option(FORGEDB_BUILD_FUZZ "Build the libFuzzer targets (clang-cl with FORGEDB_SANITIZE=fuzzer)" OFF)

if(FORGEDB_BUILD_TESTS)
  list(APPEND VCPKG_MANIFEST_FEATURES "tests")
endif()
if(FORGEDB_BUILD_BENCH)
  list(APPEND VCPKG_MANIFEST_FEATURES "microbench")
endif()

project(ForgeDB
  VERSION 0.1.0
  DESCRIPTION "Crash-safe embedded LSM-tree key-value storage engine for Windows"
  LANGUAGES CXX)

# -----------------------------------------------------------------------------
# Platform guard: Windows 10/11 x64 only, by design. Fail early with a clear
# message instead of a wall of errors from missing Win32 headers.
# -----------------------------------------------------------------------------
if(NOT WIN32)
  message(FATAL_ERROR "ForgeDB builds only on Windows 10/11 x64.")
endif()
if(NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
  message(FATAL_ERROR
    "ForgeDB builds only for x64. Open an x64 Developer PowerShell (Launch-VsDevShell.ps1 -Arch amd64).")
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Put debug info inside each object file (/Z7) instead of one shared .pdb (/Zi).
# With Ninja's parallel compiles this avoids contention on the shared PDB, and it
# is the format compiler caches (used later in CI) can handle. Linked .exe files
# still get a normal .pdb.
set(CMAKE_MSVC_DEBUG_INFORMATION_FORMAT "$<$<CONFIG:Debug,RelWithDebInfo>:Embedded>")

list(APPEND CMAKE_MODULE_PATH "${PROJECT_SOURCE_DIR}/cmake")
include(ForgeDBCompilerOptions)
include(ForgeDBSanitizers)

add_subdirectory(src)

if(FORGEDB_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()
if(FORGEDB_BUILD_BENCH)
  add_subdirectory(bench)
endif()
if(FORGEDB_BUILD_FUZZ)
  add_subdirectory(fuzz)
endif()
