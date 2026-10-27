# ForgeDB — TODO

Crash-safe, embedded LSM-tree key-value storage engine in C++20.
**Windows only:** developed, built, tested, benchmarked and run on Windows 10/11 x64 (NTFS)
with MSVC and clang-cl. No Linux, no WSL, no drivers, no admin rights.

**How to use this file**

- Work top to bottom. Don't start a phase until the previous **Exit gate** is fully ticked.
- `[ ]` open · `[x]` done · `[~]` in progress · `[-]` dropped (write why next to it).
- Every task that changes behaviour lands with its tests in the same commit.
- Estimates assume roughly 10–15 hours a week; total about 12–16 weeks.
- **The engine is CV-worthy after Phase 3.** Phases 4–6 add depth.

---

## Ground rules (apply to every phase)

- [ ] The core library (`src/`) has **zero third-party dependencies**; vcpkg packages are for tests, fuzzing and benchmarks only
- [ ] All Win32 calls live in `src/env/`; every other file uses only standard C++20 and the `FileSystem` interface
- [ ] Errors are values: `Status` / `Result<T>`, no exceptions crossing the public API
- [ ] Every on-disk structure is checksummed (CRC32C) and length-checked before it is trusted
- [ ] Every parser treats file bytes as untrusted input: never read past a buffer, never trust a length field
- [ ] RAII for every resource: Win32 handles, locks, file mappings, cache handles, snapshots
- [ ] Warnings as errors: `/W4 /WX` on MSVC, `-Wall -Wextra -Werror` on clang-cl; clang-format on every commit; clang-tidy and MSVC `/analyze` clean before merging to `main`
- [ ] Every byte format is documented in `docs/file-formats.md` before or with the code that writes it
- [ ] Every non-obvious decision gets an ADR in `docs/adr/`
- [ ] Debug builds assert invariants (ordering, reference counts, file ownership) liberally

---

## Phase 0 — Foundations (≈1 week)

### 0.1 Repository and build
- [x] Create repo `forgedb` with the layout: `include/forgedb/`, `src/{env,util,memtable,log,table,db}`, `tools/`, `tests/{unit,model,crash,corruption,stress}`, `fuzz/`, `docs/{adr}`, `.github/workflows/` (added: `cmake/` build modules, `bench/` microbenchmarks, `triplets/` vcpkg overlay triplets, `scripts/` dev scripts)
- [x] Top-level `CMakeLists.txt`: C++20, static library target `forgedb`, `FORGEDB_BUILD_TESTS`, `FORGEDB_BUILD_BENCH`, `FORGEDB_BUILD_FUZZ` options
- [x] `CMakePresets.json`: `msvc-debug`, `msvc-release`, `msvc-asan`, `clangcl-debug`, `clangcl-asan`, `clangcl-fuzz`
- [x] `vcpkg.json` manifest with pinned baseline: `gtest`, `benchmark`; a `bench` feature with `rocksdb`, `leveldb`, `sqlite3` (`gtest` and `benchmark` are features `tests` / `microbench`, switched on by the matching CMake options; ASan presets use the overlay triplet `triplets/x64-windows-static-md-asan` built with `_ANNOTATE_STL`)
- [x] `.clang-format`, `.clang-tidy`, `.gitignore` (`out/`, `build/`, `.vs/`, test databases)
- [x] `README.md` stub: one paragraph, build steps on Windows, badge placeholders

