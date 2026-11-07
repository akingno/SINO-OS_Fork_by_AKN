# Text sessions and G1 desktop frontend

Stage F delivered the headless session backend. G1 adds a Ring 3 desktop frontend
and a strict, deadline-bound session probe using the same API. F12 now opens the formal user Shell. G2 adds atomic interactive launch/restore; PTY, ANSI and job control remain future work. Input arrives through focused USERAPP events, with copied drawing
commands for display. Global input and framebuffer access stay kernel-owned.

## Authority and lifecycle

A compatibility frontend creates a session and holds its control handle. Only
an owned WAIT|CANCEL completion handle for a live strict task may be selected as
foreground; the argument is a handle, never an arbitrary PID. Selection grants
a new child-local client handle, obtained by TerminalCurrent. An opt-in program
must wait with a bounded deadline for attachment after StartRun: this is a
separate operation, not atomic RunSpec startup inheritance. Old programs keep
their existing stdio; no startup structure is changed.

Controllers can feed text/events, resize, drain output and cancel only that
session's foreground task. Strict tasks cannot create controls. Client handles
cannot impersonate controllers. Clients are PID-bound and all input, output,
event and mode operations require current foreground ownership; background
clients receive S_DENIED. Metadata queries on an owned client remain permitted.
No arbitrary client/control inheritance or global keyboard access is provided.

Switching foreground clears pending input, the edit line, EOF and queued events,
and restores LINE|ECHO. Output history remains for the frontend to drain. Another
session's buffers/mode/size are independent. Switching back to a task already
attached to this session reuses its client; attaching a task belonging to another
session is S_BUSY. Pending allocations are rolled back before a failed attach
can change foreground state.

The last client-object reference closes its binding. If it is still foreground,
normal exit, explicit close, fault, timeout, cancellation or external kill thus
restore the default mode and clear foreground/input/events. Closing an old
background client cannot reset the new foreground task. Last controller close
marks the session hung up; it does not kill the task. An application must handle
S_CLOSED or have a managed task deadline. Client references retain the session
until cleanup. Dup extends the corresponding object's lifetime as usual.

## Bounds and input/output

There are at most two live sessions, one metadata page each; each attached
client object adds a page. The existing process/object/handle and two-retained-run
budgets are unchanged. A session has 1024-byte input and output buffers, a
256-byte uncommitted canonical line (including the newline), and eight typed
events. Dimensions are 1..240 columns and 1..100 rows. Default mode is LINE|ECHO.

TEXT feeds accept <=256 bytes atomically. In LINE mode, data is withheld until
newline; backspace/delete removes the last UTF-8 byte sequence from the edit
line. Input is transported as bytes, not validated/normalized Unicode. EOF
commits an unfinished line, then reads return 0 after draining. RAW mode exposes
bytes immediately, including NUL. Mode changes with a partial edit line return
S_BUSY so no input silently disappears. RAW ECHO appends the original input
bytes. Canonical ECHO appends accepted text and effective backspaces only:
backspace at an empty edit line emits nothing, and DEL becomes byte8. This
prevents erasing a prompt. A frontend owns presentation of these echo bytes.

When committing input or echo cannot fit, feed returns S_AGAIN and changes
nothing. An overlong edit line returns S_LIMIT without mutation. Output writes
accept a fitting prefix and return S_AGAIN when full. Frontends must drain it.
No escape sequence, command or drawing interpretation occurs here.

KEY is a typed event only in RAW mode. RESIZE queues an event and changes size
only if the event can be queued; full event queues return S_AGAIN. EOF is also a
typed event and repeat feed is idempotent. HANGUP marks the session closed even
when its event cannot fit. Buffered input remains readable; thereafter clients
get S_CLOSED, distinct from EOF=0. Event polling can likewise return S_CLOSED once
queued events are consumed. Controller drain returns 0 when closed and empty.
Client zero-byte I/O validates ownership then returns 0 without probing closure.

The frontend converts Ctrl+C into CANCEL. It cancels only the bound foreground
task with PROCESS_CANCELED, without CAP_MANAGE. Sending byte 0x03 as TEXT is just
data; it is not an implicit privileged operation. Readiness, stream I/O and
deadline helpers use the same session checks. StreamRead/Write/Ready accept the
client handle, so user/stream.h can be reused. Per-call session I/O is <=1024
bytes; no capture of terminal bytes is added to RunResult. Stdout/stderr are not
automatically rebound: applications explicitly use TerminalCurrent's handle.

## Wire ABI

