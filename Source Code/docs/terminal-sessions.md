# Text sessions

The kernel brokers input, output and foreground ownership for text sessions.
The [console host](text-console.md) uses the session controller to launch and
serve the Shell. See [interactive startup](interactive-runs.md) for atomic
terminal binding at process launch.

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
| 22 | WAIT_INPUT | primary foreground client | 0 / 0 | 0 / timeout ticks |

Event types: TEXT=1, KEY=2, EOF=3, HANGUP=4, CANCEL=5, RESIZE=6. KEY uses extra
as its opaque key value; RESIZE uses (width<<16)|height. Others require extra=0.
Modes: LINE=1, ECHO=2; zero is raw without echo. TerminalInfo: size=32, version=1,
width, height, mode, foreground PID, closed, reserved=0. TerminalEvent: type,
value, width, height. INFO/EVENT failure leaves output unchanged; query tails
remain untouched. Nonblocking empty reads/event/drain return S_AGAIN while live.

### Blocking byte reads

Committed bytes notify a FIFO reader, with remaining-input handoff after retry
completion or cancellation. EOF, hangup and foreground changes broadcast.
Notifications retain the original request and deadline; they do not copy bytes
to a user's buffer. The queue key includes the client object's generation, and
all queue/timer membership is revoked before thread or object-slot reuse.

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

### Waiting for events or bytes

`TerminalWaitInput` waits on a primary client handle for a queued terminal
event, readable bytes or EOF. It returns S_OK without consuming input. The
caller retries its existing EVENT and READ operations in that order. A
queued event or buffered bytes remain available after hangup. Once neither
is pending, hangup returns S_CLOSED. Foreground loss and local handle close
return S_DENIED and S_BADHANDLE respectively. The timeout is 0..60000 guest
ticks, with zero selecting an indefinite wait.

Event-only changes wake input waiters. Byte commits, EOF, hangup and
foreground loss also notify byte readers according to their own readiness.
The kernel checks readiness and registers the wait under the same interrupt
boundary. If another thread consumes the condition first, retry keeps the
original deadline.

### Blocking terminal output

Flagged `SYS_STREAM_WRITE` on a foreground client transfers up to 1024
bytes per call and returns the accepted byte count. If the output queue is
full, only the calling thread parks. A controller drain wakes a writer
after taking bytes out. If space remains after a writer completes,
times out, or is canceled, another waiter can use it. Hangup, client
closure and loss of foreground wake the affected writers to recheck
their handles and permissions.

Wakeup retries under the writer's address space and revalidates the source
buffer before copying. Partial progress returns to the caller.
`StreamWriteAllFor` submits the remaining suffix using the original
deadline and reports accepted bytes even when the operation times out.

### Edit frame acknowledgement

On `TERM_EDIT` and `TERM_EDIT_STATUS`, `extra=TERM_EDIT_BLOCK` parks the
foreground root Shell while a frame is pending. The first operation retries
publication after the previous frame is acknowledged and revalidates its source
buffer on retry. The second waits until the published frame is acknowledged.
The controller acknowledges the matching serial after rendering. Redirecting
the foreground, closing the handle or hanging up the session also wakes the
affected Shell waiter. With `extra=0`, both operations retain immediate status.

## Verification

`regression.ps1 -Suite terminal` runs strict tasks against two independent
sessions. It checks raw and canonical input, echo, resize, foreground transfer,
backpressure, handle rights, EOF, hangup and recovery after fault or cancel.

`tests/host/test-terminal-waits.js <ShellBridge build directory>` checks blocking
reads and selected pipe and completion interactions on a private guest disk.
The scheduler parks only the calling thread and resumes from its saved state.
