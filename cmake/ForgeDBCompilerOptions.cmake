# Compiler detection and the per-target settings every ForgeDB target gets.
#
# Settings are applied per target (forgedb_target_defaults) rather than
# globally, so /WX and friends never leak into third-party code.
include_guard(GLOBAL)

if(CMAKE_CXX_COMPILER_ID STREQUAL "MSVC")
  set(FORGEDB_COMPILER_MSVC ON)
  set(FORGEDB_COMPILER_CLANG_CL OFF)
elseif(CMAKE_CXX_COMPILER_ID STREQUAL "Clang" AND CMAKE_CXX_COMPILER_FRONTEND_VARIANT STREQUAL "MSVC")
  set(FORGEDB_COMPILER_MSVC OFF)
  set(FORGEDB_COMPILER_CLANG_CL ON)
else()
  message(FATAL_ERROR
    "ForgeDB supports MSVC (cl) and clang-cl only; got '${CMAKE_CXX_COMPILER_ID}' (${CMAKE_CXX_COMPILER}).")
endif()

# Invariant checks (FORGEDB_DCHECK, added in Phase 0.3) are controlled by this
# option instead of NDEBUG, so ASan builds (which use RelWithDebInfo and
# therefore define NDEBUG) still check invariants.
option(FORGEDB_ENABLE_ASSERTS "Compile ForgeDB's debug invariant checks" OFF)
option(FORGEDB_ENABLE_CLANG_TIDY "Run clang-tidy on ForgeDB sources while compiling" OFF)
option(FORGEDB_ENABLE_MSVC_ANALYZE "Compile ForgeDB sources with MSVC /analyze" OFF)

if(FORGEDB_ENABLE_CLANG_TIDY)
  find_program(FORGEDB_CLANG_TIDY_EXE NAMES clang-tidy REQUIRED)
endif()

# Applies ForgeDB's warning, platform, assertion, analysis and sanitizer
# settings to one of our own targets. Call it for every target we define.
function(forgedb_target_defaults target)
  # Warnings are errors. In clang-cl, /W4 means clang's -Wall -Wextra.
  # (Writing -Wall in clang-cl would mean -Weverything, because clang-cl
  # treats it as MSVC's /Wall.)
  target_compile_options(${target} PRIVATE /W4 /WX /permissive- /utf-8)
  if(FORGEDB_COMPILER_MSVC)
    # Report the real __cplusplus value and use the standard-conforming preprocessor.
    target_compile_options(${target} PRIVATE /Zc:__cplusplus /Zc:preprocessor)
  endif()

  # Win32 headers: target Windows 10+, keep <windows.h> small, no min/max
  # macros, and use the wide-character (W) APIs by default.
  target_compile_definitions(${target} PRIVATE
    _WIN32_WINNT=0x0A00
    WINVER=0x0A00
    NOMINMAX
    WIN32_LEAN_AND_MEAN
    UNICODE
    _UNICODE)

  if(FORGEDB_ENABLE_ASSERTS)
    target_compile_definitions(${target} PUBLIC FORGEDB_ENABLE_ASSERTS=1)
  endif()

  if(FORGEDB_ENABLE_CLANG_TIDY)
    set_target_properties(${target} PROPERTIES CXX_CLANG_TIDY "${FORGEDB_CLANG_TIDY_EXE}")
  endif()

  # /analyze runs on the library only: on test and benchmark files it mostly
  # analyzes GoogleTest's and Google Benchmark's macro expansions. clang-tidy
  # still checks those files. /analyze:external- skips external headers (vcpkg,
  # Windows SDK); /WX makes its warnings errors.
  get_target_property(_forgedb_target_type ${target} TYPE)
  if(FORGEDB_ENABLE_MSVC_ANALYZE AND FORGEDB_COMPILER_MSVC AND _forgedb_target_type STREQUAL "STATIC_LIBRARY")
    target_compile_options(${target} PRIVATE /analyze /analyze:external-)
  endif()

  forgedb_apply_sanitizers(${target})
endfunction()
