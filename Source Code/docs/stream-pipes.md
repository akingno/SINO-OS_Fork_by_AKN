# Stage E: bounded streaming and runtime waiting

Stage E adds pipe-backed streams and explicit standard-stream binding to strict
children. It uses the existing StreamRead/StreamWrite, object table and completion
objects. It does not change the kernel Shell's builtin pipe implementation, the
host v2 finite-capture protocol, or interactive terminal behavior.

## ABI and quotas

FEATURE_PIPES=64 raised the Stage E mask to 127 (A-F now use 255). Existing IDs/layouts and
errors remain unchanged. Append S_CLOSED=-12 for a pipe's missing reader, and
S_TIMEOUT=-13 for user runtime deadlines. No Unix SIGPIPE is delivered.

| ID | Call | EBX | ECX | EDX |
| --- | --- | --- | --- | --- |
| 0x147 | PipeCreate | capacity bytes | PipePair* | output capacity >=8 |
| 0x148 | StreamReady | owned stream handle | 0 | 0 |
| 0x149 | QueryPipeLimits | PipeLimits* | output capacity >=32 | 0 |

PipePair is two u32 local handles: read, write. Success is 0; failed creation
leaves it unchanged. Reserve both objects and local handles transactionally;
even failure to allocate the second handle must release the first and all pages.
PipeCreate is allowed in either policy; it creates only process-local authority.

PipeLimits is eight u32 words: size=32, version=1, pipes=8, capacity=4096,
io_bytes=65536, timeout_ticks=60000, ready_flags=31, reserved=0. The pipe pool is
global; capacity is chosen per pipe in 1..4096, including non-powers of two.
Each live pipe allocates one metadata and one data page. The existing 64 objects,
24 handles per process, 8 processes and two retained managed-run limit remain.
Closing only one endpoint does not free a pipe held by its other endpoint.

Queries leave output unchanged on failure and leave capacity tails untouched.
Invalid capacities, flags, memory ranges or I/O lengths return S_INVALID;
wrong-type/stale/foreign/insufficient-rights handles S_BADHANDLE; exhausted
pipe/object/handle pools S_LIMIT; page allocation failure S_NOMEM. These limits
are bounds, not guarantees that allocation succeeds.

## Byte streams and endpoint lifetime

Read uses READ=1 and write uses WRITE=2, on distinct endpoint object types. Dup
retains the same endpoint and cannot reverse direction. A pipe does not retain
user pointers or run records. Only the last object reference closes an endpoint,
including all duplicate handles, pending startup references and child bindings.

- Read returns available bytes, possibly fewer than requested.
- Empty with a writer still alive returns S_AGAIN.
- After every writer closes, buffered bytes remain readable; drained Read returns
  repeatable 0 (EOF).
- Write accepts the fitting prefix. Full with a reader alive returns S_AGAIN.
- With no remaining reader, a nonempty write returns S_CLOSED.
- A zero-byte call validates the handle and direction, then returns 0 without
  accessing a buffer or probing peer state. A closed handle is still an error.

Nonempty calls validate the entire supplied user range before changing the
buffer, with at most 65536 bytes requested per call and actual progress bounded
by the pipe. Failed validation cannot consume bytes. No NUL/UTF-8/newline
transformation occurs. There are no message, whole-write, whole-line or
multi-writer atomicity guarantees. Each kernel operation is bounded and runs
without sleeping in the existing non-preemptible single-CPU kernel.

## Explicit startup binding

RunSpecV3 is 80 bytes: the 64-byte RunSpecV2 context with base size=80/version=3,
then in, out, err, reserved=0. Directory grant rules from Stage D are unchanged.
StartRunWithStreams uses SYS_RUN_START with ECX=80, EDX=0. Versions 1 (48) and
2 (64) remain accepted without modification; the base flags/reserved and both
directory reserved words must remain zero.

Each nonzero in/out/err names an owned **pipe** endpoint, read for stdin and write
for stdout/stderr. Zero keeps the corresponding Stage C finite-input/capture
backend. Binding a pipe stdin with nonempty finite input is invalid, so input is
not silently discarded. Captures, sockets, files and arbitrary handles are not
accepted as pipe bindings. Only compatibility launchers may start runs, and the
child always remains strict with caps=0 and no ambient Spawn/file access.

The launch transaction acquires endpoint references, then transfers them to
new child-local handles. It rolls back all acquired references on failure,
including a later binding or executable/loader failure. Stdout and stderr may
explicitly share a writer; closing one role must not close the other. No arbitrary
handle table is inherited, and directory delegation still requires DELEGATE.

CliInfo v1 and its opaque in/out/err roles remain unchanged. Finite backends
continue using their existing per-run binding checks. Pipe endpoints instead
use owned type/rights checks, so a child can use an explicitly received pipe or
one it creates locally. Knowing another process's handle number grants nothing.
StreamReady does not expose kernel pointers or the pipe's backing address.

