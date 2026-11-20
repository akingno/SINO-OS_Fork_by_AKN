# User Shell

At boot, the [text console](text-console.md) starts strict `/boot/gsh.snx`
through the [interactive broker](interactive-runs.md). The Shell opens a
[writable workspace](workspace-shell.md).

## Authority and paths

The console host uses compatibility policy with caps=0 and holds a kernel-issued
workspace grant. The Shell and external tools use PROCESS_STRICT with caps=0.
The Shell receives an explicit grant for `/home/user`. In the Shell, `/` means
that grant's root, not the global filesystem root. `pwd` and the prompt show
this logical path. Each Shell owns its own relative cwd and open directory
reference. There is no kernel-global chdir or environment inheritance.

`cd` normalizes `.`, repeated separators and `..`; traversal above the grant
root is rejected. Absolute paths restart at the grant root. Normalized paths
fit 127 bytes. A failed open preserves the old cwd; successful navigation opens
the new directory before closing the old reference. Existing VFS open-reference
Busy rules apply while a directory is held.

Catalog ID3 is the Shell root. ID4 is `/boot/gcmd.snx`, a small external tool
selected by argv[0]. Both executable nodes must resolve to read-only boot files.
The Shell submits typed broker requests and cannot directly Spawn/StartRun.
The ID4 text tools request zero directory rights and receive no directory
handle. Catalog ID5 selects `/boot/tview.snx` for read-only commands with
normalized cwd-relative arguments and explicit READ|LOOKUP (plus ENUM for ls).
See [file-tools.md](file-tools.md) for text safety, paging and consistency.

## Commands and syntax

| Command | Behavior |
| --- | --- |
| `help` | List builtins and tools |
| `pwd` | Print logical cwd |
| `cd [path]` | Change cwd; omitted path selects grant root |
| `clear` | Clear the console scrollback and redraw the prompt |
| `history` | Print the last 16 submitted nonempty lines, including itself |
| `status` | Print retained signed code, completion reason and startup status |
| `exit [0..255]` | Exit with explicit code or retained last code |
| `echo [args...]` | External tool prints arguments separated by spaces |
| `args [args...]` | External tool prints argc, argument byte lengths and values |
| `exitcode signed-i32` | External tool returns the specified signed code |
| `spin` | Diagnostic worker waits until canceled |
| `fault` | Diagnostic worker triggers intentional #UD |
| `ls [-a\|-l\|-la\|-al] [path]` | List cwd or another granted directory |
| `stat path` | Print file/directory kind, byte length and revision |
| `cat path` | Display a file as safe text, without pagination |
| `view path` | Page safe text; Space continues, q quits |

Lines contain at most 255 bytes, up to 16 arguments and a 1024-byte argv block.
Single/double quotes preserve spaces and empty arguments; adjacent quoted and
unquoted fragments concatenate. Backslash escapes the next byte except inside
single quotes. UTF-8 argument bytes are preserved. Unterminated quotes, trailing
escapes, embedded NUL and unsupported control bytes are syntax errors.
The Shell accepts one unquoted `>` or `>>` for echo in a writable session.
Other unquoted shell operators and variable expansions remain unsupported.
For example, `args "" "a b"` passes two arguments after
the command name, one empty and one containing a space.

## Input and task results

The Shell owns the editable line and history recall.
The frontend displays acknowledged edit frames. Workers may use backend
canonical editing and echo.
Ctrl+C at the prompt clears the unfinished line and retains code130/reason3;
during a tool it cancels only that foreground worker. The broker restores the
Shell foreground before delivering the once-consumed result. Queued typeahead is cleared during foreground transfer. Wait for the next prompt
before entering another external command. See [shell-editing.md](shell-editing.md)
for editor input handling.

Normal worker completion preserves its signed exit code and reason1. Cancel
maps to code130/reason3, fault to 128+vector/reason2 (#UD gives134), other
termination to137. Syntax/usage errors use2, unknown commands127, startup
failure126 with reason0 and the negative startup status. `status` leaves the
retained result unchanged. Ctrl+D commits an unfinished line, executes it, then
exits with the retained code. The console host starts a new session when the Shell exits.

stdin, stdout and stderr are atomically bound directional session handles;
display output shares one terminal stream. This path does not use the C finite
4KiB capture buffers or either of its two retained-run slots. It is an interactive
entry, not a new host run.ps1/v2 protocol.

## Verification

`tests/host/test-gsh.cpp` checks the shared parser, path and integer helpers.
The [text console checks](text-console.md#验证) exercise startup, editing and
history, workspace commands, cancellation and restart. Run `regression.ps1`
with `-Suite shell-waits` to check blocking I/O and foreground handoff.

See [Shell commands](shell-tools.md), [line editing](shell-editing.md) and
[workspace writes](workspace-shell.md) for their individual contracts.
