# AddressSanitizer and libFuzzer support for MSVC and clang-cl.
#
#   FORGEDB_SANITIZE=""        no sanitizer
#   FORGEDB_SANITIZE=address   ASan (msvc-asan, clangcl-asan presets)
#   FORGEDB_SANITIZE=fuzzer    libFuzzer + ASan, clang-cl only (clangcl-fuzz preset)
include_guard(GLOBAL)

set(FORGEDB_SANITIZE "" CACHE STRING
  "Sanitizer build: empty (none), 'address' (ASan) or 'fuzzer' (libFuzzer + ASan, clang-cl only)")
set_property(CACHE FORGEDB_SANITIZE PROPERTY STRINGS "" "address" "fuzzer")

set(FORGEDB_ASAN OFF)
if(FORGEDB_SANITIZE STREQUAL "address" OR FORGEDB_SANITIZE STREQUAL "fuzzer")
  set(FORGEDB_ASAN ON)
elseif(NOT FORGEDB_SANITIZE STREQUAL "")
  message(FATAL_ERROR "FORGEDB_SANITIZE must be empty, 'address' or 'fuzzer' (got '${FORGEDB_SANITIZE}').")
endif()

if(FORGEDB_SANITIZE STREQUAL "fuzzer")
  if(NOT FORGEDB_COMPILER_CLANG_CL)
    message(FATAL_ERROR "FORGEDB_SANITIZE=fuzzer is set up for clang-cl only (use the clangcl-fuzz preset).")
  endif()
  if(FORGEDB_BUILD_TESTS)
    message(FATAL_ERROR
      "FORGEDB_SANITIZE=fuzzer builds only the library and the fuzz targets; set FORGEDB_BUILD_TESTS=OFF.")
  endif()
endif()
if(FORGEDB_BUILD_FUZZ AND NOT FORGEDB_SANITIZE STREQUAL "fuzzer")
  message(FATAL_ERROR "FORGEDB_BUILD_FUZZ=ON requires FORGEDB_SANITIZE=fuzzer (use the clangcl-fuzz preset).")
endif()

