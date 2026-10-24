# ADR 0004: Log recovery tells a torn tail from mid-log corruption, and stops on corruption

- **Status:** Accepted
- **Date:** 2026-10-24

## Context

Recovery replays the write-ahead log (`docs/file-formats.md`, "Write-ahead log"). A crash can
leave the end of the log damaged: a record cut off by the end of the file, a chain of fragments
that never finishes, garbage from a torn write, or zeros where the file was extended but never
written. Those records were never acknowledged as synced, so dropping them is correct. Damage
elsewhere (a flipped bit on disk, a bad sector, a bug) is different: records after it may hold
acknowledged, synced writes.

The reader cannot know where the last `Sync` ended, so it needs a rule that uses only the file.

## Decision

1. A problem in the log (incomplete header or payload, bad length, unknown type, checksum
   mismatch, zero-filled space, fragments in an illegal order) is a **torn tail** if and only if
   no valid fragment exists anywhere after it in the file. Otherwise it is **mid-log corruption**.
2. A torn tail ends the log: the incomplete record is dropped, replay continues normally, and the
   reader reports where the tail began (`Reader::IgnoredTail`).
3. Mid-log corruption fails recovery with `Corruption`, the offset and the reason. Opening the
   database fails rather than silently dropping data (the **strict** policy).
4. The search for a later valid fragment checks every byte offset in the rest of the problem's
   block and in the next block, then only the start of each later block, where the writer always
   starts a fragment. Cost: linear in the damaged region.

## Alternatives considered

- **Point-in-time recovery** (RocksDB's default WAL recovery mode): replay up to the first problem,
  keep that consistent prefix, warn. Opening never fails, but real mid-log damage silently loses
  the synced writes after it. Not chosen as the default; it can be added as an option later on
  top of the same classification.
- **Skip damaged records and continue** (LevelDB's lenient mode): keeps more data, but the result
  can have holes: a later write applied without an earlier one. Breaks prefix consistency.
- **Treat every problem as the end of the log**: simplest, same silent loss as point-in-time.

## Consequences

- No silent loss of synced data from mid-log damage: the user gets an error with an offset.
- A false alarm is possible if a power failure persisted a later unsynced block but not an earlier
  one. Whether NTFS can do that for an append-only file is not documented by Microsoft; it is not
  known to us. The crash-torture model (Phase 2.4/2.5) keeps a prefix of the unsynced data plus a
  torn final write, so it does not produce this case.
- A torn record whose own payload contains bytes that form a valid fragment (e.g. a log file
  stored as a value) is reported as corruption: a false alarm, never a silent loss.
- Tests: `tests/unit/log_test.cpp` (every truncation point, every single-bit flip, zero and garbage
  tails, out-of-order fragments, damage followed by valid records). Fuzzing in Phase 2.7.

## References

- `src/log/log_reader.h`, `src/log/log_reader.cpp`
- ADR 0002 (durability rules on Windows)
