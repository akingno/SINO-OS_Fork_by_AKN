# Shell workspace

The [text console](text-console.md) automatically opens a writable workspace.
The Shell, tools and console host have caps=0.

## Trusted grant and broker

SYS_WORKSPACE=0x14c takes zero arguments and returns a directory grant fixed to
/home/user. It is denied to strict clients and ordinary compatibility programs.
The kernel grants this operation only to the console host it starts from
`/boot/conhost.snx`. The host passes `shell-write` as argv[0] and delegates
home rights without DELEGATE. The session pins that root and
its rights as the existing broker ceiling. Nonzero worker grants must still use
the exact same pinned directory object. Write intents use catalog6, resolved to
read-only /boot/writer.snx. Catalog4 accepts no directory rights; catalog5 and
the legacy diagnostic workers accept read-only rights. Existing type, request
identity, role, attenuation and foreground rules remain in force.

Shell read-only mode rejects write commands before submission. Writable Shell
normalizes every source/destination path against its own logical cwd, then
requests only command-specific rights. / still means the home grant root;
there is no global-path fallback. The root itself is trusted to manipulate its
explicit grant, not made incapable of writing by merely delegating work to tools.

## Commands

| Command | Contract |
| --- | --- |
| `touch path` | Create an empty file if absent, preserve existing contents |
| `mkdir path` | Create one directory; existing target fails; no implicit -p |
| `rm path`, `rmdir path` | Remove exact file/empty directory; no recursive removal |
| `write path text...` | Join text arguments with spaces and add LF; staged replacement |
| `append path text...` | Version-checked staged replacement of old content plus joined text and LF |
| `cp source target` | Binary range copy into a stage, then publish; existing directory target appends source basename |
| `mv source target` | Rename; directory target appends basename; existing file target fails |
| `sync` | Confirm a home snapshot through the granted directory |

Ordinary operation failure returns1, syntax/argument errors2. Same-path cp/mv
is rejected. cp replacement validates target version and Busy; source reads
validate generation/revision/length. append is conflict-detecting replacement,
not multiwriter atomic append. Files remain limited to64KiB; copies use512-byte
buffers and private staging pages. Closing/exiting/canceling before publication
discards the stage. Publication is RAM-visible and is not rolled back by a later
process exit or cancellation. Busy reference protection is retained, including
directories held by another Shell's cwd. Normal modifications print
`updated` when publication is involved. The writable gsh commits its root directory after successful writer/boot-copy completion, before issuing the next prompt. Commit rights remain with the Shell rather than being added to every worker. Save failure prints an explicit error and sets command status to 1; sync remains available for retries. Low-level FilePublish and generic VFS writes retain RAM-only semantics.

## Persistence

SYS_DIRECTORY_COMMIT=0x14d uses EBX=directory, ECX=CommitInfo output,
EDX=capacity>=32. The directory needs COMMIT and must be home or a tracked home
descendant. It submits the existing global dual-bank home snapshot, not a
per-directory isolated transaction. Other home changes may also be committed.
The32-byte report uses published=0/revision=0 (no stage publication), durable=1
and the snapshot generation on success. I/O failure gives durable=0 and S_IO;
invalid handles, pointers and unauthorized directories leave output unchanged.
Strict clients use `DirectoryCommit` with an explicit grant.

`FEATURE_WORKSPACE=512` advertises the workspace grant and commit operations.

For boot-to-home copies, the writer receives its normal home target grant and
a separate read-only boot grant. `StartBootDirectory()` uses selector 1 of
`SYS_START_DIRECTORY`. Selector 0 returns the primary grant.

Run `test-console.js <ordinary build> --persistence` for the three autosave
checks. `regression.ps1 -Suite persistence` checks explicit commits.
