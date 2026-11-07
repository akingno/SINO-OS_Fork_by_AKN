# G5b explicitly writable Shell workspace

F2, the SINO menu and Terminal.app open the writable workspace. F12 opens a
read-only session. The frontend also accepts the explicit /write argument.
The Shell, tools and frontend have caps=0. See [desktop-shell.md](desktop-shell.md).

## Trusted grant and broker

SYS_WORKSPACE=0x14c takes zero arguments and returns a directory grant fixed to
/home/user. It is denied to strict clients and ordinary compatibility programs.
The kernel records a trusted-frontend bit only when compatibility launch resolves
the executable to the read-only boot /boot/term.snx node. A writable copy,
a matching filename or a caller-provided string does not obtain this bit.
The trusted frontend remains outside the strict security boundary; caps=0 alone
never described all its authority. No ambient CAP_HOME is added to it.

Only the explicit writable mode calls this operation. Its root receives home
rights without DELEGATE and argv[0]=shell-write; the session pins that root and
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

| Command | First-version contract |
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
`published in RAM; sync confirms disk` when publication is involved.

## Persistence

SYS_DIRECTORY_COMMIT=0x14d uses EBX=directory, ECX=CommitInfo output,
EDX=capacity>=32. The directory needs COMMIT and must be home or a tracked home
descendant. It submits the existing global dual-bank home snapshot, not a
per-directory isolated transaction. Other home changes may also be committed.
The32-byte report uses published=0/revision=0 (no stage publication), durable=1
and the snapshot generation on success. I/O failure gives durable=0 and S_IO;
invalid handles, pointers and unauthorized directories leave output unchanged.
The old ambient Sync remains denied to strict clients.

FEATURE_WORKSPACE=512 made the G5b feature mask 1023; G5c adds LINE_EDIT=1024. Earlier ABI numbers,
structures, read-only modes and C finite captures are unchanged. This stage does
not add pipelines, redirection, POSIX unlink lifetimes; [shell-editing.md](shell-editing.md) describes the separately delivered line editor.

普通dev-20261105-210318-d95daeff编译通过：kernel152096 bytes，BSS末端1884832。普通镜像私有VM完成写/追加/cp/mv/sync，宿主核对快照精确字节，冷启动用当时的只读入口重新读取一致。截图已检查，VM已关闭。

G6 adds boot-to-home copy. A dedicated writer startup receives its normal home
target grant and a separate read-only boot source. `StartBootDirectory()` uses
selector 1 of SYS_START_DIRECTORY. Selector 0 returns the primary grant.
See [desktop-shell.md](desktop-shell.md).

GUI workspace and durable-save integration run only through `regression.ps1 -Suite terminal-gui`. The independent `-Suite persistence` checks binary/text snapshots without a window.