### 0.2 Continuous integration (GitHub Actions, Windows runners only)
- [x] Matrix: MSVC × {debug, release, asan} and clang-cl × {debug, asan} (plus a `clangcl-fuzz` build-only entry so that preset can't rot before 2.7; added: ASan canary test, clang-format job)
- [x] Cache vcpkg binary packages between runs (vcpkg `files` provider + `actions/cache`; the old `x-gha` provider was removed from vcpkg)
- [x] `ctest --output-on-failure` in every job; upload logs on failure
- [x] Coverage job with OpenCppCoverage; publish the report as an artifact; gate at 85% from Phase 1 on (gate is `COVERAGE_MIN_LINE_PERCENT` in ci.yml: 85 since the end of Phase 1)
- [x] Fuzz smoke job (clang-cl): 60 seconds per target on each PR (from Phase 2 on) (2.7: a ctest test in the `clangcl-fuzz` job)

### 0.3 Utilities
- [x] `Status` (code + message) and `Result<T>` (value or `Status`), both `[[nodiscard]]` (+ `FORGEDB_RETURN_IF_ERROR`, `FORGEDB_ASSIGN_OR_RETURN`, and `FORGEDB_CHECK`/`FORGEDB_DCHECK` in `forgedb/check.h`)
- [x] Little-endian fixed32/fixed64 encode/decode; varint32/varint64 encode/decode with bounds checks (also length-prefixed bytes; overflowing varints rejected)
- [x] CRC32C: table-driven implementation + SSE4.2 fast path (`_mm_crc32_u64`), chosen at runtime via `__cpuid` (+ LevelDB-style `Mask`/`Unmask` for stored CRCs)
- [x] CRC32C tests against published test vectors; both paths must agree on random buffers
- [~] Microbenchmark: CRC32C MB/s, table vs SSE4.2 (`bench/crc32c_bench.cpp` builds and runs; numbers to be recorded from the first msvc-release run)

### 0.4 FileSystem layer
- [x] Interfaces in `src/env/`: `WritableFile` (Append, Flush, Sync, Close), `SequentialFile` (Read, Skip), `RandomAccessFile` (positional Read), `FileLock`
- [x] `FileSystem` interface: NewWritableFile, NewSequentialFile, NewRandomAccessFile, RenameReplace, DeleteFile (named `RemoveFile`: `<windows.h>` defines `DeleteFile` as a macro), FileExists, GetChildren, GetFileSize, CreateDir, LockFile, UnlockFile
- [x] `UniqueHandle`: move-only RAII wrapper around a Win32 `HANDLE`
- [x] `WinFileSystem`:
  - [x] Paths via `std::filesystem::path` → wide strings (`CreateFileW`)
  - [x] Share mode `FILE_SHARE_READ | FILE_SHARE_DELETE` so open files can be deleted (readers also allow `FILE_SHARE_WRITE`; the LOCK file deliberately omits `FILE_SHARE_DELETE`; ADR 0002 rule 6)
  - [x] POSIX delete (`FileDispositionInfoEx`) so a removed file's name is free at once, with fallback to delete-on-close (ADR 0002 rule 7)
  - [x] Positional reads with `ReadFile` + `OVERLAPPED` offset (no shared file pointer, safe across threads)
  - [x] Buffered `WritableFile` (e.g., 64 KiB buffer); `Sync` = flush buffer + `FlushFileBuffers`
  - [x] `RenameReplace` = `MoveFileExW(..., MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)`
  - [x] Bounded retry with backoff on `ERROR_SHARING_VIOLATION` / `ERROR_ACCESS_DENIED` for rename and delete
  - [x] `LockFile` = `LockFileEx` on an open handle, held until `UnlockFile`
  - [x] Map `GetLastError()` to `Status` with a readable message (`FormatMessageW`)
- [x] `MemFileSystem`: in-memory implementation for fast tests
- [x] **Conformance suite**: one set of typed GoogleTest tests run against both `WinFileSystem` and `MemFileSystem` (append/read back, positional reads, rename replaces, delete while open, lock held twice fails, missing file errors)

### 0.5 Docs
- [x] `docs/adr/0000-template.md` (context, decision, alternatives, consequences)
- [x] ADR 0001: Windows-only scope and toolchain
- [x] ADR 0002: durability rules on Windows (`FlushFileBuffers`, write-through rename, no directory fsync, MANIFEST lists every live file); link each claim to Microsoft's API documentation (status Proposed: open question on whether recovery may trust the CURRENT rename, to decide before Phase 3)
- [x] `docs/file-formats.md` skeleton (created in 0.3 with the primitive encodings and CRC-32C, per the "document every byte format with its code" rule)

### ✅ Exit gate — Phase 0
- [x] All CI jobs green on `main` (confirmed by the project owner, 2026-10-23)
- [x] Conformance suite passes on both file systems (96/96 with MSVC and clang-cl, 97/97 under ASan, on Windows/NTFS)
- [x] ADR 0002 written and reviewed by you line by line (open question on trusting the CURRENT rename still to decide before Phase 3)

---

## Phase 1 — Memtable core (≈2 weeks)

> Phase 1 closed: every exit-gate item met in CI (run 40104020951; line coverage 97.0%, CI coverage gate raised to 85%).
>
> Microbenchmark numbers below: `msvc-release`, Intel Core i7-10750H laptop, one run each (Google Benchmark; the 1M insert cases ran a single iteration, so treat them as rough). Raw output is not committed (`bench/results/` is ignored).

### 1.1 Arena allocator
- [x] Allocates from 4 KiB blocks; large requests (> 1/4 block) get their own block
- [x] Aligned allocation for node structs
- [x] `MemoryUsage()` tracked atomically (read by other threads)
- [x] Unit tests: alignment, large allocations, usage accounting; ASan clean

### 1.2 Internal key format
- [x] Internal key = user key + 8-byte tag (`sequence << 8 | type`), type ∈ {value, deletion}
- [x] Comparator: user key ascending, then sequence **descending** (newest first)
- [x] `LookupKey` helper for Get at a given sequence
- [x] Unit tests for ordering, including equal user keys and maximum sequence

### 1.3 Skip list
- [x] `template <typename Key, class Comparator> class SkipList`
- [x] Max height 12, branching factor 4, seeded deterministic random generator
- [x] `std::atomic<Node*>` next pointers: release on publish, acquire on read
- [x] Contract: one writer at a time (externally synchronized), any number of lock-free readers, no deletion
- [x] Iterator: Valid, Seek, SeekToFirst, Next (Prev optional, record the decision): no Prev / SeekToLast, recorded in `skip_list.h`
- [x] Unit tests: insert/contains, ordered iteration, Seek edge cases
- [x] Concurrency test: one writer inserting while 4 readers iterate; readers must always see a sorted, consistent prefix
- [x] Microbenchmark: inserts/sec and lookups/sec at 100k and 1M entries (64-bit integer keys)
  - Insert: 4.07 M/s at 100k, 1.19 M/s at 1M · Lookup (hits, random order): 3.59 M/s at 100k, 428 k/s at 1M

### 1.4 Memtable
- [x] Entry encoding in the arena: varint key length, internal key, varint value length, value (documented in `docs/file-formats.md`, "Memtable entry")
- [x] `Add(seq, type, key, value)`, `Get(lookup_key)` → value / deleted / not found
- [x] `ApproximateMemoryUsage()` used later as the flush trigger
- [x] Reference counting (`Ref` / `Unref`) so readers can keep a memtable alive, plus the RAII handle `MemTableRef`
- [x] Microbenchmark (16-byte keys, 100-byte values): Add 595 k/s at 100k, 302 k/s at 1M · Get (hits, random order): 418 k/s at 100k, 279 k/s at 1M

### 1.5 WriteBatch
- [x] Encoded representation: 8-byte starting sequence, 4-byte count, then records (type, varint-prefixed key, value) (documented in `docs/file-formats.md`, "Write batch")
- [x] `Put`, `Delete`, `Clear`, `Count`, `ApproximateSize`, `Iterate(Handler&)`; engine-only parts (sequence, raw contents, `InsertInto(memtable)`) in `src/db/write_batch_internal.h`
- [x] Decoder rejects malformed batches with a corruption status, before applying any record (a batch is applied whole or not at all)
- [x] Unit tests including malformed input

### 1.6 DB skeleton (memory only)
- [x] Public headers: `db.h`, `options.h`, `status.h`, `write_batch.h`, `iterator.h` (option structs stay empty until a phase can honour a setting)
- [x] `DBImpl` with one memtable: Put, Delete, Write, Get, NewIterator; one mutex for writers, lock-free reads at a captured sequence number; key/value size limits → `InvalidArgument`
- [x] DB iterator over the memtable that hides older versions and deletions; reads a snapshot; re-seeks after 8 skipped entries (ADR 0003)

### 1.7 Model test harness
- [x] Seeded random operation generator: Put, Delete, WriteBatch, Get, full scan, Seek + N×Next (`tests/model/model_harness.*`)
- [x] Reference model: `std::map<std::string, std::string>`
- [x] Compare every read; on mismatch, print the seed and a minimal reproduction (reads dropped, then ddmin over writes, then inside batches; self-tested with a deliberately buggy DB)
- [x] Keys from a small key space too (forces overwrites and deletes of existing keys)
- [x] The harness is reused by every later phase (only the DB setup changes: a `DbFactory`)

### ✅ Exit gate — Phase 1
- [x] Model test: 1,000,000 random operations agree with `std::map` (several seeds, in CI): 3 seeds × 1,000,000 in `msvc-release`, 100,000 in debug/ASan presets (CI run 40104020951)
- [x] Concurrency test passes 1,000 iterations (skip list and memtable, `msvc-release`, 1000/1000)
- [x] ASan clean; coverage ≥ 85% on `src/`: ASan clean (`msvc-asan`, `clangcl-asan`, CI run 40104020951); line coverage 97.0% (CI coverage job); CI gate now 85%

---

## Phase 2 — Write-ahead log and recovery (≈2 weeks)

### 2.1 Log format
- [x] 32 KiB blocks; record header = CRC32C (4) + length (2) + type (1); types FULL, FIRST, MIDDLE, LAST for records spanning blocks
- [x] Block tail too small for a header is zero-padded
- [x] Document the byte layout in `docs/file-formats.md`
- [x] `log::Writer` (appends records) and `log::Reader` (reassembles records, reports corruption with offset and reason)
- [x] Reader distinguishes a **torn tail** (incomplete last record → ignore) from **mid-log corruption** (→ error): torn tail iff no valid fragment follows the damage; mid-log corruption fails recovery (strict policy, chosen in 2.1)
- [x] Unit tests: records of every size class, block boundaries, empty records, truncated tail, flipped bits

### 2.2 Write path with group commit
- [x] Writer queue: `std::mutex` + `std::condition_variable`; the first waiting writer becomes leader
- [x] Leader merges followers' batches (up to a size cap), assigns sequence numbers, appends **one** log record, calls `Sync` once if any writer asked for `sync = true` (cap: 1 MiB, or leader's batch + 128 KiB when it is small)
- [x] Apply merged batch to the memtable, then wake followers with their status
- [x] Sticky background error: after a failed `Sync` or append, refuse all further writes (the log's state on disk is unknown)
- [x] Unit test: many threads writing; every acknowledged write is visible; sequence numbers are unique and dense (checked by replaying the log)
- [x] Microbenchmark: writes/sec with `sync = true`, 1 thread vs 8 threads (shows group commit's effect): 1.93 k/s with 1 thread, 8.86 k/s with 8 (×4.6); `msvc-release`, i7-10750H laptop, one run; the drive's write-cache setting was not checked
- [x] `WriteOptions::sync`; DB writes `000001.log` in its directory; tests use `OpenDB(options, name, std::shared_ptr<FileSystem>)` (internal; decided in 2.2); reopening an existing DB is refused until 2.3 (lifted in 2.3)

### 2.3 Open, lock and recovery
- [x] `LOCK` file locked with `LockFileEx` (through `FileSystem::LockFile`) for the database's lifetime; a second open, by another process or another DB object, fails with `Busy` "database is already open"
- [x] File naming: `000005.log`, `000007.sst`, `MANIFEST-000003`, `CURRENT`, `LOCK` (`src/db/filename.h`); the parser accepts only these exact names, anything else in the directory is left alone
- [-] Minimal metadata for this phase: record the current log number — dropped (approved in 2.3): with logs never deleted and one memtable, a metadata file would only repeat what the directory listing says; Phase 3.6's MANIFEST records the log number
- [x] Recovery: replay every log, in number order, into a fresh memtable; restore the last sequence number; sequence numbers must continue without gaps across records and logs (a gap is `Corruption`); torn tails are ignored, mid-log damage fails the open with the log's name (ADR 0004); the new log is numbered one above the highest
- [-] New log file per memtable; memtable switch when `ApproximateMemoryUsage()` passes `memtable_bytes` — moved to 3.5 (approved in 2.3): a switch is only useful once there is somewhere to flush to. Phase 2 uses one memtable and one new log per open
- [x] Model harness: `Reopen()` operation (close, then open the same storage); the model test reopens about five times per seed; a harness test plants a lost-write-on-reopen bug and checks it is caught and shrunk to 3 operations
- Phase 2 limitation (until 3.5): all data stays in memory and logs are never deleted, so every open adds a log and replays all of them

### 2.4 Fault-injection file system
- [x] `FaultInjectionFileSystem` (`tests/support/`, test-only library `forgedb_test_support`) wraps any `FileSystem` and tracks, per file, the bytes written, flushed and synced, and per name which file a crash would leave
- [x] `SimulateCrash(kPowerLoss)`: synced bytes survive exactly; a name survives once its file was synced under it (ADR 0002 rule 3); a name whose latest change was never synced comes back old or new, so files never synced may vanish entirely
- [x] Options: drop the unsynced tail, keep a random part of it, or keep a random part with a torn end (garbage or zeros; the default); renames and removals not durable unless `durable_renames` / `durable_removals` (decided in 2.4: model only documented behavior)
- [x] Error injection: fail the Nth call, or calls with a probability, of `NewWritableFile`, `Append` (optionally writing a partial prefix first), `Flush`, `Sync`, `Close`, `RenameReplace`, `RemoveFile`, `CreateDir`, `LockFile`; `WritableFile` errors stay sticky
- [x] Its own unit tests (it must be trustworthy before it judges anything else): `tests/unit/fault_injection_file_system_test.cpp`, plus the `FileSystem` conformance suite run through it with no faults
- [x] Added in 2.4 (approved): `SimulateCrash(kProcessCrash)` (flushed bytes, renames and removals survive; only unflushed bytes are lost); `CrashAtOperation(n)` (the process dies at the n-th change: that call and every later one fail and change nothing); handles and locks from before a crash are dead afterwards
- [x] Added in 2.4 (approved): `zero_pages_in_unsynced_tail` option (pages written out of order); off by default, since ADR 0004 assumes in-order persistence. 2.5 runs with it and decides whether ADR 0004 needs a refinement
- [x] ADR 0002 rule 11: directories are assumed durable once created (undocumented; the model assumes it too)

### 2.5 Crash torture test
- [x] Loop: open DB on the fault-injection FS, run random operations (random `sync` flags), crash at a random point, reopen, verify (`tests/crash/crash_torture_test.cpp`, executable `forgedb_crash_tests`): each cycle ends with a power loss, a process crash or a clean close, between writes or in the middle of a file operation (`CrashAtOperation`); variants for each kind of unsynced tail (torn, partly kept, dropped)
- [x] Verification:
  - [x] Every write acknowledged with `sync = true` is present (after a process crash or clean close: every acknowledged write)
  - [x] Final state equals the model at **some** point between the last synced write and the last acknowledged write (prefix consistency, no holes); a write that failed may or may not be there. Also before each crash: readers see exactly the acknowledged writes
  - [x] Recovery never fails on a torn tail
- [x] Error-injection variant: after an injected failure, the DB refuses writes and reopens cleanly
- [x] Iterations configurable (`FORGEDB_CRASH_CYCLES`, `FORGEDB_CRASH_SEED`); CI runs 3 seeds × 400 cycles per variant in release builds (100 in debug and sanitizer builds), long runs locally
- [x] Bug found and fixed: data recovered from a log was never synced, so a later power loss could drop an old log's tail while newer synced writes survived (a hole; the next open failed with a sequence gap). Open now syncs every replayed log before creating the new one (new `FileSystem::SyncFile`; ADR 0002 rule 13)
- [x] Bug found and fixed in the 2.4 model: a simulated process crash made every file durable; now unsynced data stays at risk of a later power loss
- [x] Ran with `zero_pages_in_unsynced_tail` (decision from 2.4): ADR 0004's strict rule then refuses some opens with `Corruption` (17 of 600 cycles in the first run). Decided in 2.5: cure deferred to 3.6; until then the test checks the weaker promise (a correct prefix or `Corruption`, never wrong data)
- [ ] Renames and removals: once Phase 3.6 uses them, run with the default (not durable) and settle ADR 0002's open question

### 2.6 Process-kill test (real crash, Windows)
- [x] `tests/crash/crash_child.exe` (`forgedb_crash_child`): writes keys with `sync = true` (batches of 1-5 keys), appends each acknowledged key to a side file (flushed to the OS, so a kill cannot lose it)
- [x] Parent (`ProcessKillTest`, in `forgedb_crash_tests`) launches it with `CreateProcessW`, waits a random time (a quarter of kills land while the child opens and recovers), kills it with `TerminateProcess`, reopens the DB and checks every acknowledged key and its value, that the child's keys form a prefix (no holes; at most one unrecorded batch more), and that earlier runs' keys are untouched. `FORGEDB_KILL_RUNS` / `FORGEDB_KILL_SEED`; CI: 50 kills in release builds, 20 in debug and sanitizer builds
- [x] Note in the README: this covers process crashes; power loss is covered only by the fault-injection model

### 2.7 Fuzzing
- [x] `fuzz/fuzz_log_reader.cpp` (raw bytes; and a write / cut / read-back round trip that must return exactly the complete records) and `fuzz/fuzz_write_batch.cpp` (all-or-nothing decoding, re-encoding, memtable insert), with a seed corpus written by `forgedb_fuzz_seeds` (no binary files in git)
- [x] Build with clang-cl `-fsanitize=fuzzer,address` (libFuzzer runtime linked by name, like the ASan runtime)
- [x] Enable the CI fuzz smoke job: `ctest --preset clangcl-fuzz` runs each target for 60 s (`FORGEDB_FUZZ_SMOKE_SECONDS`), so the existing `clangcl-fuzz` CI job runs it through `build-all.ps1`; long runs: `scripts\fuzz.ps1`
- [x] Fuzzing found a recovery bug: a record whose value contains the bytes of a valid log fragment, torn by a crash, made the open fail with `Corruption` (ADR 0004's search for later valid data looked inside the torn record's own payload). Fixed (approved): an intact fragment's own payload is not searched; its CRC decides whether its length field is damaged (ADR 0004, rule 5). New tests in `log_test.cpp` and `CrashTortureTest.ValuesContainingLogFragments`

### ✅ Exit gate — Phase 2
- [ ] 10,000 crash-torture iterations with zero lost synced writes and zero recovery failures
  - Linux run (2.7, before the Windows run): 10,002 cycles per variant, none lost, no recovery failures except the zero-filled-pages variant (deferred to 3.6). Windows: `FORGEDB_CRASH_CYCLES=3334` (× 3 seeds)
- [ ] 500 process-kill runs pass (Windows only: `FORGEDB_KILL_RUNS=500`)
- [ ] Each fuzz target ran 1 hour locally with no crashes and no ASan reports (Windows: `scripts\fuzz.ps1`)

---

## Phase 3 — SSTables, flush and MANIFEST (≈2–3 weeks)

### 3.1 Blocks
- [ ] `BlockBuilder`: sorted entries with prefix compression; restart point every 16 entries; restart array + count at the end
- [ ] `Block` reader + iterator: binary search over restart points, then linear scan
- [ ] Every block written with a 5-byte trailer: type byte + CRC32C; verified on read
- [ ] Unit tests: shared prefixes, single entry, restart boundaries, corrupted restart array rejected

### 3.2 Bloom filter
- [ ] Double-hashing bloom filter; `k = round(bits_per_key × ln 2)`, clamped to [1, 30]
- [ ] One filter per table (record the choice and its trade-off in an ADR)
- [ ] Test: no false negatives ever; measured false-positive rate within expectations for 10 bits per key

### 3.3 Table format
- [ ] `TableBuilder`: data blocks (~4 KiB), filter block, index block (last key of each data block → block handle), footer
- [ ] Footer: fixed size, holds index and filter handles + a magic number
- [ ] `TableReader`: open via `RandomAccessFile`, read and verify footer, index and filter
- [ ] `Get`: bloom check first, then index lookup, then one data block
- [ ] Two-level iterator (index iterator → data block iterator)
- [ ] Document the byte layout in `docs/file-formats.md`
- [ ] Unit tests: round-trip 100k keys, iterate, seek, missing keys, empty table rejected or handled

### 3.4 Caches
- [ ] Table cache: open `TableReader`s keyed by file number, LRU by count
- [ ] Block cache: sharded LRU (e.g., 16 shards), each with its own mutex, hash map and list; charged in bytes
- [ ] Cache handles are reference-counted RAII objects; an entry in use is never freed
- [ ] Stats: hits, misses, evictions
- [ ] Concurrency test: many threads hitting the cache; ASan clean

### 3.5 Flush to L0
- [ ] Memtable switch when `ApproximateMemoryUsage()` passes `memtable_bytes`; a new log file per memtable (moved from 2.3)
- [ ] Immutable memtable → `TableBuilder` → new `.sst` file → `Sync` → close
- [ ] Old log deleted only after the new table is recorded in the MANIFEST and synced

### 3.6 MANIFEST and versions
- [ ] `VersionEdit`: log number, next file number, last sequence, added files (level, number, size, smallest and largest key), deleted files; encode/decode with tags
- [ ] MANIFEST = a log file (same format as 2.1) of `VersionEdit` records
- [ ] `CURRENT` update: write `MANIFEST-xxxxxx` name to a temp file → `Sync` → `RenameReplace` to `CURRENT`
- [ ] `Version` (files per level) and `VersionSet` (current version, applies edits)
- [ ] Recovery: read `CURRENT` → replay MANIFEST → replay logs newer than its log number (until then, 2.3 replays every log)
- [ ] On open, delete files not referenced by the current version (leftovers from a crash)
- [ ] Zero-filled pages in the newest log (from 2.5): at open, write the recovered memtable to a synced table and record it in the MANIFEST, so old logs are never replayed again; then the newest log stops at its first problem (point-in-time, reported) instead of failing the open. Amend ADR 0004, and switch `CrashTortureTest.ZeroFilledPagesInUnsyncedTails` to "always opens"

### 3.7 Merged reads
- [ ] Merging iterator (min-heap over child iterators: memtable, immutable, L0 files, levels)
- [ ] DB iterator at a sequence number: hides older versions and deletions
- [ ] `Get` path: memtable → immutable → L0 newest first → one file per deeper level

### 3.8 Tools
- [ ] `sst_dump`: print a table's footer, index, filter stats and entries
- [ ] `forgedb_cli`: `put`, `get`, `delete`, `scan`, `stats` from the command line

### 3.9 Tests
- [ ] Model test now runs with a small `memtable_bytes` so data constantly flushes to tables
- [ ] Crash torture extended: crashes during flush and during the `CURRENT` switch
- [ ] Corruption tests: flip random bits in `.sst`, `.log` and MANIFEST files; every read returns correct data or a corruption status, never wrong data, never a crash
- [ ] Fuzz targets: block parser, table footer, `VersionEdit` decoder

### 3.10 Docs
- [ ] ADR: table format (block size, restart interval, one filter per table)
- [ ] ADR: MANIFEST + `CURRENT` switch on Windows
- [ ] README: architecture diagram, build steps, short feature list, current status

### ✅ Exit gate — Phase 3 (minimum CV-worthy engine)
- [ ] Data survives clean restarts and simulated crashes with flushes in progress
- [ ] Corruption suite: zero wrong answers, zero crashes
- [ ] Model test passes 1,000,000 operations across many flushes
- [ ] **Checkpoint:** add ForgeDB to the CV now, even if later phases continue

---

## Phase 4 — Compaction and snapshots (≈2–3 weeks)

### 4.1 Versions and snapshots
- [ ] Versions reference-counted; iterators and Gets pin the version they started with
- [ ] `Snapshot` = a sequence number held by `std::shared_ptr<const Snapshot>`; a list tracks the oldest live snapshot
- [ ] `ReadOptions::snapshot` honoured by Get and iterators
- [ ] Test: an iterator on a snapshot sees a frozen view while writes, flushes and compactions continue

### 4.2 Background work
- [ ] One background thread (`std::thread` + condition variable) for flushes and compactions; flush has priority
- [ ] Clean shutdown: stop scheduling, finish or abandon the current job safely, join the thread
- [ ] Write stalls: slow writes when L0 has too many files, stop writes above a hard limit, resume after compaction (document thresholds)

### 4.3 Leveled compaction
- [ ] Triggers: L0 file count (e.g., 4); level byte targets growing 10× per level
- [ ] Score each level; compact the highest score above 1.0
- [ ] Input selection: round-robin compaction pointer per level; expand inputs to every overlapping file in the next level
- [ ] Output: new tables split by size; never split one user key's versions across outputs in a way that breaks ordering
- [ ] Drop rules:
  - [ ] A value shadowed by a newer value is dropped only if no live snapshot can see it
  - [ ] A deletion marker is dropped only at the bottommost level for that key range, and only if no snapshot needs it
- [ ] Install outputs with one `VersionEdit`; delete input files once no version references them

### 4.4 Statistics
- [ ] Write amplification = bytes written by flushes and compactions ÷ bytes written by users
- [ ] Per-level file counts and sizes; compaction count and time; stall time
- [ ] Exposed through `DB::GetStats()` and `forgedb_cli stats`

### 4.5 Tests
- [ ] Model test with snapshots: random snapshot creation and reads at old snapshots, compared to saved copies of the model
- [ ] Compaction correctness: after forced full compaction, contents equal the model and dropped data is really gone
- [ ] Crash torture with compaction running, including crashes mid-compaction and mid-MANIFEST-switch
- [ ] Stress test: 8 reader threads (Gets, scans, snapshot reads) + 1–4 writer threads + background compaction for 10 minutes; checked against the model; debug assertions on
- [ ] Error injection during compaction: the DB stops writes with a sticky error, data stays readable, reopen recovers

### 4.6 Docs
- [ ] ADR: compaction policy and thresholds
- [ ] ADR: concurrency model without TSan (single writer, immutable files, pinned versions)

### ✅ Exit gate — Phase 4
- [ ] Stress test runs 10 minutes clean, three times in a row
- [ ] Crash torture passes 10,000 iterations with compaction enabled
- [ ] Write amplification reported for a 1-million-key random-write workload

---

## Phase 5 — Benchmarks and tuning (≈1–2 weeks)

### 5.1 Benchmark tool
- [ ] `tools/forgedb_bench`: workloads `fillseq`, `fillrandom`, `overwrite`, `readrandom`, `readmissing`, `readseq`, `seekrandom` + scan, `mixed 90/10`
- [ ] Options: key count, key size (16 B), value size (100 B, 1 KB), `sync` on/off, threads, cache size
- [ ] Reports ops/sec, p50 and p99 latency, MB/s, write and space amplification; output as JSON
- [ ] Engine adapters behind one interface: ForgeDB, RocksDB, LevelDB (from vcpkg `bench` feature), SQLite (`WITHOUT ROWID` key-value table, WAL journal mode, `synchronous` matched to the sync setting)
- [ ] Same options for every engine; options printed with every result

### 5.2 Methodology
- [ ] Exclude the benchmark folder from Windows Defender real-time scanning; note it in `docs/benchmarks.md`
- [ ] Power plan High performance; close other heavy programs
- [ ] 5+ runs per workload; median with min and max
- [ ] Record CPU, RAM, SSD model, Windows build, compiler and flags
- [ ] `bench/plot.py` (matplotlib) turns JSON results into charts

### 5.3 Profiling and optimization
- [ ] Profile `fillrandom` and `readrandom` with the Visual Studio profiler or WPR/WPA
- [ ] At least two optimizations, each with before/after numbers and a flame graph (candidates: write-path lock scope, cache shard count, avoiding copies with `string_view`, CRC32C fast path, bloom check before index read)
- [ ] Every optimization keeps the full test suite green

### ✅ Exit gate — Phase 5
- [ ] `docs/benchmarks.md` has comparison charts for all workloads, both sync modes
- [ ] Includes an honest section on where ForgeDB loses and why

---

## Phase 6 — Write-up and release (≈1 week)

- [ ] README: what it is, architecture diagram, feature list, results table, quick start, how to run tests and benchmarks
- [ ] `docs/architecture.md`: write path, read path, threading, versions and snapshots
- [ ] `docs/file-formats.md`: complete byte layouts for log, block, table, footer, MANIFEST
- [ ] `docs/adr/README.md`: index of all ADRs
- [ ] `LIMITATIONS.md` (or a README section): see "Known limitations" below
- [ ] All CI badges on the README: build, tests, ASan, coverage, fuzz smoke
- [ ] Tag `v1.0.0` with release notes
- [ ] CV line updated with real numbers (crash iterations, ops/sec, write amplification, comparison)
- [ ] Practise the interview talking points out loud (see the plan doc)

### ✅ Exit gate — Phase 6
- [ ] A stranger can clone, build, test and reproduce the headline benchmark on Windows from the README alone

---

## Stretch goals (only after v1.0.0)

- [ ] Block compression (LZ4 or Zstandard behind a feature flag) and its effect on space and speed
- [ ] Key-value separation for large values (values in a separate log)
- [ ] Python bindings with pybind11
- [ ] Direct I/O for tables (`FILE_FLAG_NO_BUFFERING`) compared with buffered I/O
- [ ] Prefix bloom filters for range-scan workloads

---

## Known limitations (keep this list honest and in the README)

- [ ] Single process per database (enforced by the `LOCK` file)
- [ ] No transactions beyond atomic `WriteBatch`
- [ ] Power-loss safety is verified with a simulated file system; real behaviour also depends on the drive's write cache
- [ ] No data-race detector on Windows; concurrency is verified by design, stress tests and assertions
- [ ] Durability needs a drive that honors flush requests; "Turn off Windows write-cache buffer flushing" voids it (ADR 0002, F4)
- [ ] File systems without POSIX delete (FAT32, some network shares, Wine) keep a removed file's name until its last handle closes (ADR 0002, rule 7)
- [ ] Database paths are limited to MAX_PATH unless Windows long paths are enabled
- [ ] Until Phase 3.6: if a power loss leaves unsynced log pages written out of order (undocumented whether NTFS can), the open may fail with `Corruption` instead of dropping the unsynced writes (ADR 0004; measured in 2.5)
