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
- [x] Coverage job with OpenCppCoverage; publish the report as an artifact; gate at 85% from Phase 1 on (gate is `COVERAGE_MIN_LINE_PERCENT` in ci.yml: 0 now, set to 85 when Phase 1 starts)
- [ ] Fuzz smoke job (clang-cl): 60 seconds per target on each PR (from Phase 2 on) (deferred to 2.7, when the first fuzz targets exist)

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

> `[~]` items in 1.1-1.2: tests pass on Linux (gcc and clang, ASan+UBSan, asserts on and off) and in the Windows build under Wine; waiting for the MSVC/clang-cl run.

### 1.1 Arena allocator
- [~] Allocates from 4 KiB blocks; large requests (> 1/4 block) get their own block
- [~] Aligned allocation for node structs
- [~] `MemoryUsage()` tracked atomically (read by other threads)
- [~] Unit tests: alignment, large allocations, usage accounting; ASan clean

### 1.2 Internal key format
- [~] Internal key = user key + 8-byte tag (`sequence << 8 | type`), type ∈ {value, deletion}
- [~] Comparator: user key ascending, then sequence **descending** (newest first)
- [~] `LookupKey` helper for Get at a given sequence
- [~] Unit tests for ordering, including equal user keys and maximum sequence

### 1.3 Skip list
- [ ] `template <typename Key, class Comparator> class SkipList`
- [ ] Max height 12, branching factor 4, seeded deterministic random generator
- [ ] `std::atomic<Node*>` next pointers: release on publish, acquire on read
- [ ] Contract: one writer at a time (externally synchronized), any number of lock-free readers, no deletion
- [ ] Iterator: Valid, Seek, SeekToFirst, Next (Prev optional, record the decision)
- [ ] Unit tests: insert/contains, ordered iteration, Seek edge cases
- [ ] Concurrency test: one writer inserting while 4 readers iterate; readers must always see a sorted, consistent prefix
- [ ] Microbenchmark: inserts/sec and lookups/sec at 100k and 1M entries

### 1.4 Memtable
- [ ] Entry encoding in the arena: varint key length, internal key, varint value length, value
- [ ] `Add(seq, type, key, value)`, `Get(lookup_key)` → value / deleted / not found
- [ ] `ApproximateMemoryUsage()` used later as the flush trigger
- [ ] Reference counting (`Ref` / `Unref`) so readers can keep a memtable alive

### 1.5 WriteBatch
- [ ] Encoded representation: 8-byte starting sequence, 4-byte count, then records (type, varint-prefixed key, value)
- [ ] `Put`, `Delete`, `Clear`, `Count`, `ApproximateSize`, `Iterate(Handler&)`
- [ ] Decoder rejects malformed batches with a corruption status
- [ ] Unit tests including malformed input

### 1.6 DB skeleton (memory only)
- [ ] Public headers: `db.h`, `options.h`, `status.h`, `write_batch.h`, `iterator.h`
- [ ] `DBImpl` with one memtable: Put, Delete, Write, Get, NewIterator
- [ ] DB iterator over the memtable that hides older versions and deletions

### 1.7 Model test harness
- [ ] Seeded random operation generator: Put, Delete, WriteBatch, Get, full scan, Seek + N×Next
- [ ] Reference model: `std::map<std::string, std::string>`
- [ ] Compare every read; on mismatch, print the seed and a minimal reproduction
- [ ] Keys from a small key space too (forces overwrites and deletes of existing keys)
- [ ] The harness is reused by every later phase (only the DB setup changes)

### ✅ Exit gate — Phase 1
- [ ] Model test: 1,000,000 random operations agree with `std::map` (several seeds, in CI)
- [ ] Concurrency test passes 1,000 iterations
- [ ] ASan clean; coverage ≥ 85% on `src/`

---

## Phase 2 — Write-ahead log and recovery (≈2 weeks)

### 2.1 Log format
- [ ] 32 KiB blocks; record header = CRC32C (4) + length (2) + type (1); types FULL, FIRST, MIDDLE, LAST for records spanning blocks
- [ ] Block tail too small for a header is zero-padded
- [ ] Document the byte layout in `docs/file-formats.md`
- [ ] `log::Writer` (appends records) and `log::Reader` (reassembles records, reports corruption with offset and reason)
- [ ] Reader distinguishes a **torn tail** (incomplete last record → ignore) from **mid-log corruption** (→ error)
- [ ] Unit tests: records of every size class, block boundaries, empty records, truncated tail, flipped bits

