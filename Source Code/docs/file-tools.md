# Read-only Shell file tools

The Shell dispatches `ls`, `stat`, `cat` and `view` to strict `/boot/tview.snx`
through catalog ID5. `user/tview.cpp` and `user/file_text.h` implement listing,
metadata formatting, text decoding and paging.

## Authority and paths

The Shell normalizes each path relative to its logical cwd and sends a copied,
grant-relative argument through the existing typed broker. An omitted ls path
means cwd. A leading slash means the session grant root, currently /home/user,
not the global filesystem root. Escaping that root is rejected before submit;
the kernel's anchored OpenAt independently enforces containment. Empty normalized
root is represented by `.`. There are no implicit mounts, global cwd or symlinks.

The trusted frontend delegates only READ|LOOKUP, plus ENUM for ls. These are
attenuated from the same pinned directory object as the Shell root. Strict
caps=0 policy and the catalog's read-only boot identity checks remain in force.
Catalog ID4 (`gcmd.snx`) accepts no directory grant. Catalog ID5 accepts only
read rights.

## Command contracts

| Command | First-version behavior |
| --- | --- |
| `ls [-a\|-l\|-la\|-al] [path]` | List a directory, hide dot names unless -a; -l prints kind, byte length and name |
| `stat path` | Print kind, byte length and current boot-local revision of a file or directory |
| `cat path` | Stream a text file to the terminal without pagination |
| `view path` | Stream a text file by pages; Space continues, q quits, Ctrl+D ends, Ctrl+C cancels |

ls appends `/` to directory names and follows VFS enumeration order; it does not
sort or synthesize `.`/`..` entries. Enumeration uses batches of eight entries
and the D version/cursor contract. Changes between batches report S_CHANGED;
already displayed rows are not rolled back. Unsupported options and wrong
argument counts return code2, ordinary file errors code1, normal finish or
pager quit/EOF code0. The Shell retains these through `status`.

These are text tools. NUL, ESC, CR, backspace, DEL and unsupported controls are
displayed as `?`; LF remains newline and tabs expand to four-column stops.
Valid UTF-8 is preserved across 512-byte reads, malformed or incomplete sequences
become `?`. cat preserves source line breaks and lets the frontend wrap display
rows; an unterminated final line receives a display newline before the prompt.
It is not a binary copy interface. No-output empty files complete normally.

view uses raw input without echo while running, reserves rows for its prompt,
and uses the frontend's one-cell ASCII/three-cell non-ASCII width convention.
It explicitly wraps page rows and consumes resize events. Paging is forward
only and does not require ANSI cursor addressing. Normal exit restores canonical
LINE|ECHO; fault/cancel/close uses the broker's foreground cleanup. The frontend
still owns PgUp/PgDn scrollback. Input typed during task transitions follows
the existing typeahead discard contract.

## Consistency and bounds

File contents use a 512-byte input buffer and a 512-byte output buffer, not a
64KiB stack allocation. The initial generation, revision and length are checked
before and after each range read, before continuing a page and at EOF. A changed
file stops with `file changed; reopen`; old prefixes may already be visible,
but the tool does not continue by splicing in new-version chunks. EOF before
the original expected length also fails. Output uses partial-write/backpressure
helpers; errors are not silently treated as successful truncation.

The maximum file remains 64KiB. Interactive output is not the C 4KiB finite
capture backend. These tools require atomic interactive terminal startup and do
use the host `run.ps1` protocol. See [workspace writes](workspace-shell.md)
for file modification commands.

## Tests

`tests/host/test-file-text.cpp` covers the decoder and layout helpers.
`tests/host/test-console.js` checks interaction through PS/2 input and VGA text memory.

head/tail/wc/grep/hexdump/tree/find use the same catalog and file identity checks. See [shell-tools.md](shell-tools.md) for syntax, raw-byte counting versus text display and traversal bounds. tree/find also receive ENUM. The ordinary VGA console is 80 columns wide.
