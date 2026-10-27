# ADR 0004: Log recovery tells a torn tail from mid-log corruption, and stops on corruption

- **Status:** Accepted; amended in Phase 2.7 (rule 5)
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
5. *(Amended in Phase 2.7.)* When the damaged fragment's **header is intact** (a known type and a
   length that fits its block; the problem is then a checksum mismatch, or a payload cut off by
   the end of the file), the search starts **after that fragment's payload**, not inside it: the
   payload is user data and may contain bytes that form a valid fragment. Instead, its CRC decides
   whether the length field is damaged: if a prefix of the payload shorter than the stated length
   matches the CRC, the real fragment ended there and data follows it, so it is corruption
   ("fragment length field damaged"). A torn fragment's CRC covers its whole, never-finished
   payload, so no prefix matches it, except by a coincidence of about 1 in 2^32 per length tried.

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
  known to us. The fault-injection file system (Phase 2.4) keeps a prefix of the unsynced data plus
  a torn final write by default, so it does not produce this case; its
  `zero_pages_in_unsynced_tail` option does (zeroed 4 KiB pages followed by surviving data).
  *Measured in 2.5:* with that option the crash torture test sees this rule refuse to open the
  database after some power losses (17 of 600 cycles in the first run; in a targeted experiment
  with ~33 KB of unsynced log, 239 of 500). Never wrong data: the refusal is `Corruption`.
  *Decided in 2.5:* keep this rule for now and cure it in Phase 3.6, where open writes the
  recovered data to a synced table and the MANIFEST, so a log is replayed only while it is the
  newest; the newest log can then stop at its first problem (point-in-time, reported) without
  ever being read again under the strict rule. This ADR will be amended then.
- *Found by fuzzing in Phase 2.7, fixed by rule 5:* before rule 5, a torn record whose own
  payload contained bytes forming a valid fragment (7 bytes suffice: an empty FULL fragment; or a
  log file stored as a value) was reported as corruption, so a crash at the wrong moment left a
  database that would not open, triggered by ordinary user data. The crash torture test now also
  runs with such values (`CrashTortureTest.ValuesContainingLogFragments`).
- What rule 5 gives up: a damaged length field is caught only through the CRC. A false alarm is
  still possible by coincidence (above), and deliberately: CRC32C is not cryptographic, so someone
  who controls values can craft a payload whose prefix matches its CRC, and a crash tearing that
  record makes the open fail with `Corruption`. No checksum-based format prevents that.
- Tests: `tests/unit/log_test.cpp` (every truncation point, also with logs stored as values; every
  single-bit flip; zero and garbage tails; out-of-order fragments; damage followed by valid
  records; payloads holding valid fragments; damaged lengths). Fuzzing: `fuzz/fuzz_log_reader.cpp`
  (arbitrary bytes, and write / cut / read back, which must return exactly the complete records).

## References

- `src/log/log_reader.h`, `src/log/log_reader.cpp`
- ADR 0002 (durability rules on Windows)
