# ADR 0008: Leveled compaction: what is compacted, what is dropped, and when files go

- **Status:** Accepted
- **Date:** 2026-10-29

## Context

Every flush adds a level-0 table, and until Phase 4.3 nothing merged them: a `Get` for a missing
key asked every table's bloom filter, an iterator merged every table, and old versions and
deletions were kept forever. Compaction merges tables into fewer, larger, non-overlapping ones and
drops what no reader can see. It must never lose a version that a read (current, or at a live
snapshot: Phase 4.1) can still see, never bring back a deleted key, and survive a crash at
any point, with the same MANIFEST rules as flushes (ADR 0007).

LevelDB's design ([impl.md](https://github.com/google/leveldb/blob/main/doc/impl.md),
"Compactions") is the model. Its known pitfalls shaped three of the decisions below.

## Decision

1. **Levels and scores.** Seven levels. Level 0 scores its table count divided by
   `Options::level0_compaction_trigger` (4); levels 1 to 5 their bytes divided by a target,
   `max_bytes_for_level_base` (10 MiB) for level 1 and ten times more for each level below; level
   6 is the last and is never compacted on its own. The background thread compacts the
   highest-scoring level of 1 or more, one compaction at a time, after any queued flush. A long
   compaction runs a queued flush between its entries.
2. **Inputs.** From level 0, **every** table (level-0 tables overlap; moving only some down could
   put an older version of a key above a newer one; LevelDB picks overlapping subsets, which is
   correct but harder to get right). From level n ≥ 1, one table, round-robin by a per-level
   pointer kept in memory only (a reopen restarts it; persisting it would need a new MANIFEST
   field for no correctness gain). Plus every table of level n+1 whose user-key range overlaps
   them. A single table with no overlap below moves down without being rewritten (a "trivial
   move").
3. **Outputs are cut only between user keys**, at about `target_file_size` (2 MiB). So in levels
   1 and below no user key is split between tables, and taking one table takes every version of
   its keys. (LevelDB cut anywhere and later needed "boundary inputs" to avoid moving the newer
   half of a key's versions below the older half.) A key with a huge history makes a larger table.
4. **Drop rules.** S is taken under the DB mutex when the compaction starts: the oldest live
   snapshot's sequence number, or the latest sequence number if none is alive. Entries arrive
   newest first per user key.
   - (a) An entry is dropped when the newer entry of the same key before it has a sequence number
     ≤ S: that entry is visible to every snapshot and to current reads, so this one never is.
   - (b) A deletion with sequence number ≤ S is dropped when no level below the output level has
     a table whose range holds the key: no older version exists outside the inputs, and those in
     the inputs are dropped by (a).
   - A snapshot T between two versions has S ≤ T < the newer version's number, so the version T
     reads (and any deletion it must see) is kept; a snapshot taken during the compaction has a
     number ≥ S and reads the newest versions, which are always kept.
   - Only the oldest snapshot is consulted: a version hidden from every snapshot but newer than
     the oldest one stays until that snapshot is released (RocksDB checks each snapshot boundary;
     not needed for correctness).
5. **Installing and deleting.** Outputs are synced, then one `VersionEdit` (inputs removed,
   outputs added) is appended to the MANIFEST and synced, then the new version is installed. An
   input table is deleted only when (i) no version still in use lists it (`VersionSet::LiveFiles`:
   a read keeps its version, and so its tables), and (ii) another MANIFEST record has followed
   the edit that removed it. (ii) is ADR 0007 rule 5 applied to tables: while the edit is the
   last record, damage to it is ignored like a torn write, and recovery then needs the inputs.
   Deleted tables leave the table cache first; Windows deletes a file that is still open (ADR
   0002, rule 7).
6. **Write stalls.** With `level0_slowdown_writes_trigger` (8) level-0 tables or more, each write
   sleeps 1 ms once, without the mutex; with `level0_stop_writes_trigger` (12) or more, a write
   that needs a new memtable waits for a compaction. Open checks 1 ≤ compaction trigger ≤
   slowdown ≤ stop. Stalls are counted (`DB::GetStats`, since Phase 4.4).
7. **Errors and shutdown.** A failed compaction becomes the sticky background error (writes
   stop, reads go on, a reopen recovers). It removes its outputs only if it failed before writing
   its MANIFEST edit: if appending or syncing the edit fails, the edit may still be in the file and
   survive a crash, so the outputs it lists stay (the next open deletes them if the edit is not
   there). (Found by the crash torture test; `CompactionErrorTest.AFailedManifestSyncKeepsTheOutputs`.) Shutdown abandons a running
   compaction at its next check (every 256 entries) and removes its outputs; the next open
   deletes anything left unlisted.

## Alternatives considered

- **Compact a subset of level 0.** Smaller compactions, but the subset must be chosen so that no
  older overlapping table stays above a newer one. Rejected for now: all of level 0 is at most
  `level0_stop_writes_trigger` tables.
- **Persist the compaction pointers.** Rejected: one more MANIFEST field, and the only effect of
  losing them is which table goes next.
- **Delete inputs right after the edit.** Rejected for the reason in 5 (ii); the cost is keeping
  one compaction's inputs until the next flush or compaction.
- **Universal (tiered) compaction.** Lower write amplification, more space and read cost.
  Leveled is simpler to reason about, and the classic choice.
- **Grandparent-overlap limits on output tables** (LevelDB cuts an output early if it would
  overlap too much of level n+2). Not done: a performance refinement for later phases.

## Consequences

- Level 0 stays small, deeper levels have one table per key range, `Get` reads at most one table
  per level below 0 (binary search), and iterators open deeper tables one at a time.
- Tests: `CompactionPickTest` (scores, inputs, round-robin, base level), `CompactionTest` (the
  drop rules read back from the raw tables, with and without snapshots; a deletion kept while an
  older version is deeper; when inputs are deleted; a damaged last compaction record; a frozen
  snapshot view; stalls), `CompactionErrorTest`, `CompactionShutdownTest`,
  `CompactionCrashTest` (a crash at each of about 1,000 file operations of two compactions' worth
  of writes), `CrashTortureTest.CrashesDuringCompactions` and `...BackgroundCompactions`, the
  model test `AgreesWithStdMapAcrossCompactions` (data in levels 1 to 3, with snapshots), and the
  corruption suite with small levels.
- Thresholds are `Options` fields; tuning them is Phase 5.

## References

- ADR 0002 (durability), ADR 0007 (MANIFEST; rule 5 extended here), TODO 4.3.
- `src/db/compaction.*`, `src/db/db_impl.cpp` (`RunCompaction`), `src/db/level_iterator.*`.
- LevelDB, "Compactions": https://github.com/google/leveldb/blob/main/doc/impl.md
