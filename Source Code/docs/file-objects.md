# Stage D: directory grants and versioned file saves

This extends the existing VFS without replacing legacy application APIs. Strict
processes still cannot use ambient Read/Write/List/Sync, Spawn, the clipboard or
desktop proxies. FEATURE_FILE_OBJECTS=32 advertises these mechanisms; features
totaled 63 at Stage D (A-F now total 255). A feature bit never grants access. New IDs are appended after
Stage C's 0x13a; old syscall numbers, query sizes and RunSpec v1 remain stable.
S_CHANGED=-11 is appended to the existing errors.

## Directory authority and startup

`GrantDirectory(absolute_path, rights)` is compatibility-only. Read grants use
the caller's existing ambient read authority. Any WRITE/CREATE/REMOVE/COMMIT
grant requires CAP_HOME and a resolved directory at or below /home/user.
Containment uses VFS ancestry, not a string prefix. Writable /boot grants are
denied. Strict callers receive S_DENIED before pointer access.

Rights are independent bits: READ=1, WRITE=2, LOOKUP=4, ENUM=8, CREATE=16,
REMOVE=32, COMMIT=64, DELEGATE=128. LOOKUP permits resolution, ENUM permits
enumeration, CREATE permits new entries, REMOVE permits removal/source rename,
and DELEGATE permits explicit transfer to a strict child. READ/WRITE govern file
contents. A directory's rights bound the rights of objects opened below it.
Regular files may only be opened with READ and/or WRITE. FileGetStat permits
metadata queries on any owned file/directory/staging handle, including handles
without content-read rights. Closed, stale, wrong-type or insufficient-rights
handles return S_BADHANDLE. Dup can only reduce rights and shares the object.

RunSpecV2 is 64 bytes: the original 48-byte RunSpec (size=64, version=2), followed
by directory, directory_rights, reserved[2]=0. StartRunWithDirectory uses the same
SYS_RUN_START with ECX=64. A zero directory requires zero rights; otherwise the
source handle must have DELEGATE plus every requested bit. The child receives
only the requested bits, including DELEGATE only when explicitly requested.
No other file/directory handles or ambient caps are inherited. The child remains
strict and cannot spawn; this is not general process delegation.

StartDirectory returns the child's handed-in directory handle, without making
a duplicate. No grant returns S_NOTFOUND. Closing it makes later queries return
S_BADHANDLE; it is not automatically recreated. Launch failure, child exit,
fault, cancellation and timeout release transferred references. A retained
completion record does not retain a finished child's directory grant.

The startup directory is an immutable per-process reference for explicit OpenAt
operations, not POSIX cwd/chdir. There is no environment block or implicit path
resolution. Existing kernel Shell cwd is unchanged. Host run.ps1/v2 and old
Shell launch syntax do not yet request a directory grant; compatibility native
launchers can use RunSpecV2. No host directory is mounted or authorized here.

## Opening and range I/O

OpenAt(directory, relative_path, rights, flags) resolves components under that
specific handle, never under the global cwd. Absolute paths, drive colons,
backslashes and control bytes are rejected. Repeated '/' and '.' are accepted;
'..' may traverse within the anchor but attempting to leave it returns S_DENIED
even when later components would reenter. Missing intermediate components and
traversal through files return S_INVALID; a missing final name returns
S_NOTFOUND. /boot lookup retains VFS case folding. No symlinks are introduced.
Paths including NUL fit 128 bytes; individual names including NUL fit 32 bytes.
A trailing slash requires an existing directory.

Flags: CREATE=1, EXCLUSIVE=2 (requires CREATE), TRUNCATE=4 (requires file WRITE),
DIRECTORY=8. Unknown flags/rights and zero rights are invalid. The requested kind
must match an existing node. CREATE needs directory CREATE and LOOKUP in addition
to requested rights. It creates only the final component. EXCLUSIVE reports
S_EXISTS for an existing entry in the same kernel operation. Objects and local
handles are reserved before creation/truncation; exhaustion must not leave a
created file. TRUNCATE on an existing file sets length to zero. Ordinary writes,
including legacy whole-file writes, may change an opened file's contents.