### 2.2 Write path with group commit
- [ ] Writer queue: `std::mutex` + `std::condition_variable`; the first waiting writer becomes leader
- [ ] Leader merges followers' batches (up to a size cap), assigns sequence numbers, appends **one** log record, calls `Sync` once if any writer asked for `sync = true`
- [ ] Apply merged batch to the memtable, then wake followers with their status
- [ ] Sticky background error: after a failed `Sync` or append, refuse all further writes (the log's state on disk is unknown)
- [ ] Unit test: many threads writing; every acknowledged write is visible; sequence numbers are unique and dense
- [ ] Microbenchmark: writes/sec with `sync = true`, 1 thread vs 8 threads (shows group commit's effect)

### 2.3 Open, lock and recovery
- [ ] `LOCK` file locked with `LockFileEx` for the database's lifetime; second open fails with a clear error
- [ ] File naming: `000005.log`, `000007.sst`, `MANIFEST-000003`, `CURRENT`, `LOCK`
- [ ] Minimal metadata for this phase: record the current log number (full MANIFEST arrives in Phase 3)
- [ ] Recovery: replay every log at or after the recorded log number, in order, into a fresh memtable; restore the last sequence number
- [ ] New log file per memtable; memtable switch when `ApproximateMemoryUsage()` passes `memtable_bytes`

### 2.4 Fault-injection file system
- [ ] `FaultInjectionFileSystem` wraps any `FileSystem` and tracks, per file, how many bytes are synced
- [ ] `SimulateCrash()`: truncate every file to its synced length; files never synced may vanish entirely
- [ ] Options: keep a random part of the unsynced tail; corrupt the final partial write (torn write)
- [ ] Error injection: fail the Nth `Append`, `Sync`, `RenameReplace` or `RemoveFile` with an I/O error
- [ ] Its own unit tests (it must be trustworthy before it judges anything else)

### 2.5 Crash torture test
- [ ] Loop: open DB on the fault-injection FS, run random operations (random `sync` flags), crash at a random point, reopen, verify
- [ ] Verification:
  - [ ] Every write acknowledged with `sync = true` is present
  - [ ] Final state equals the model at **some** point between the last synced write and the last acknowledged write (prefix consistency, no holes)
  - [ ] Recovery never fails on a torn tail
- [ ] Error-injection variant: after an injected failure, the DB refuses writes and reopens cleanly
- [ ] Iterations configurable; CI runs a short version, long runs locally

### 2.6 Process-kill test (real crash, Windows)
- [ ] `tests/crash/crash_child.exe`: writes keys with `sync = true`, appends each acknowledged key to a side file
- [ ] Parent launches it with `CreateProcessW`, waits a random time, kills it with `TerminateProcess`, reopens the DB and checks every acknowledged key
- [ ] Note in the README: this covers process crashes; power loss is covered only by the fault-injection model

### 2.7 Fuzzing
- [ ] `fuzz/fuzz_log_reader.cpp` and `fuzz/fuzz_write_batch.cpp` with seed corpus
- [ ] Build with clang-cl `-fsanitize=fuzzer,address` (or MSVC `/fsanitize=fuzzer` and `/fsanitize=address` as separate flags)
- [ ] Enable the CI fuzz smoke job

### ✅ Exit gate — Phase 2
- [ ] 10,000 crash-torture iterations with zero lost synced writes and zero recovery failures
- [ ] 500 process-kill runs pass
- [ ] Each fuzz target ran 1 hour locally with no crashes and no ASan reports

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
- [ ] Immutable memtable → `TableBuilder` → new `.sst` file → `Sync` → close
- [ ] Old log deleted only after the new table is recorded in the MANIFEST and synced

### 3.6 MANIFEST and versions
- [ ] `VersionEdit`: log number, next file number, last sequence, added files (level, number, size, smallest and largest key), deleted files; encode/decode with tags
- [ ] MANIFEST = a log file (same format as 2.1) of `VersionEdit` records
- [ ] `CURRENT` update: write `MANIFEST-xxxxxx` name to a temp file → `Sync` → `RenameReplace` to `CURRENT`
- [ ] `Version` (files per level) and `VersionSet` (current version, applies edits)
- [ ] Recovery: read `CURRENT` → replay MANIFEST → replay logs newer than its log number
- [ ] On open, delete files not referenced by the current version (leftovers from a crash)

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
