# libFuzzer targets (Phase 2.7 onwards), built only by the clangcl-fuzz preset.
# Each target will compile with -fsanitize=fuzzer,address and link clang's
# libFuzzer runtime; the library itself is built with fuzzer-no-link
# instrumentation by cmake/ForgeDBSanitizers.cmake.
