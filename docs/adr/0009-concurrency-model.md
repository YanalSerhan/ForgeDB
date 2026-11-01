# ADR 0009: Concurrency without a data-race detector on Windows

- **Status:** Accepted
- **Date:** 2026-10-31

## Context

ForgeDB is used from many threads at once (forgedb/db.h: every DB method may be called
concurrently), and since Phase 4.2 it runs a background thread that flushes and compacts. A data
race in a storage engine can lose or corrupt data silently.

The usual tool for finding races is ThreadSanitizer, and it does not run where ForgeDB runs:
clang's ThreadSanitizer supports Android, Darwin, FreeBSD, Linux and NetBSD, not Windows
([clang docs](https://clang.llvm.org/docs/ThreadSanitizer.html)), and MSVC's only sanitizer is
AddressSanitizer ([Microsoft docs](https://learn.microsoft.com/en-us/cpp/sanitizers/)). ADR 0001
keeps ForgeDB Windows-only, so ThreadSanitizer cannot be part of its builds or CI.

So the concurrency design has to be simple enough to check by reading, and every rule that a
tool can check should be checked by one.

## Decision

1. **One DB mutex.** `DBImpl::mutex_` guards all of the DB's shared mutable state: the writers'
   queue, the memtables, the current `Version`, the last sequence number, the background
   thread's job state, the statistics and the sticky error. Each such member is declared
   `FORGEDB_GUARDED_BY(mutex_)`. The mutex is never held during I/O: log appends and syncs,
   table writes, file deletions and reads all happen with it released.
2. **The rule above is checked by the compiler.** ForgeDB locks through `Mutex`, `MutexLock` and
   `CondVar` (`src/util/mutex.h`), which carry Clang thread-safety annotations
   (`src/util/thread_annotations.h`); MSVC's `std::mutex` has none. Every target built with
   clang-cl gets `-Wthread-safety`, an error under `/WX`: reading or writing a guarded member
   without its mutex, or calling a `FORGEDB_REQUIRES(mutex_)` function without it, does not
   compile in the `clangcl-*` presets. `ThreadSafetyCanary.UnguardedAccessIsAnError` proves the
   analysis is on. Waits are written as loops around `CondVar::Wait`, not predicate lambdas,
   because the analysis treats a lambda as a separate function that holds nothing.
   `FORGEDB_NO_THREAD_SAFETY_ANALYSIS` (the escape hatch) is not used; a use must say why.
3. **Single writer.** Writes queue under the mutex; the writer at the front leads a group
   (Phase 2.2). Only the leader touches the log, the memtable's write side, the group batch and
   the memtable entry count, and leadership passes under the mutex, so there is always one
   writer. This ownership is by role, not by a mutex, so the analysis cannot check it; it is
   documented at each member.
4. **The memtable.** The skip list has one writer and lock-free readers: a new node's next
   pointers are set first and it is linked in with release stores; readers follow pointers with
   acquire loads. Nodes are never removed. The arena that holds them lives as long as any
   `MemTableRef` (an atomic reference count) to its memtable.
5. **All-or-nothing visibility.** `last_sequence_` advances under the mutex only after the whole
   group is in the memtable, and every read uses one sequence number taken under the mutex (or a
   snapshot's). Entries above it are invisible, so a batch appears all at once.
6. **Immutable files, pinned versions.** Tables never change after they are synced. A read takes
   the memtable, the immutable memtable, the current `Version` (a `shared_ptr<const Version>`)
   and its sequence number under the mutex, then works without it; whatever it took stays alive
   for as long as it needs. A table is deleted only when no live `Version` lists it
   (`VersionSet::LiveFiles`), and only after a later MANIFEST record (ADR 0007, ADR 0008).
7. **One background thread.** It does one job at a time (a flush before anything else). After
   the open, only it uses the `VersionSet`, except for file numbers, an atomic counter the write
   leader also takes for new logs. It and the writers coordinate through the DB mutex and two
   `CondVar`s; a running compaction checks two atomic flags (a queued flush, shutdown) without
   the mutex.
8. **Other locks, and their order.** `SnapshotList` (one per DB, shared with its snapshots) and
   each shard of the LRU caches (block cache, table cache) have their own `Mutex`, and so do
   `MemFileSystem` and each of its files. Order: the DB mutex may be held while taking the
   snapshot list's mutex, never the reverse; the cache shards' mutexes are taken without the DB
   mutex; `MemFileSystem` takes its own mutex before a file's. No callback runs under a lock, and
   cache values are destroyed after the shard's lock is released.

## Alternatives considered

- **Finer-grained locks** (separate locks for the queue, the version, the statistics). More
  concurrency in theory, but more orders to get right with no detector to catch mistakes; the
  stress test shows no contention problem worth that (4.5: reads and writes both progress with
  8 readers and 4 writers).
- **Lock-free publishing of versions (RCU style).** Reads already hold the mutex only to copy a
  few pointers; the gain would be small and the reasoning much harder.
- **Several background threads** (parallel compactions, as RocksDB does). Higher throughput on
  large machines; a Phase 5 question if measurements call for it. It would change rules 6 and 7.
- **ThreadSanitizer in CI on Linux.** The portable code (everything except `src/env`'s Windows
  file system) does build on Linux, and development has used ThreadSanitizer there; making it
  part of CI would mean supporting a Linux build, which ADR 0001 rules out.
- **The ADR only, without annotations.** Rule 1 would then be a convention that review must
  catch; with the annotations, breaking it is a compile error.

## Consequences

- A mistake against rule 1 (or a missing `FORGEDB_REQUIRES`) fails the `clangcl-*` builds. MSVC
  builds accept it; the clang-cl presets run in every `build-all.ps1` and in CI.
- What the analysis cannot see is checked by tests: role-owned state (rule 3, the background
  thread's own members), the `Writer` fields (guarded by `DBImpl::mutex_`, which the nested
  struct cannot name), the atomics of rules 4 and 7, and constructors and destructors, which the
  analysis skips ([clang docs](https://clang.llvm.org/docs/ThreadSafetyAnalysis.html)). Those
  tests are the stress test (`forgedb_stress_tests`, with assertions in the ASan presets; its
  Phase 4 exit-gate runs: three 10-minute runs on Windows), the planted bugs it caught (4.5), the
  `Version` invariant check, and ThreadSanitizer runs of the portable code on Linux during
  development (no warnings in the Phase 4 runs, including 5 minutes of the stress test).
- Writes are serialized through the leader, and there is one compaction at a time. Reads never
  wait for I/O done under the lock.
- Revisit if a profile shows the DB mutex as a bottleneck, or if parallel compactions are added.

## References

- ADR 0001 (Windows-only scope), ADR 0007 and 0008 (when files are deleted).
- `src/util/mutex.h`, `src/util/thread_annotations.h`, `src/db/db_impl.h` (the comment at the top
  and the member list), `src/memtable/skip_list.h`, `tests/static/thread_safety_canary.cpp`,
  `tests/unit/mutex_test.cpp`, `tests/stress/`.
- Clang thread-safety analysis: https://clang.llvm.org/docs/ThreadSafetyAnalysis.html
- ThreadSanitizer platforms: https://clang.llvm.org/docs/ThreadSanitizer.html