FEATURE_TERMINALS=128. See [os-interface.md](os-interface.md) for the feature mask. SYS_TERMINAL=0x14a:
EBX=TerminalRequest*, ECX=24, EDX=0. Six u32 fields: op, handle, buffer, bytes,
value, extra. All unused request fields must be zero, except zero-length I/O
ignores its data pointer and queries ignore capacity beyond the defined output.
Handles are local and opaque. Wrong/stale/type/rights handles return S_BADHANDLE;
invalid memory, dimensions, operation, event, mode or reserved arguments
S_INVALID; quota S_LIMIT, allocation S_NOMEM. Foreground changes return a client
handle in the child's table, which is not usable in the controller's table.

| op | Name | handle | buffer / bytes | value / extra |
| --- | --- | --- | --- | --- |
| 1 | CREATE | 0 | 0 / 0 | width / height; returns control handle |
| 2 | CURRENT | 0 | 0 / 0 | 0 / 0; returns client or S_NOTFOUND |
| 3 | FOREGROUND | control | 0 / 0 | owned task handle / 0 |
| 4 | FEED | control | TEXT bytes / length, otherwise 0 / 0 | event type / event value |
| 5 | INFO | either | TerminalInfo* / capacity >=32 | 0 / 0 |
| 6 | MODE | client | 0 / 0 | mode flags / 0 |
| 7 | READ | client | data / capacity <=1024 | 0 / 0, or TERM_READ_BLOCK / timeout ticks |
| 8 | WRITE | client | data / length <=1024 | 0 / 0 |
| 9 | DRAIN | control | data / capacity <=1024 | 0 / 0 |
| 10 | EVENT | client | TerminalEvent* / capacity >=16 | 0 / 0 |
| 11 | READY | client | 0 / 0 | 0 / 0; returns stream readiness mask |

Event types: TEXT=1, KEY=2, EOF=3, HANGUP=4, CANCEL=5, RESIZE=6. KEY uses extra
as its opaque key value; RESIZE uses (width<<16)|height. Others require extra=0.
Modes: LINE=1, ECHO=2; zero is raw without echo. TerminalInfo: size=32, version=1,
width, height, mode, foreground PID, closed, reserved=0. TerminalEvent: type,
value, width, height. INFO/EVENT failure leaves output unchanged; query tails
remain untouched. Nonblocking empty reads/event/drain return S_AGAIN while live.

### Blocking byte reads

The kernel supports opt-in waiting to TERM_READ. Set value to `TERM_READ_BLOCK` (1) and
extra to the timeout in guest ticks (0..60000). Zero selects indefinite waiting.
An empty, live read suspends the calling thread. Committed TEXT, EOF and hangup
wake readers, including hangup caused by closing the last controller. A partial
canonical line becomes readable on newline or EOF. KEY, RESIZE and edit events
remain on their respective event paths.

The kernel saves the request and revalidates its client handle, foreground
ownership and output range on wake. If another reader takes the bytes first,
the wait continues with its original deadline. A foreground change wakes the
previous foreground reader so it can receive S_DENIED. Closing the exact local
client handle from a sibling thread completes the read with S_BADHANDLE.
Expiry returns S_TIMEOUT without writing to the destination. EOF, buffered
data and S_CLOSED follow the same ordering as a nonblocking read. Existing
interactive-shell cancellation on controller loss still applies.

## Evidence and remaining work

termtest.snx drives real strict CPL3 tasks with two independent controls: raw
binary, canonical backspace/newline, echo, resize, foreground transfer and denied
background reads, fault/cancel restoration, quota, direction/role/unknown request
errors, output integrity/backpressure, event saturation with unchanged dimensions,
EOF versus explicit hangup, controller loss, and generic stream/runtime access.
The host waits for completion, checks one intentional #UD, all remaining PIDs
and object/page baselines. Existing A-E, host protocol and cold boot stay enabled.

G2/G3 deliver atomic launch-time binding and interactive Shell launch/restore.
Remaining work includes further application integration, foreground groups instead of one
task, full PTY/ANSI and detailed page-allocation failure injection. The backend
does not reserve global device input. The headless F suite covers its own API
authority checks. G1 separately exercises desktop keyboard/focus routing below.
The terminal-wait runner `tests/host/test-terminal-waits.js <ShellBridge build directory>`
checks blocking reads and selected pipe/completion regressions with a private
guest disk. The scheduler parks only the calling thread and resumes from its
saved user state.

## Historical G1 desktop frontend

`user/terminal_frontend.c` implements `/boot/term.snx`. F12 and the legacy Shell
command `run /boot/term.snx` open independent windows. The resolved read-only boot
image is forced to caps=0. Its frontend uses compatibility policy and retains
ambient read access under that policy. Its strict child runs the probe from
`tests/guest/terminal_probe.c`. Both entry points share the boot image, selected by the
trusted startup policy, to fit the current FAT12 image. Only this new program
uses function-section garbage collection during linking.

