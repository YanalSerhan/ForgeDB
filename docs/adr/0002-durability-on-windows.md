# ADR 0002: Durability rules on Windows

- **Status:** Proposed. Rules 1-10 and 12 are implemented in `src/env/` (Phase 0.4); rule 11, an
  assumption, was added in Phase 2.4; rule 13 in Phase 2.5. The open question at the end needs a decision before
  Phase 3. This ADR is the Phase 0 exit-gate item "reviewed line by line".
- **Date:** 2026-10-23

## Context

ForgeDB promises:

- A write acknowledged with `sync = true` survives a crash of the process, of the operating system,
  and a power loss.
- A write acknowledged without `sync` survives a crash of the process.
- After any crash, the database recovers to a consistent state: a prefix of the acknowledged
  writes, with no holes.

Keeping those promises requires knowing exactly what Windows guarantees. This ADR collects the
facts (each linked to Microsoft's documentation, quoted where wording matters), states the rules
ForgeDB follows, and is explicit about where the documentation is silent.

### What the documentation says

**F1. Writes land in a write-back cache.** "write operations write file data to the system file
cache rather than to the disk, and this type of cache is referred to as a write-back cache." A
lazy writer flushes it over time; if "a sudden system failure (such as a loss of power to the
computer) will happen before the flush … the cached data will be lost."
[File Caching][caching]

**F2. `FlushFileBuffers` forces a file's cached data to the device.** It "writes all the buffered
information for a specified file to the device or pipe." The handle needs `GENERIC_WRITE`. It "can
be inefficient when used after every write to a disk drive device when many writes are being
performed separately." [FlushFileBuffers][flush]

**F3. Metadata is cached too, and flushing the file stores it.** "File system metadata is always
cached. Therefore, to store any metadata changes to disk, the file must either be flushed or be
opened with `FILE_FLAG_WRITE_THROUGH`." [File Caching][caching]

