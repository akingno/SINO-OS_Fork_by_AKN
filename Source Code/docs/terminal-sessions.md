# Stage F: text session backend

The headless text-session backend is implemented and tested. Desktop keyboard /
display wiring, a user terminal/Shell, PTY, ANSI parsing and job control are NOT
implemented by this delivery. No global input or framebuffer access is granted.
Both a headless and a future graphical frontend use the same session API and
process/handle cleanup; only the headless frontend has been exercised here.

Final gate: stage2-20261105-040857-714bf310, 8/8 groups, 360 checks, failed=0,
not-run=0, exit 0. Ordinary non-bridge build dev-20261105-041022-5443cb45 passed
(kernel 147904 bytes, BSS end 1884768). The test-first run on Stage E failed at
session creation, as expected. This does not claim the full 22-suite GUI gate.

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
S_BUSY so no input silently disappears. ECHO appends the original input bytes
to output: a frontend owns their presentation, not the kernel session backend.

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

FEATURE_TERMINALS=128, current feature mask=255. SYS_TERMINAL=0x14a:
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
| 7 | READ | client | data / capacity <=1024 | 0 / 0 |
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

## Evidence and remaining work

termtest.snx drives real strict CPL3 tasks with two independent controls: raw
binary, canonical backspace/newline, echo, resize, foreground transfer and denied
background reads, fault/cancel restoration, quota, direction/role/unknown request
errors, output integrity/backpressure, event saturation with unchanged dimensions,
EOF versus explicit hangup, controller loss, and generic stream/runtime access.
The host waits for completion, checks one intentional #UD, all remaining PIDs
and object/page baselines. Existing A-E, host protocol and cold boot stay enabled.

Remaining work: desktop keyboard/display adapter, interactive Shell integration,
atomic launch-time terminal binding if needed, foreground groups instead of one
task, full PTY/ANSI and detailed page-allocation failure injection. The backend
does not reserve global device input, so it does not establish desktop focus
security or prove physical keyboard routing. No thread WAITING/blocked kernel
stack has been introduced. Stage F's full frontend integration is still pending.