The frontend creates a session, launches a C bounded run, then explicitly calls
TerminalForeground. The probe waits at most 100 ticks for TerminalCurrent and
uses that client directly. CliInfo stdio retains the existing C finite backend.
Each session consumes two process slots, one of the two retained C runs, three
frontend handles and four child handles. At steady state its window, controller,
completion, three finite streams and terminal client occupy seven objects.
Two windows use four of eight process slots and both C run slots. G2 needs a
separate interactive task contract before adding resident Shells and children.

The default probe deadline is 30000 guest ticks, approximately five guest
minutes. Window close requests cancel the owned task and release observation
and control references. Frontend crash still follows F controller-loss semantics,
with the managed run deadline bounding a client that ignores hangup. The fault
test uses a 100-tick deadline and verifies this noncooperative case. A completed
probe leaves a readable result until the user closes the window.

### Input and display

- Since G5a, the default grid is 120 by 25 in an 872 by 522 window. Client dimensions map to
  seven-pixel columns and 18-pixel rows, capped at 120 by 35. Size changes replace
  a pending resize with the newest dimensions and retry S_AGAIN.
- Each process owns 128 history rows and a streaming UTF-8 decoder. History is
  clipped when resized. Newline, CR, four-cell tabs, wide-character backspace and
  malformed/incomplete UTF-8 have explicit handling. NUL, ESC and other unsupported
  controls display `?`, preserving bytes after them. Output returns to the bottom
  of scrollback. PgUp/PgDn are reserved for local history in G1.
- MSG_TEXT codepoints are encoded once. MSG_KEY provides printable ASCII,
  newline, backspace and tab. Other raw keys become typed KEY events. Ctrl+D
  feeds EOF. Ctrl+C cancels the owned foreground probe. A desktop focus selection
  clears an unfinished IME composition before assigning another window.
- Each iteration consumes at most eight window events, retries eight pending
  input items and drains 1024 output bytes. Accepted input survives S_AGAIN in
  order. The pending queue holds 64 items. Further input receives visible full
  queue feedback, while close/cancel remain responsive. The existing 15-event
  desktop delivery queue also remains bounded. Arbitrarily fast input has no
  lossless delivery guarantee.
- `UI_CELL_TEXT=9` appends a copied drawing command for fixed-grid text. Font 2
  draws at x/y, advancing seven pixels for ASCII and 21 for non-ASCII. The existing
  17-pixel CJK glyph sits within the latter cells. Strings hold at most 79 UTF-8
  bytes and split only between codepoints. The frontend's compile-time bound
  ensures the entire visible grid fits one 256-command canvas.

### Probe and regression coverage

Type any line to receive `LINE ...`. `raw` enables byte and special-key reporting,
Escape returns to canonical mode, `flood` writes 12289 bytes under backpressure,
and `unicode` splits a UTF-8 sequence across writes and includes visible controls.
`fault` deliberately faults the strict client. `/selftest` runs a small CPL3 model
smoke check. `/qa` enables bounded diagnostic markers. `/fault-test` faults the
frontend after the probe starts. `rawpause` and `stall` exercise input pressure.

The host compiles the actual model for malformed UTF-8, wide-cell overwrite,
backspace, wrapping, ring history, independent state and bounded scene segments.
The native suite injects real QMP keys, verifies copied scene text, switches two
focused windows, commits Chinese input, checks ordered retries, maps a simple
maximize operation and checks EOF/cancel/fault/external termination. It also
exhausts sessions and the pending queue, closes a stalled client, and checks exact
page/object baselines. F2 receives a separate real-keyboard compatibility check.
Complex visual interactions and the complete 22-suite GUI gate remain separate.

G2 adds an opt-in atomic interactive startup and session broker. See [interactive-runs.md](interactive-runs.md) for ownership, typed requests/results and stream roles. In a ShellBridge test image, use `run /boot/termqa.snx /broker-test` for its strict diagnostic probes.

G3 introduced the broker path now used by `run /boot/term.snx` for a strict user Shell with quoted argv,
per-session cwd within a read-only home grant, bounded history and external text
tools. See [user-shell.md](user-shell.md) for syntax, statuses and verification.

G5a adds the plain-text user welcome, everyday text/information commands and a 120-column default. See [shell-tools.md](shell-tools.md). The earlier G1 dimensions describe that delivery.

G5b adds an explicit writable frontend mode, now selected by /write. The default stays read-only; session ownership and cancellation contracts are unchanged. See [workspace-shell.md](workspace-shell.md).

G5c adds the editable prompt protocol and separates diagnostics from the ordinary boot image. See [shell-editing.md](shell-editing.md).

G6 routes F2, the SINO menu and Terminal.app to the writable Ring 3 frontend. F12 opens a read-only window. For read-only boot access and the typed desktop application launcher, see [desktop-shell.md](desktop-shell.md).
