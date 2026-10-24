# ADR 0003: The DB iterator re-seeks past long runs of hidden entries

- **Status:** Accepted
- **Date:** 2026-10-24

## Context

The memtable stores every version of every key, ordered by user key ascending and then newest
version first (`docs/file-formats.md`, "Internal key"). The DB iterator (`src/db/db_iter.cpp`)
turns that stream into the user's view: for each user key, the newest version visible at the
iterator's sequence number decides (a value is shown, a deletion hides the key), and every other
entry of that key is skipped. Entries newer than the iterator's sequence number are skipped too.

Skipping one entry at a time costs O(versions of the key) per `Next()`. Phase 1 never flushes the
memtable, so a frequently written key accumulates versions without bound; later phases flush at a
size limit, but a hot key can still have many versions in one memtable, and compaction keeps
versions that live snapshots need.

Measured on the Phase 1 model test (seed 1, 1,000,000 operations, 70% of keys from a 32-key set,
optimized gcc build in the development container): stepping one entry at a time took
**11 min 29 s**, and the iterator stepped over 6.7 billion entries (about 3,480 per call).

## Decision

1. The iterator counts the entries it skips in a row while looking for the next user key.
2. After `kMaxSequentialSkip` (8) skipped entries it stops stepping and re-seeks:
   - if it is skipping older versions of a key it already returned or found deleted, to the
     internal key `(that key, sequence 0, deletion)`, which sorts after every real version of it
     (the DB never writes sequence 0);
   - if it is skipping versions newer than its sequence number, to `(that key, its sequence
     number)`, which is where `Seek()` would land.
3. Both targets sort strictly after the current entry, so every re-seek makes progress. If the
   current entry itself has sequence 0 (only possible when a memtable is built directly, in
   tests), the iterator steps instead of seeking.

## Alternatives considered

- **Always step.** Simplest, and LevelDB does this. Too slow here (above).
- **Always seek past a key's remaining versions.** Every `Next()` would pay O(log n) even when a
  key has one or two versions, which is the common case. The threshold keeps those cases a single
  step.
- **Store a "next user key" pointer in each skip-list node.** Would make skipping O(1) but
  changes the skip list's insert path and memory layout, for a case the re-seek already handles.

## Consequences

- With the re-seek, the same model run takes 2.9 s, and moving past a key with 20,000 versions
  costs about 0.24 µs instead of 80 µs (same build, `Seek` then `Next`).
- The threshold is the same idea and default as RocksDB's `max_sequential_skip_in_iterations`.
  It is a constant for now; it becomes an option only if a benchmark shows a reason.
- Tests: `DBTest.IteratorSkipsManyVersionsByReseeking` covers both re-seek targets, and
  `DBIterTest.ReseekNextToSequenceZeroStillMakesProgress` covers rule 3. A wrong re-seek target
  that still lands on the same key is only slower, not wrong, so correctness tests cannot catch
  it; the model test's run time would.
- Phase 3's merging iterator (memtables plus table files) must keep the same rules; this ADR
  applies to it unchanged.

## References

- `src/db/db_iter.h`, `src/db/db_iter.cpp`
- RocksDB commit 197034e4 adding `max_sequential_skip_in_iterations` (default 8) and the re-seek
  to sequence number 0 in `DBIter::FindNextUserEntry` (viewed on a mirror of the repository):
  https://git.nextgraph.org/NextGraph/rocksdb/commit/197034e4c3674885f454bc15c21ff99973ebd472
