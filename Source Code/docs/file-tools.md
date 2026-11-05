# G4 read-only terminal file tools

The G3 Shell entry `run /boot/term.snx /g3` now recognizes `ls`, `stat`, `cat`
and `view`. These commands are dispatched to the strict `/boot/tview.snx`
program, catalog ID5. Listing, metadata formatting, text decoding and paging
are user-space policy, implemented in user/tview.c and user/file_text.h.
No old kernel Shell command is invoked to implement them.

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
The existing ID4 gcmd tools receive zero directory rights; the kernel now also
rejects nonzero directory rights for that catalog entry. The G2 diagnostic IDs
retain their existing behavior. No write, create, remove, commit or delegation
rights are added to this interactive path.

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
not add a noninteractive host run.ps1/v2 mode. Arbitrary pipelines, redirections,
file writes and default desktop entry switching belong to subsequent work.

## Tests

`tools/test-file-text.c` compiles the actual decoder/layout helpers, covering
controls, malformed/partial UTF-8, four-byte codepoints, a sequence straddling
byte512, empty input and wide-character wrapping. `tools/stage2-files-cli.js`
uses real QMP keyboard events and private-VM fixtures to exercise the Shell,
broker, strict file tools, paging and resource cleanup. Fixtures are created
by rdtest.snx, then removed; they must not be created on a daily development disk.
All checks are integrated into stage2-test.ps1. Inspect explicit saved runs with
stage2-summary.ps1; the full GUI/system gate remains a separate scope.

Final gate `stage2-20261105-182013-bf319296`: 8/8 groups, 385 checks, failed0,
not-run0, exit0. Ordinary build `dev-20261105-182311-caee9e61` passed (kernel
151424 bytes, BSS end1884800). Its visible private-VM smoke passed ls/cat,
pager prompt, Space to the final page and Shell foreground restoration;
screenshot inspected and VM stopped.

G5a adds head/tail/wc/grep/hexdump/tree/find using the same catalog and file identity checks; see [shell-tools.md](shell-tools.md) for syntax, raw-byte counting versus text display and traversal bounds. tree/find also receive ENUM. Default frontend width is now120 cells.