**F4. The drive's own write cache.** Drives have caches of their own. Windows offers a device
setting, "Turn off Windows write-cache buffer flushing on the device"; its own warning says not to
select it "unless the device has a separate power supply that allows the device to flush its buffer
in case of power failure," and if the hardware doesn't qualify, "you risk data corruption."
[Raymond Chen, *The Old New Thing*][chen] (a Microsoft engineer's blog, not API documentation). How
`FlushFileBuffers` interacts with the drive's cache is not spelled out on its API page.

**F5. `MoveFileExW`.** `MOVEFILE_REPLACE_EXISTING` replaces an existing target.
`MOVEFILE_WRITE_THROUGH`: "The function does not return until the file is actually moved on the
disk. Setting this value guarantees that a move performed as a copy and delete operation is flushed
to disk before the function returns." **The page says nothing about atomicity, and its durability
guarantee is stated only for moves done as copy-and-delete (moves across volumes).** It doesn't
describe replacing a target that is open. [MoveFileExW][move]

**F6. Microsoft's pattern for atomic single-file updates:** "write the document to a new file,
then replace the original file with the new one", e.g. with `ReplaceFile`.
[Alternatives to using Transactional NTFS][txf]

**F7. There is no documented way to flush a directory.** `FlushFileBuffers` documents files,
pipes and whole volumes ("call FlushFileBuffers with a handle to the volume. The caller must have
administrative privileges"); it says nothing about directories. [FlushFileBuffers][flush]

**F8. Sharing modes.** Without `FILE_SHARE_DELETE`, "no process can open the file or device if it
requests delete access", and "Delete access allows both delete and rename operations." An open
whose sharing mode conflicts with an existing handle fails with `ERROR_SHARING_VIOLATION`.
[CreateFileW][create]

**F9. Deleting an open file.** Classic `DeleteFile` "marks a file for deletion on close … the file
deletion does not occur until the last handle to the file is closed. Subsequent calls to
CreateFile to open the file fail with ERROR_ACCESS_DENIED." With POSIX delete "the file [is]
deleted while handles remain open. Subsequent calls to CreateFile to open the file fail with
ERROR_FILE_NOT_FOUND." [DeleteFileW][delete] With `FILE_DISPOSITION_POSIX_SEMANTICS`, "the link is
removed from the visible namespace as soon as the POSIX delete handle is closed," while existing
handles keep access to the data. [FILE_DISPOSITION_INFORMATION_EX][posix]

**F10. Byte-range locks belong to a handle.** "If the locking process opens the file a second
time, it cannot access the specified region through this second handle until it unlocks the
region." When a handle closes with locks held, "the time it takes for the operating system to
unlock these locks depends upon available system resources. Therefore, it is recommended that your
process explicitly unlock all files it has locked." [LockFileEx][lock]

**F11. Positional reads.** On a handle opened without `FILE_FLAG_OVERLAPPED`, if an `OVERLAPPED`
is passed, "the read operation starts at the offset that is specified in the OVERLAPPED
structure." A read that "begins at or beyond the end of the file … fails with the error
ERROR_HANDLE_EOF." [ReadFile][read]

**F12. Path length.** "By default, the name is limited to MAX_PATH characters." [CreateFileW][create]

## Decision

Rules, each implemented in `src/env/win_file_system.cpp` unless noted.

1. **Durable means `FlushFileBuffers`.** `WritableFile::Flush` only hands the user-space buffer to
   the operating system (`WriteFile`): enough to survive a process crash. `WritableFile::Sync`
   additionally calls `FlushFileBuffers`: required for an OS crash or power loss. (F1, F2)
2. **`Sync` is expensive, so writers share it** (group commit, Phase 2.2). (F2)
3. **Sync before reference.** A file a durable structure points to (a log file, a table file, a new
   MANIFEST) is synced before anything refers to it. Because flushing a file also stores its
   metadata (F3), its size is durable too. *Interpretation:* F3 is read as covering the file's own
   directory entry as part of "its metadata changes"; the documentation doesn't spell this out.
4. **Never rely on flushing a directory** (F7). The MANIFEST lists every live file, and recovery
   deletes files it doesn't list, which removes leftovers from interrupted operations (Phase 3.6).
5. **Switching CURRENT:** write `CURRENT.tmp`, `Sync` it, then
   `MoveFileExW(tmp, CURRENT, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)`, following
   Microsoft's write-new-then-replace pattern (F6). The documentation does not promise that this
   rename is atomic or durable when it returns (F5); see the open question.
6. **Sharing modes** (F8):

   | Open for            | Sharing mode                          | Why                                                             |
   |---------------------|---------------------------------------|-----------------------------------------------------------------|
   | writing             | `READ \| DELETE`                      | readers may open it; no second writer; can be deleted or renamed |
   | reading             | `READ \| WRITE \| DELETE`             | can read a file still being written; delete never blocked by readers |
   | the LOCK file       | `READ \| WRITE` (no `DELETE`)         | see rule 9                                                       |

7. **POSIX delete.** `RemoveFile` opens the file for `DELETE` and calls
   `SetFileInformationByHandle(FileDispositionInfoEx, DELETE | POSIX_SEMANTICS)`, so the name
   disappears at once while open handles (e.g. the table cache) keep reading (F9). If the file
   system rejects it (`ERROR_INVALID_PARAMETER`, `ERROR_NOT_SUPPORTED`, `ERROR_INVALID_FUNCTION`),
   it falls back to classic delete-on-close. ForgeDB declares the one-field struct and information
   class 21 itself, because the SDK only declares them when targeting Windows 10 1607 or later.
   Class 21 was checked against both Microsoft's documented enumeration order and the MinGW headers.
8. **Retry transient sharing errors.** Creating (with truncation), renaming and deleting retry on
   `ERROR_SHARING_VIOLATION` and `ERROR_ACCESS_DENIED`: 10 attempts with 1, 2, 4, … 256 ms waits
   (about 0.5 s in total). Microsoft does not document why such errors appear transiently;
   antivirus scanners, the search indexer and backup tools opening files without
   `FILE_SHARE_DELETE` are the usual cause. This is a defensive measure, not a documented
   guarantee.
9. **One process per database.** The LOCK file is opened with `OPEN_ALWAYS` and locked whole with
   `LockFileEx(LOCKFILE_EXCLUSIVE_LOCK | LOCKFILE_FAIL_IMMEDIATELY)`. Locks belong to handles
   (F10), so a second open of the same database fails even inside one process; no in-process lock
   table is needed, unlike POSIX `fcntl` locks. The LOCK file is opened *without*
   `FILE_SHARE_DELETE`; otherwise, with POSIX delete, another process could delete it, create a new
   LOCK file under the same name and lock that one too. Unlocking calls `UnlockFileEx` before
   `CloseHandle` (F10).
10. **Errors are sticky.** After any failed write, flush or sync, a `WritableFile` returns that
    first error from every later call, and the database refuses further writes (Phase 2.2). The
    documentation doesn't say what is on disk after `FlushFileBuffers` fails, so the only safe
    assumption is "unknown."
11. **Directories are assumed durable once created.** No documentation says when a new
    directory's entry reaches the disk, and there is no documented way to flush it (F7). ForgeDB
    creates a database directory once and never removes it; a crash right after creation could, in
    principle, lose it with everything inside. *Assumption, not a documented guarantee*; the
    fault-injection model (Phase 2.4) makes the same assumption.
12. **Positional reads** pass their offset in an `OVERLAPPED` structure, so concurrent readers
    don't share a file position; `ERROR_HANDLE_EOF` means "0 bytes" (F11). Windows serializes
    synchronous I/O on one handle, so these reads are correct but not parallel; revisit in Phase 5
    if profiling shows contention.
13. **Syncing a closed file.** `FileSystem::SyncFile` opens an existing file with `GENERIC_WRITE`
    (`OPEN_EXISTING`, so nothing is created or truncated) and calls `FlushFileBuffers`. Recovery
    uses it to make every replayed log durable before new writes can depend on its data (found by
    the crash torture test, Phase 2.5). *Interpretation:* F2 says `FlushFileBuffers` writes "all
    the buffered information for a specified file"; the cache belongs to the file, so a new handle
    flushes data written through an earlier, closed one.

## Open question: does recovery trust the rename of CURRENT?

**Problem.** The plan assumes that once `MoveFileExW(… WRITE_THROUGH)` returns, CURRENT durably
names the new MANIFEST. The documentation doesn't say so for a same-volume rename (F5), and there is
no documented way to flush the directory to force it (F7). If CURRENT can revert to the old
MANIFEST after a power loss, and ForgeDB has already deleted files that only the old MANIFEST lists
(compaction inputs, the old MANIFEST itself), the database cannot be recovered.

**Option A: trust it** (the current plan). Rely on NTFS journaling its metadata and on
`MOVEFILE_WRITE_THROUGH`. Simple and widely done, but it rests on behavior Microsoft doesn't
document.

**Option B: don't depend on it** (proposed). Use only the documented guarantees (F2, F3):
1. A new MANIFEST is completely written and `Sync`ed before CURRENT is switched (rule 3).
2. Recovery treats CURRENT as a hint: it verifies the MANIFEST CURRENT names, and also checks for a
   newer `MANIFEST-N` whose records verify (CRC and a complete first snapshot record). It uses the
   newest valid one.
3. Files made obsolete by a new MANIFEST are deleted only after that MANIFEST is synced, which, given
   step 2, is enough.

Option B costs a little complexity in Phase 3.6 (directory scan, verification) and gives a
correctness argument that uses only documented behavior. The fault-injection file system
(Phase 2.4) can then model "the rename didn't persist" as an ordinary crash outcome and test it.

**Decision needed** from the project owner before Phase 3.

## Alternatives considered

- **`FILE_FLAG_WRITE_THROUGH` / `FILE_FLAG_NO_BUFFERING` instead of `FlushFileBuffers`.** Microsoft
  recommends unbuffered, write-through I/O for many small durable writes (F2). It forces sector-aligned
  buffers and makes every write synchronous to the device, which defeats group commit. Kept as a
  Phase 5 experiment (stretch goal: direct I/O for tables).
- **`ReplaceFile` for CURRENT** (F6). Designed for replacing documents while preserving attributes,
  with more failure modes (backup file, partial states documented on its page) than a plain rename
  needs. Not chosen; can be revisited with the open question.
- **Classic delete only.** Would forbid reusing a file name while any handle is open and makes
  deleting files the table cache holds fail or linger. POSIX delete matches what the engine needs.
- **Flushing the volume to persist directory changes.** Requires administrative privileges (F7);
  ForgeDB must run as a normal user.

## Consequences

- `WinFileSystem` and `MemFileSystem` share one conformance suite
  (`tests/unit/file_system_conformance_test.cpp`); Windows-specific behavior (retry, UTF-8 errors,
  handle ownership) is tested in `tests/unit/win_file_system_test.cpp`.
- **Not provable by tests on real hardware:** power-loss durability. It rests on F1-F4 and is
  exercised by the fault-injection model (Phase 2.4, `tests/support/fault_injection_file_system.h`);
  the process-kill test (Phase 2.6) covers process crashes only.
- **The fault-injection model trusts only these rules.** After a simulated power loss, synced
  bytes survive (F2, F3); a name survives once its file was synced under it (rule 3); a rename or
  a removal may be undone, unless the `durable_renames` / `durable_removals` options say otherwise
  (F5, F7); directories survive (rule 11). The open question below can therefore be tested both
  ways: option A with `durable_renames = true`, option B with the default.
- **Limitations** (also in `TODO.md`, "Known limitations"):
  - Durability depends on the drive honoring flush requests; the write-cache setting in F4 voids it.
  - File systems without POSIX delete (FAT32, some network shares) get classic delete-on-close:
    removed files keep their names until the last handle closes. Observed: Wine 9.0 rejects
    `FileDispositionInfoEx` with `ERROR_INVALID_PARAMETER`, so under Wine the conformance test
    "RemoveWhileOpenKeepsHandleAndFreesName" fails by design.
  - Paths are limited to `MAX_PATH` unless long paths are enabled (F12).
- Revisit when the open question is decided, or if Microsoft documents rename durability.

## References

[caching]: https://learn.microsoft.com/en-us/windows/win32/fileio/file-caching
[flush]: https://learn.microsoft.com/en-us/windows/win32/api/fileapi/nf-fileapi-flushfilebuffers
[chen]: https://devblogs.microsoft.com/oldnewthing/20130416-00/?p=4643
[move]: https://learn.microsoft.com/en-us/windows/win32/api/winbase/nf-winbase-movefileexw
[txf]: https://learn.microsoft.com/en-us/windows/win32/fileio/deprecation-of-txf
[create]: https://learn.microsoft.com/en-us/windows/win32/api/fileapi/nf-fileapi-createfilew
[delete]: https://learn.microsoft.com/en-us/windows/win32/api/fileapi/nf-fileapi-deletefilew
[posix]: https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/ntddk/ns-ntddk-_file_disposition_information_ex
[lock]: https://learn.microsoft.com/en-us/windows/win32/api/fileapi/nf-fileapi-lockfileex
[read]: https://learn.microsoft.com/en-us/windows/win32/api/fileapi/nf-fileapi-readfile

- [File Caching][caching]
- [FlushFileBuffers][flush]
- [The Old New Thing: "Dangerous setting is dangerous"][chen]
- [MoveFileExW][move]
- [Alternatives to using Transactional NTFS][txf]
- [CreateFileW][create]
- [DeleteFileW][delete]
- [FILE_DISPOSITION_INFORMATION_EX][posix]
- [LockFileEx][lock]
- [ReadFile][read]
- [FILE_INFO_BY_HANDLE_CLASS](https://learn.microsoft.com/en-us/windows/win32/api/minwinbase/ne-minwinbase-file_info_by_handle_class)
- [SetFileInformationByHandle](https://learn.microsoft.com/en-us/windows/win32/api/fileapi/nf-fileapi-setfileinformationbyhandle)
- Code: `src/env/file_system.h`, `src/env/win_file_system.cpp`, `src/env/mem_file_system.cpp`