FileReadAt/FileWriteAt use explicit offsets and do not maintain a shared cursor.
Writes return the byte count; legacy whole-file Write still returns 0. Range
offset and length must each fit within the 64 KiB boundary using subtraction
checks to reject overflow. Valid zero-length I/O returns 0 without requiring a
buffer. Reads stop at EOF, including offsets beyond the current length. Writes
grow without truncating, zero-fill gaps and allocate all growth pages before
altering visible contents. Allocation failure returns S_NOMEM and preserves
the old size/content. Disk/boot reads use FAT range traversal without a 64 KiB
stack buffer. A FAT read failure can leave a partially written destination.
This does not lift file/path/node limits or provide append/stream adapters.

FileStat is eight u32 fields (32 bytes): size=32, version=1, kind, length,
generation, revision, readonly, reserved=0. Kinds: regular=1, directory=2,
private staging=3. Generation identifies a node lifetime; revision identifies a
content/metadata version. Unpublished staging reports generation=revision=0.
Both counters are boot-local. Counter exhaustion fails closed rather than
wrapping; revisions are not stored in snapshots or meaningful across reboots.

Objects pin the node generation until their last reference closes. VFS remove
and move inspect references for both new and legacy callers. Open targets cannot
be deleted/replaced; moves that affect any open descendant or directory anchor
return S_BUSY. Legacy SYS_REMOVE/MOVE preserve S_BUSY for this case. Other old
VFS errors retain their old mapping. This is not POSIX unlink-while-open.

## Directory enumeration

DirectoryRead takes an in/out DirPage: version, cursor, buffer (user address),
capacity (entries), count, done; 24 bytes. Start with version=cursor=0 and
capacity 1..128. Entry layout is name[32], length, dir, revision (44 bytes).
Only count entries are written. The entry buffer cannot overlap the header.
Continue using the returned version/cursor until done=1, including when a full
batch happened to be the last one. Slot-based cursors are valid only with that
directory version, not stable file identifiers or security tokens.

Namespace changes and direct children's content/size changes update the
directory revision. A mismatching version returns S_CHANGED without changing
the header or entries; restart from zero. A caller-supplied version is not a
claim that the kernel retains a snapshot. Private staging never appears here.

RemoveAt uses LOOKUP|REMOVE; its is_directory argument must be 0 or 1. RenameAt
requires source LOOKUP|REMOVE and destination LOOKUP|CREATE. Destination is an
exact missing name, rather than the old mv convention of moving inside an
existing directory. Existing destinations return S_EXISTS; directory cycles and
invalid moves are rejected. Busy/protected objects remain unchanged. First
version maps other failed remove conditions (such as a nonempty directory) to
S_BUSY and other failed rename conditions to S_INVALID.

## Private staging, publication and durability

FileStage(directory, relative_path, expected_revision) requires
LOOKUP|CREATE|WRITE. expected_revision=0 requires an absent destination; otherwise
it must equal the current file revision. The stage records the target generation
as well, so slot reuse or an absent-then-created target cannot bypass the check.
Its separate, initially empty pages are outside the VFS and auto snapshots.
ReadAt/WriteAt work on those private pages until publication. Close/exit/fault
before publication discards them. Stage allocation pins the parent anchor but
does not pin the old target. Inherited COMMIT authority is copied onto the stage
only if the source directory handle has COMMIT.

FilePublish validates generation/revision, write protection and target open
references again. Conflict returns S_CHANGED; an open target returns S_BUSY.
Failure preserves the current target and keeps the private stage retryable.
Successful publication transfers prepared pages in one non-preemptible operation,
updates file/directory revisions and pins the published identity until Close.
Other callers cannot observe a half-written version, but the change is only
RAM-visible. Thereafter ReadAt/WriteAt on the stage return S_BUSY; reopen the
published path for content access. Repeating publication checks the same
published version without republishing it. Ordinary/legacy writes may still
change that version, making subsequent publish/commit return S_CHANGED.