RunResult/ReadRunOutput still describe finite input consumption and captures;
piped roles have no capture and report zero capture bytes, not total transported
bytes. Applications can count their own transferred bytes. RunData retains the
same finite buffer allocation and two-run budget even for fully bound runs.
This version supports the tested two-process producer/consumer; it does not
expand the budget for arbitrary-length native pipelines. Host run.ps1 continues
using finite v1 requests and does not launch streaming pipelines.

Start every pipeline member before waiting, then close unused parent copies.
Otherwise retained readers suppress S_CLOSED and retained writers suppress EOF.
An observation handle does not keep a finished child's pipe endpoints open.
Exit, fault, owned cancellation, timeout and external kill use normal handle
cleanup; unread bytes survive writer exit while a reader remains.

## Readiness and bounded waits

StreamReady returns a nonnegative bit mask: READABLE=1, WRITABLE=2, EOF=4,
HANGUP=8, LIMIT=16. For pipe readers, pending data sets READABLE; no writers sets
HANGUP, and when drained sets EOF|READABLE too. For pipe writers, available space
sets WRITABLE; no readers sets HANGUP. Full/empty-but-connected can return zero.
Finite stdin is always READABLE and sets EOF when consumed; finite captures are
WRITABLE with space and LIMIT when full. Readiness queries never consume data
or terminate a capture-overflowing run.

`user/stream.h` supplies bounded helpers. On kernels advertising THREAD_WAIT,
pipe transfers, terminal byte reads and completion waits can suspend the caller.

- StreamWaitFor(handle, READABLE or WRITABLE, timeout) polls readiness flags and returns them,
  S_CLOSED for a hung-up writer, S_LIMIT for full finite capture, or S_TIMEOUT.
- StreamReadFor/StreamWriteFor retry only S_AGAIN, returning the first actual
  transfer/EOF/error, including short positive progress.
- StreamWriteAllFor(handle, data, bytes, timeout, &written) uses one deadline
  across the whole bounded operation. It returns 0 only after every byte is
  accepted, and always records accepted progress even on error/timeout. Retry
  only the remaining suffix. It accepts total bytes <=65536.
- RunWaitFor and ProcessWaitFor wait on owned completion objects and preserve
  the output on S_TIMEOUT. A wait timeout does not automatically cancel a run;
  use CancelProcess explicitly when required, then wait for its final result.

Timeouts are 0..60000 guest ticks at the current 100 Hz. Zero permits an immediate
attempt; it is not an infinite wait. Unsigned elapsed differences handle tick
wrap within this bound. Persistent errors and closure return immediately.
Terminal output backpressure and older kernels use the bounded Yield(1) retry.
Readiness is a hint. Another reader or writer
can consume the opportunity before the next syscall. Helpers do not reserve it.
The older CliWriteAll remains a finite-backend helper and may return S_AGAIN on
pipes; use stream.h when waiting is intended.

### Opt-in blocking pipe calls

The kernel supports a blocking mode to the pipe endpoints of SYS_STREAM_READ and
SYS_STREAM_WRITE. EBX holds the handle and ECX holds the user buffer. Set
`STREAM_BLOCK_FLAG` (bit 31) in EDX alongside the byte count (0..65536).
ESI holds the timeout in guest ticks (0..60000), with zero selecting indefinite
waiting. Calls without the flag ignore ESI and retain the existing behavior.
The flag requires a pipe endpoint with matching rights. Finite and terminal
stream handles return S_BADHANDLE in this mode.

A flagged call attempts the transfer first. On S_AGAIN the scheduler suspends
the calling thread. Opposite-end progress or final peer close makes it eligible
to retry. The scheduler rechecks the handle and the entire user buffer before
transferring. Another waiter may consume the available bytes or space first.
In that case the thread waits again with its original deadline. Once that
deadline expires, the call returns S_TIMEOUT without transferring bytes.
Closing the exact local handle from a sibling thread returns S_BADHANDLE to the
waiter even if a duplicate keeps the endpoint alive. Peer close follows the
normal drain/EOF and S_CLOSED rules.

The kernel retains the saved user registers and wait metadata across suspension.
Process teardown discards the wait and releases the normal handle references.
The `stream.h` helpers use the remaining deadline for blocking and treat zero
as a single immediate attempt.

## Validation boundary

`pipetest.snx suite` remains part of the independent non-GUI pipes group. Its
cases cover exact memory/size/direction limits, short writes, tiny circular
buffers, binary NUL, quota and second-handle rollback, duplicate writer EOF,
explicit v3 startup and rollback, real concurrent 12289-byte transport through a
257-byte pipe, early reader close, stdout/stderr sharing, producer #UD, blocked
writer cancellation/timeout, runtime deadline/progress and synthetic tick wrap.
The host checks final results, intentional fault counts and object/page baselines.

The pipe-wait runner `tests/host/test-pipe-waits.js <ShellBridge build directory>`
uses a private guest disk. It covers the legacy pipe probe, blocked read and write,
final peer close, timeout, competing readers, local handle reuse, buffer unmap,
wire validation and process timeout cleanup. It checks the free-page count after
each case and saves the aggregate result in `shell-report.json`.

Terminal sessions and foreground/input ownership belong to Stage F.
