# G5a everyday Shell commands

Use `run /boot/term.snx /g3`. The new frontend starts at **120 by 25 cells**
in an 872 by 522 pixel window (7-pixel ASCII cells plus margins). Resizing still
maps to the actual available grid, up to 120 by 35. Non-ASCII glyphs occupy
three cells under the existing font convention. The legacy F2 terminal keeps
its historical implementation until G6 switches default entry points.

## Welcome, help and command discovery

The plain-text SINO SH lettering is in **user/shell_banner.h**, derived from
the existing src32/banner.h lettering. gsh prints it once at startup when at
least 56 columns are available, then the real version, current grid size and
`/ = /home/user (read-only)` mapping. Narrow startup grids use the short welcome.
No kernel drawing/color calls or ANSI parser are required for this banner.

user/shell_commands.h is the command catalog shared by help and which.
`help [command]` gives supported syntax; `which command` distinguishes a builtin
from an explicitly cataloged executable. It does not search a global PATH.
DIR/dir/LS map to ls, TYPE/type/CAT to cat, VER to ver. Write-oriented DOS aliases
are not advertised before their underlying commands exist. Diagnostic tools
args/exitcode/spin/fault remain explicit and separate from everyday help.

## Strict tools with no directory grant

gcmd.snx retains strict caps=0 and zero directory rights:

| Command | Behavior |
| --- | --- |
| `echo [-n] [-e] [text...]` | Optional omitted newline; -e accepts only newline, tab and literal backslash escapes |
| `date` | RTC calendar date, YYYY-MM-DD |
| `time` | RTC time, HH:MM:SS |
| `uptime` | Current boot tick count divided by queried tick frequency |
| `uname [-a]`, `ver` | SINO identity/version and i386 architecture |
| `about`, `credits` | Source, toolchain, firmware, font and pinyin attribution |

Single quotes preserve backslashes for echo, for example `echo -e 'a\nb'`.
Unsupported escapes fail with code2 before printing any argument prefix.
Regular Shell quoting/escaping still runs first. This restricted text contract
does not accept arbitrary control bytes, hex escapes, variables or ANSI colors.

mem/free, df and global ps are recognized as unavailable: current strict public
queries do not expose their live global data. SystemInfo capacity limits are
not used as fake usage counters. Installation, arbitrary run paths, kill,
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
output uses G4's UTF-8/control handling, so binary byte counts and hex output do
not depend on substituted display characters. grep uses a bounded prefix matcher
and rereads matching ranges instead of allocating a whole line; tail uses two
scans. Arbitrarily long lines within the existing 64KiB file limit are supported.
Ranges/scans retain generation/revision/length checks and stop on mutation.

Traversal uses at most 128 pending paths in user BSS, and opens one enumeration
directory at a time. Each directory has versioned enumeration, not an atomic
whole-tree snapshot. Long paths, exhausted budgets and namespace changes fail
explicitly. Output order follows a bounded DFS stack and may differ from the old
kernel Shell's node order; tree prints logical paths rather than ASCII branches.
Empty grep patterns match actual empty lines; unlike the old implementation,
they are not silently skipped. No phantom line is synthesized after a final LF.

## Public calendar query

SYS_CALENDAR=0x14b appends a read-only query without changing SYS_TIME. EBX is
the output address, ECX capacity>=32 and EDX must be0. CalendarTime is eight u32:
size32, version1, year, month, day, hour, minute, second. Success writes exactly
32 bytes. Invalid/short/nonwritable output, flags or stale RTC data leave output
unchanged; stale data returns S_AGAIN. The cached RTC service supplies the value;
there is no CMOS access from userspace. FEATURE_CALENDAR=256 makes the current
feature mask511. Older clients must keep querying feature bits.

policy_tests.c checks strict/compat access, layout, date ranges, invalid output,
reserved flags and untouched output tails. test-text-scan.c exercises the same
word counter, prefix matcher and checked integer helper used by the file tools.
stage2-shell-tools.js drives real QMP commands, 120-character rows, help/aliases,
calendar/information output, long files and resource recovery. Run the Stage2
gate and read its saved summary; full GUI/system validation has separate scope.

Final evidence: stage2-20261105-184009-da901c97 passed 8/8 groups and389 checks, failed0/not-run0/exit0. Ordinary dev-20261105-184416-8fd9f1b2 passed (kernel151648 bytes, BSS end1884800). Visible private-VM welcome/120-column/date/tail/pager smoke passed; screenshot inspected and VM stopped.