if(FORGEDB_ASAN)
  # Sanitizer builds use RelWithDebInfo, never Debug:
  #  - CMake's default MSVC Debug flags include /RTC1, which cannot be combined
  #    with /fsanitize=address.
  #  - LLVM's ASan runtime on Windows supports only the release CRT (/MD), not /MDd.
  # Invariant checks stay on through FORGEDB_ENABLE_ASSERTS, which ignores NDEBUG.
  if(CMAKE_BUILD_TYPE STREQUAL "Debug")
    message(FATAL_ERROR
      "Sanitizer builds must use CMAKE_BUILD_TYPE=RelWithDebInfo (see cmake/ForgeDBSanitizers.cmake).")
  endif()

  # Incremental linking is not supported with ASan; replace CMake's default.
  foreach(flags_var IN ITEMS CMAKE_EXE_LINKER_FLAGS_RELWITHDEBINFO CMAKE_SHARED_LINKER_FLAGS_RELWITHDEBINFO)
    string(REGEX REPLACE "/INCREMENTAL(:YES)?( |$)" "/INCREMENTAL:NO\\2" ${flags_var} "${${flags_var}}")
  endforeach()

  set(_forgedb_asan_dll_dirs "")
  get_filename_component(_forgedb_compiler_dir "${CMAKE_CXX_COMPILER}" DIRECTORY)
  list(APPEND _forgedb_asan_dll_dirs "${_forgedb_compiler_dir}")

  if(FORGEDB_COMPILER_CLANG_CL)
    # CMake runs the linker (link.exe or lld-link) directly instead of through
    # clang-cl, so the linker doesn't know where clang keeps its runtime
    # libraries. Find them via clang's resource directory and name them.
    execute_process(
      COMMAND "${CMAKE_CXX_COMPILER}" /clang:-print-resource-dir
      OUTPUT_VARIABLE _forgedb_resource_dir
      OUTPUT_STRIP_TRAILING_WHITESPACE
      RESULT_VARIABLE _forgedb_rc
      ERROR_QUIET)
    if(NOT _forgedb_rc EQUAL 0 OR _forgedb_resource_dir STREQUAL "")
      message(FATAL_ERROR "Could not query clang-cl's resource directory (clang-cl /clang:-print-resource-dir).")
    endif()
    file(TO_CMAKE_PATH "${_forgedb_resource_dir}" _forgedb_resource_dir)

    # Older LLVM layout: lib/windows/clang_rt.asan_dynamic-x86_64.lib
    # Newer LLVM layout (per-target runtime dirs): lib/x86_64-pc-windows-msvc/clang_rt.asan_dynamic.lib
    find_path(FORGEDB_CLANG_RT_DIR
      NAMES clang_rt.asan_dynamic-x86_64.lib clang_rt.asan_dynamic.lib
      PATHS "${_forgedb_resource_dir}/lib/windows" "${_forgedb_resource_dir}/lib/x86_64-pc-windows-msvc"
      NO_DEFAULT_PATH)
    if(NOT FORGEDB_CLANG_RT_DIR)
      message(FATAL_ERROR
        "clang ASan runtime libraries not found under ${_forgedb_resource_dir}/lib. "
        "Is the 'C++ Clang Compiler for Windows' component installed completely?")
    endif()
    if(EXISTS "${FORGEDB_CLANG_RT_DIR}/clang_rt.asan_dynamic-x86_64.lib")
      set(FORGEDB_CLANG_RT_SUFFIX "-x86_64")
    else()
      set(FORGEDB_CLANG_RT_SUFFIX "")
    endif()

    # The runtime thunk must be linked whole so none of its interceptors are dropped.
    set(FORGEDB_ASAN_LINK_OPTIONS
      "/LIBPATH:${FORGEDB_CLANG_RT_DIR}"
      "clang_rt.asan_dynamic${FORGEDB_CLANG_RT_SUFFIX}.lib"
      "/WHOLEARCHIVE:clang_rt.asan_dynamic_runtime_thunk${FORGEDB_CLANG_RT_SUFFIX}.lib")
    list(APPEND _forgedb_asan_dll_dirs "${FORGEDB_CLANG_RT_DIR}")
  else()
    # cl.exe /fsanitize=address embeds the linker directives for its own ASan
    # runtime, so nothing has to be named here.
    set(FORGEDB_ASAN_LINK_OPTIONS "")
  endif()

  # Copying the ASan runtime DLL next to every executable lets tests run from
  # any shell, from ctest and from Visual Studio, not only a Developer shell.
  find_file(FORGEDB_ASAN_RUNTIME_DLL
    NAMES clang_rt.asan_dynamic-x86_64.dll clang_rt.asan_dynamic.dll
    PATHS ${_forgedb_asan_dll_dirs}
    NO_DEFAULT_PATH)
  if(NOT FORGEDB_ASAN_RUNTIME_DLL)
    message(WARNING
      "ASan runtime DLL not found in: ${_forgedb_asan_dll_dirs}. Executables will need it on PATH to start.")
  endif()
endif()

# Called by forgedb_target_defaults() for every ForgeDB target.
function(forgedb_apply_sanitizers target)
  if(NOT FORGEDB_ASAN)
    return()
  endif()

  # STL container annotations stay ON in our code. The vcpkg libraries linked
  # into ASan builds come from the x64-windows-static-md-asan overlay triplet
  # (triplets/), built with _ANNOTATE_STL, so they don't trigger the linker's
  # 'annotate_*' mismatch check (LNK2038). See that triplet file for details.

  if(FORGEDB_SANITIZE STREQUAL "fuzzer")
    # Coverage instrumentation without libFuzzer's main(); fuzz targets add
    # -fsanitize=fuzzer themselves (Phase 2.7).
    target_compile_options(${target} PRIVATE -fsanitize=fuzzer-no-link,address)
  elseif(FORGEDB_COMPILER_CLANG_CL)
    target_compile_options(${target} PRIVATE -fsanitize=address)
  else()
    target_compile_options(${target} PRIVATE /fsanitize=address)
  endif()

  get_target_property(_type ${target} TYPE)
  if(_type STREQUAL "EXECUTABLE")
    target_link_options(${target} PRIVATE ${FORGEDB_ASAN_LINK_OPTIONS})
    if(FORGEDB_ASAN_RUNTIME_DLL)
      add_custom_command(TARGET ${target} POST_BUILD
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different "${FORGEDB_ASAN_RUNTIME_DLL}" "$<TARGET_FILE_DIR:${target}>"
        VERBATIM)
    endif()
  endif()
endfunction()
