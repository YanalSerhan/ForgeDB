# libFuzzer targets (TODO 2.7), built only by the clangcl-fuzz preset. The
# library is built with -fsanitize=fuzzer-no-link,address instrumentation by
# cmake/ForgeDBSanitizers.cmake; each target adds -fsanitize=fuzzer and links
# clang's libFuzzer runtime.
#
#   forgedb_fuzz_log_reader    the write-ahead log reader on untrusted bytes
#   forgedb_fuzz_write_batch   write batch decoding on untrusted bytes
#   forgedb_fuzz_block         the table block parser (TODO 3.9)
#   forgedb_fuzz_table         table files: footer, index, filter, data blocks (TODO 3.9)
#   forgedb_fuzz_version_edit  version edits and MANIFEST recovery (TODO 3.9)
#   forgedb_fuzz_seeds         writes the seed corpus (no binary files in git)
#
# `ctest --preset clangcl-fuzz` writes the corpus and runs each target for
# FORGEDB_FUZZ_SMOKE_SECONDS (CI's smoke run). Long runs: scripts\fuzz.ps1.

set(FORGEDB_FUZZ_SMOKE_SECONDS 60 CACHE STRING "Seconds per fuzz target in the ctest smoke run")

# CMake links with lld-link directly, not through clang-cl, so the libFuzzer
# runtime has to be named (as the ASan runtime is, in ForgeDBSanitizers.cmake).
# It is built for the static release CRT, which is why the fuzz preset uses
# /MT (ForgeDBSanitizers.cmake); LLVM 22 ships no /MD build of it.
set(_libfuzzer "clang_rt.fuzzer${FORGEDB_CLANG_RT_SUFFIX}.lib")
if(NOT EXISTS "${FORGEDB_CLANG_RT_DIR}/${_libfuzzer}")
  message(FATAL_ERROR
    "libFuzzer runtime ${_libfuzzer} not found in ${FORGEDB_CLANG_RT_DIR}. "
    "Libraries there: ${FORGEDB_CLANG_RT_LIBS}")
endif()

function(forgedb_add_fuzzer name source)
  add_executable(${name} ${source})
  target_link_libraries(${name} PRIVATE forgedb::forgedb)
  target_include_directories(${name} PRIVATE "${PROJECT_SOURCE_DIR}/src")
  forgedb_target_defaults(${name})
  target_compile_options(${name} PRIVATE -fsanitize=fuzzer)
  # Whole archive, as clang-cl does it: libFuzzer provides main().
  target_link_options(${name} PRIVATE "/WHOLEARCHIVE:${FORGEDB_CLANG_RT_DIR}/${_libfuzzer}")
endfunction()

forgedb_add_fuzzer(forgedb_fuzz_log_reader fuzz_log_reader.cpp)
forgedb_add_fuzzer(forgedb_fuzz_write_batch fuzz_write_batch.cpp)
forgedb_add_fuzzer(forgedb_fuzz_block fuzz_block.cpp)
forgedb_add_fuzzer(forgedb_fuzz_table fuzz_table.cpp)
forgedb_add_fuzzer(forgedb_fuzz_version_edit fuzz_version_edit.cpp)

add_executable(forgedb_fuzz_seeds make_seed_corpus.cpp)
target_link_libraries(forgedb_fuzz_seeds PRIVATE forgedb::forgedb)
target_include_directories(forgedb_fuzz_seeds PRIVATE "${PROJECT_SOURCE_DIR}/src")
forgedb_target_defaults(forgedb_fuzz_seeds)

# Smoke run: write the corpus, then fuzz each target. New inputs go to the
# *-work directory (libFuzzer writes to the first corpus directory), crashing
# inputs to corpus/artifacts/.
set(_corpus "${CMAKE_CURRENT_BINARY_DIR}/corpus")
add_test(NAME FuzzSeedCorpus COMMAND forgedb_fuzz_seeds "${_corpus}")
set_tests_properties(FuzzSeedCorpus PROPERTIES FIXTURES_SETUP forgedb_fuzz_corpus)
math(EXPR _timeout "${FORGEDB_FUZZ_SMOKE_SECONDS} + 300")
foreach(target IN ITEMS log_reader write_batch block table version_edit)
  add_test(NAME Fuzz.${target}
    COMMAND forgedb_fuzz_${target}
      -max_total_time=${FORGEDB_FUZZ_SMOKE_SECONDS}
      -print_final_stats=1
      "-artifact_prefix=${_corpus}/artifacts/"
      "${_corpus}/${target}-work" "${_corpus}/${target}")
  set_tests_properties(Fuzz.${target} PROPERTIES
    FIXTURES_REQUIRED forgedb_fuzz_corpus
    TIMEOUT ${_timeout})
endforeach()
