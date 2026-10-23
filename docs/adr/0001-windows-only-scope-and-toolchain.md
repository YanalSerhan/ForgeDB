# ADR 0001: Windows-only scope and toolchain

- **Status:** Accepted
- **Date:** 2026-10-23

## Context

ForgeDB is a portfolio project whose goal is depth: a finished, well-tested, measurably fast
storage engine that its author can explain line by line. A storage engine's hardest correctness
questions are about the operating system: what survives a crash, which operations are atomic, how
file locking and sharing behave. Answering those carefully for one platform is more valuable than
answering them loosely for several.

Windows is a good platform for that depth because its file semantics differ from POSIX in ways that
matter to a database (sharing modes, delete-on-close, no directory `fsync`, byte-range locks owned
by handles; see ADR 0002), and fewer open-source engines document them carefully.

## Decision

1. **Platform:** Windows 10/11, x64, NTFS only. No Linux, no WSL, no POSIX APIs. The build refuses
   to configure for anything else (`CMakeLists.txt`).
2. **Compilers:** MSVC (Visual Studio 2022 17.5+ / 2026) and clang-cl, both in C++20. Every change
   must build warning-free (`/W4 /WX`) with both.
3. **Build:** CMake 3.25+ with Ninja, driven by `CMakePresets.json`: `msvc-debug`, `msvc-release`,
   `msvc-asan`, `clangcl-debug`, `clangcl-asan`, `clangcl-tidy`, `msvc-analyze`, `clangcl-fuzz`.
   `scripts\build-all.ps1` runs them all; CI runs the same script.
4. **Dependencies:** the core library (`src/`) has none. vcpkg (manifest mode, pinned baseline)
   supplies GoogleTest, Google Benchmark and, for comparison benchmarks only, RocksDB, LevelDB and
   SQLite. Packages are static libraries with the dynamic CRT (`x64-windows-static-md`); ASan
   presets use an overlay triplet built with `_ANNOTATE_STL`.
5. **Correctness tooling:** AddressSanitizer (both compilers; a canary test proves it is active),
   libFuzzer via clang-cl, clang-tidy, MSVC `/analyze`, OpenCppCoverage. Windows has no
   ThreadSanitizer, so concurrency safety comes from design (single writer, immutable files,
   reference-counted versions), stress tests checked against a model, and debug assertions.
6. **CI:** GitHub Actions on the pinned `windows-2025-vs2026` image only.
7. **Portability boundary:** only `src/env/` may include `<windows.h>`; everything else uses
   standard C++20 and the `FileSystem` interface.

## Alternatives considered

- **Cross-platform (POSIX + Windows).** Doubles the platform-specific surface (I/O, locking,
  durability rules, CI) and invites lowest-common-denominator answers to exactly the questions this
  project wants to answer precisely.
- **Linux only.** Well-trodden: LevelDB and RocksDB already document Linux behavior thoroughly, so
  it adds less. Also not the author's daily platform.
- **MinGW-w64 (GCC) as a supported compiler.** Not the toolchain Windows C++ jobs use; MSVC and
  clang-cl cover the MSVC ABI that matters. (MinGW is still handy as a *test harness* for compiling
  against Windows headers outside Windows, but it is not supported.)

## Consequences

- One platform's semantics can be studied, documented (ADR 0002) and tested in depth.
- No ThreadSanitizer: concurrency bugs must be prevented by design and caught by stress tests and
  assertions. ADR for the concurrency model follows in Phase 4.
- Porting later stays possible: the `FileSystem` interface is the only platform seam, and the
  conformance suite defines its contract.
- Every CI job pays the cost of a Windows runner; vcpkg's binary cache keeps that manageable.
- Revisit if the project's goal changes from depth to reach.

## References

- `TODO.md`, "Ground rules"
- `CMakeLists.txt`, `CMakePresets.json`, `cmake/ForgeDBCompilerOptions.cmake`,
  `cmake/ForgeDBSanitizers.cmake`, `triplets/x64-windows-static-md-asan.cmake`
- ADR 0002: durability rules on Windows
- [GitHub Actions runner images](https://github.com/actions/runner-images)
