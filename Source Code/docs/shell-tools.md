# Shell commands

The [text console](text-console.md) opens the Shell on an 80×25 text display.

## Welcome, help and command discovery

The Shell prints the banner from `user/shell_banner.h` at startup when
at least 56 columns are available, then prints the version, terminal size
and `/home/user` grant mode.

user/shell_commands.h is the command catalog shared by help and which.
`help` lists every catalog entry by category, including diagnostics and explicitly
unavailable commands. The list is generated from the command catalog, rather than
a separate handwritten subset. `help command` gives syntax, purpose, parameter
and option descriptions; aliases resolve to their canonical command. For example,
`help echo` describes `-n`, `-e`, supported escapes and output redirection.
`help status` explains the retained code, completion reason and startup status.
Unavailable commands can be described with help but cannot be executed.
`which command` distinguishes a builtin from an explicitly cataloged executable.
It does not search a global PATH.
DIR/dir/LS map to ls, TYPE/type/CAT to cat, and VER to ver. Diagnostic tools
args/exitcode/spin/fault appear in the Diagnostics category.

## Strict tools with no directory grant

gcmd.snx retains strict caps=0 and zero directory rights:

| Command | Behavior |
| --- | --- |
| `echo [-n] [-e] [text...]` | Optional omitted newline; -e accepts only newline, tab and literal backslash escapes |
| `date` | RTC calendar date, YYYY-MM-DD |
| `time` | RTC time, HH:MM:SS |
| `uptime` | Current boot tick count divided by queried tick frequency |
| `uname [-a]`, `ver` | SINO identity/version and i386 architecture |
| `about`, `credits` | Source, toolchain and firmware attribution |

Single quotes preserve backslashes for echo, for example `echo -e 'a\nb'`.
Unsupported escapes fail with code2 before printing any argument prefix.
Regular Shell quoting/escaping still runs first. This restricted text contract
does not accept arbitrary control bytes, hex escapes, variables or ANSI colors.

Arbitrary run paths, kill,
reboot/halt and other management commands remain on separate trusted paths.

## Read-only file tools

tview.snx (catalog5) adds these commands to the existing ls/stat/cat/view:

| Command | Behavior |
| --- | --- |
| `head [-n N] path` | First N lines, default10; N=0 prints nothing |
| `tail [-n N] path` | Last N lines, default10; trailing newline does not create a phantom final line |
| `wc [-l\|-w\|-c] path` | LF count, ASCII-whitespace word count, raw byte count; default prints all three |
| `grep [-n] text path` | Literal byte substring by line, optional 1-based line numbers; no match returns1 |
| `hexdump path` | 16 raw bytes per row with offset, hexadecimal bytes and safe ASCII |
| `tree [path]` | Depth-first listing of logical paths rooted at the requested directory/file |
| `find [path] [text]` | Same traversal with optional literal path-substring filter |

All file paths are normalized inside the Shell's existing grant. Only tree/find
and ls receive ENUM; other tools receive READ|LOOKUP. There is no implicit stdin
file mode, file write permission, regex engine, pipeline or redirection.
Missing/extra arguments and unsupported options are errors rather than silently
ignored. Line counts are checked unsigned32 values; negatives/overflow fail.

Counts and matching operate on original bytes, before display sanitizing. Text
output uses the [file viewer's UTF-8 and control-byte handling](file-tools.md), so binary byte counts and hex output do
not depend on substituted display characters. grep uses a bounded prefix matcher
and rereads matching ranges instead of allocating a whole line; tail uses two
scans. Arbitrarily long lines within the existing 64KiB file limit are supported.
Ranges/scans retain generation/revision/length checks and stop on mutation.

Traversal uses at most 128 pending paths in user BSS, and opens one enumeration
directory at a time. Each directory has versioned enumeration, not an atomic
whole-tree snapshot. Long paths, exhausted budgets and namespace changes fail
explicitly. `tree` prints logical paths in depth-first order. Empty grep
patterns match actual empty lines. A final LF does not create another line.

## Public calendar query

SYS_CALENDAR=0x14b appends a read-only query without changing SYS_TIME. EBX is
the output address, ECX capacity>=32 and EDX must be0. CalendarTime is eight u32:
size32, version1, year, month, day, hour, minute, second. Success writes exactly
32 bytes. Invalid/short/nonwritable output, flags or stale RTC data leave output
unchanged; stale data returns S_AGAIN. The cached RTC service supplies the value;
userspace reads the cached result. `FEATURE_CALENDAR=256` advertises this query.

`tests/guest/policy_tests.cpp` checks access, layout and output bounds.
`tests/host/test-text-scan.cpp` exercises the shared scan helpers. See the
[workspace contract](workspace-shell.md) for write commands.
