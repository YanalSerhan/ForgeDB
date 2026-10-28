# ADR 0007: The MANIFEST, and switching CURRENT without trusting the rename

- **Status:** Accepted
- **Date:** 2026-10-28

## Context

From Phase 3 the data lives in table files as well as logs, and the database must know, after any
crash, which tables are live, which logs still hold writes that no table has, the next file
number and the last sequence number. LevelDB keeps this in a MANIFEST (a log of version edits)
named by a small CURRENT file, and switches to a new MANIFEST by writing a temporary file and
renaming it over CURRENT.

On Windows that rename is the weak point (ADR 0002, open question): `MoveFileExW` with
`MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH` is the documented way to replace a file, but
Microsoft does not document that the rename itself survives a power loss once the call returns,
and there is no documented way to flush a directory. If recovery trusted CURRENT and the rename
reverted after the old MANIFEST had been deleted, the database could not be opened.

The fault-injection file system (Phase 2.4) undoes renames and removals on a simulated power loss
unless told otherwise, so whatever is decided can be tested as an ordinary crash outcome.

Byte layouts: `docs/file-formats.md`, "Version edit" and "MANIFEST and CURRENT".

## Decision

1. **A MANIFEST is a log** (the write-ahead log format) of version edits. Its first record is a
   full snapshot: log number, next file number, last sequence number and every live file. Later
   records are edits (files added or removed, new scalars).
2. **Every edit is synced before it takes effect** (`VersionSet::LogAndApply`), and every file an
   edit adds is synced before the edit is written (ADR 0002, rule 3). A table is never referenced
   before its bytes are durable.
3. **Every open starts a new MANIFEST** (`StartNewManifest`): it writes the snapshot, syncs it,
   then switches CURRENT: write `CURRENT.tmp` with the MANIFEST's name, sync it, rename it over
   `CURRENT` (ADR 0002, rule 5).
4. **Recovery does not trust CURRENT (option B of ADR 0002).** It lists the directory and uses
   the newest `MANIFEST-N` whose first record (the snapshot) is complete and passes its checksum.
   - A newer MANIFEST whose first record is incomplete (empty, or cut short) is skipped: it was
     being written when the process stopped, and CURRENT was never switched to it (rule 3).
   - Any other damage in the chosen MANIFEST (a bad record followed by valid ones, a malformed
     edit, an edit that removes a file that is not there) is `Corruption`; an unfinished last edit
     is a torn tail and is ignored (its flush never completed, and its log was not deleted).
   - CURRENT is read only as a hint and a consistency check: if it names a MANIFEST that exists
     but is incomplete, or if CURRENT exists and no MANIFEST is complete, that is `Corruption`.
5. **Files are deleted only after the MANIFEST that makes them obsolete is synced**: old logs
   after the flush's edit, and, at open, after the new MANIFEST and CURRENT: older logs and
   MANIFESTs, unlisted tables (a flush that crashed before its edit) and `CURRENT.tmp`.
6. **Recovery checks every listed table** exists with the recorded size before opening.
7. **The next file number** is above every file number in the directory, so a leftover file is
   never overwritten by a new one with its number.

Why "newest complete MANIFEST" is safe: a MANIFEST's snapshot is written only after everything it
lists is durable, and nothing it depends on is deleted until a newer MANIFEST is synced. So any
complete MANIFEST describes a database whose files exist; the newest one describes the latest of
them. Whether the rename of CURRENT survived does not matter.

## Alternatives considered

- **Option A: trust CURRENT** (LevelDB). Simpler, and almost certainly fine on NTFS, which
  journals metadata; but it rests on undocumented behavior, and a reverted rename after the old
  MANIFEST is deleted makes the database unopenable. Rejected: option B costs a directory listing
  and reading one or two extra MANIFEST headers at open.
- **No CURRENT at all.** Option B does not need it. Kept anyway: it is cheap, it lets a person (or
  a future tool) see which MANIFEST is current, and its consistency check catches some damage.
- **Reuse one MANIFEST across opens** (LevelDB's `reuse_logs`-style options). Fewer files and
  renames, but then the CURRENT switch is rarely exercised; a new MANIFEST at every open keeps the
  switch, the code path most likely to hide a crash bug, under constant test.
- **Write the MANIFEST without syncing each edit** and rely on the logs. Recovery would then not
  know whether a table it finds is complete. Rejected: one sync per flush is cheap.

## Consequences

- An open costs: listing the directory, reading the chosen MANIFEST (and the first record of any
  newer, incomplete one), replaying the newest logs, writing a table if anything was recovered,
  a new log, a new MANIFEST, `CURRENT.tmp`, a rename and the deletions. About 27 file operations
  for a small database (measured by `FlushCrashTest.ACrashAtAnyStepOfAnOpenLosesNothing`).
- Each open with unflushed writes adds a level-0 table, and nothing merges level-0 tables until
  compaction (Phase 4). Reads consider every level-0 table until then.
- Tested on the fault-injection file system with renames and removals that may be undone:
  `FlushCrashTest` crashes at every operation of a flush (524 operations for 120 writes with 2
  flushes) and of an open (27), each followed by a power loss and by a process crash;
  `CrashTortureTest.CrashesDuringFlushes` and `...WithInjectedErrors`;
  `RecoveryDurabilityTest.UndoneCurrentSwitchIsHarmless`; `tests/unit/version_set_test.cpp`
  covers every MANIFEST-selection rule.
- Not testable on real hardware: whether a power loss can revert the rename. Option B makes the
  answer irrelevant to correctness.

## References

- ADR 0002 (durability rules on Windows; this decides its open question); ADR 0004 (rule 6 depends
  on every open writing a new MANIFEST); ADR 0006 (table format).
- `src/db/version_set.*`, `src/db/version_edit.*`, `src/db/db_impl.cpp`.
- LevelDB's implementation notes: https://github.com/google/leveldb/blob/main/doc/impl.md