FileCommit requires stage COMMIT, successful publication and an unchanged target
revision. Before publication it returns S_AGAIN. It calls the existing global
storage_sync, retaining its dual-bank payload/flush/header/flush sequence. It
confirms at least that target revision, and can save other home changes as well;
it is not per-file isolation. Nonpersistent targets cannot be confirmed.
Successful commit is repeatable and retains its acknowledged snapshot generation.

Both calls return a CommitInfo (32 bytes): size=32, version=1, published,
revision (published file revision), durable, generation (acknowledged storage
snapshot generation), status (signed return code), reserved=0. With a valid
authorized stage and valid output range, this report is written on errors too.
Bad handle, insufficient rights or invalid output leaves output unchanged.
This is an explicit exception to ordinary query failure-output rules.

published=1,durable=0 with S_IO means RAM has the published version but disk
durability is unconfirmed: retry on the same stage. It does not prove the disk
retains the old version. Auto sync may have saved it without this handle yet
acknowledging it. Once durable=1, it remains historical confirmation for that
reported revision even if another write later causes S_CHANGED; it is not a
claim that the current RAM revision is unchanged. Close does not roll back a
published version. No new crash or hardware durability guarantee is asserted
without fault/restart testing.

## Wire register layout

All request addresses and handles below are u32; every unspecified reserved
argument must be zero. Fixed request sizes are exact; query output capacity may
be larger than defined output, with the extra bytes left untouched.

| ID | Call | EBX | ECX | EDX |
| --- | --- | --- | --- | --- |
| 0x13b | DIR_GRANT | absolute path | rights | 0 |
| 0x13c | START_DIRECTORY | 0 | 0 | 0 |
| 0x13d | OPEN_AT | directory | OpenRequest* | 12 |
| 0x13e | FILE_READ_AT | file/stage | FileIo* | 12 |
| 0x13f | FILE_WRITE_AT | file/stage | FileIo* | 12 |
| 0x140 | FILE_STAT | file/directory/stage | FileStat* | capacity >=32 |
| 0x141 | DIR_READ | directory | DirPage* | 24 |
| 0x142 | REMOVE_AT | directory | relative path | is_directory |
| 0x143 | RENAME_AT | RenameSpec* | 16 | 0 |
| 0x144 | FILE_STAGE | directory | StageSpec* | 8 |
| 0x145 | FILE_PUBLISH | stage | CommitInfo* | capacity >=32 |
| 0x146 | FILE_COMMIT | stage | CommitInfo* | capacity >=32 |

OpenRequest: path, rights, flags. FileIo: offset, buffer, bytes. RenameSpec:
source_dir, source_path, target_dir, target_path. StageSpec: path,
expected_revision. No kernel pointer or node index crosses the API.

## Validation and remaining boundaries

fstest.snx suite is registered in the independent non-GUI files group. It exercises real
CPL3 grants/attenuation, path escape rejection, EOF/holes/cross-page/64 KiB I/O,
old/new reference protection, enumeration versions, private stages, Busy and
revision conflicts, handle exhaustion, child launch rollback and child fault
cleanup. The host compares object/page baselines and expects one intentional
child #UD. It publishes a distinct new 257-byte contracts-file-save.bin; the host
checks exact bytes in the CRC-valid snapshot, and fstest.snx reboot reads them
after a cold guest boot. Only that new file's one page may remain above the
previous resource baseline. Existing persistence fixtures remain unchanged.

Additional validation must cover allocation-point
failure injection, node/counter exhaustion, forged headers/pointers and output
canaries, cancellation/timeout while holding file objects, all commit failure
points and private-stage invisibility across a forced reboot. The core regression alone does not prove every such failure case or unchanged old binaries.

G5b adds explicit `run /boot/term.snx /write` for touch/mkdir/rm/rmdir/write/append/cp/mv/sync using a granted home workspace. Read-only sessions and catalogs 4 and 5 keep their previous access scope. Writable-mode authority, publication versus persistence and intentional compatibility differences are in [workspace-shell.md](workspace-shell.md).
