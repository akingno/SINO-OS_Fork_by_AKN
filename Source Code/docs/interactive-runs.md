# Interactive tasks and session broker

The session broker starts a strict root or worker with prebound terminal,
streams and directory handles. The [console host](text-console.md) uses it to
launch the Shell. ShellBridge images include `brokerqa.snx` for diagnostics.

## Startup and resource ownership

The compatibility controller has caps=0 and owns its terminal controller,
applicable directory grants and process observer handles. A new interactive task
has PROCESS_STRICT and caps=0. Its private CliInfo, attenuated directory handle,
terminal client and three role-specific stream handles exist before it can run.
Startup is a non-yielding kernel transaction. The terminal bind prepares all
client handles before committing the foreground change. Failure closes partial
resources while preserving the old foreground and input state.

CliInfo is backed by one per-process metadata page, freed with execution
resources. Interactive tasks use completion records and consume zero finite RunData
slots or finite capture pages. GetProcessResult observes their final result.
See [bounded runs](bounded-cli.md) for finite capture and the host protocol.

Each session has one root and at most one external task. Standard-stream
handles reference each process's client object. Startup uses a temporary
specification page, and each bound task retains a CliInfo page and a client page.
The Shell also holds a directory reference while its cwd is below the grant root.

## Session protocol

Operations append to SYS_TERMINAL without changing TerminalRequest's 24-byte ABI.
Definitions are shared in src32/interactive.h and user/interactive.h. Older
kernels reject the new operation numbers. FEATURE_TERMINALS remains the coarse
session feature flag.

InteractiveSpec is 1116 bytes. Its fields are size, version=1, program, id, argc,
bytes, directory rights, sixteen contiguous string offsets, and 1024 argv bytes.
Up to sixteen NUL-terminated arguments are copied and retain empty/UTF-8 values.
Legacy probe workers and catalog5 accept only READ/LOOKUP/ENUM directory rights; root catalog3 and writer catalog6 additionally accept explicit workspace write/commit rights without DELEGATE. The controller must own a
grant with those rights and DELEGATE. Children receive only requested rights.

InteractiveResult is 48 bytes. It holds size, version=1, request id, signed
startup status and a 32-byte ProcessCompletion. Startup errors have a negative
status. A completed task has status=0 and its typed exit/fault/cancel reason.

| Operation | Caller and inputs | Result |
| --- | --- | --- |
| IX_START=12 | Controller, exact Spec in buffer; value=directory handle, extra=0 root or 1 worker | Owned WAIT/CANCEL observer handle |
| IX_REQUEST=13 | Controller, Spec output buffer; value/extra zero | Peek at pending typed intent, or S_AGAIN |
| IX_SUBMIT=14 | Foreground root's primary client, exact Spec; value/extra zero | One pending intent accepted |
| IX_RESULT=15 | Root primary client, Result output, value=request id, extra=0 or IX_RESULT_BLOCK \| timeout | Consume one ready result, S_AGAIN while running in nonblocking mode, S_NOTFOUND after consumption |
| IX_FINISH=16 | Controller; value=owned completed worker handle, other fields zero | Restore root foreground, then publish its result |

Blocking waits use bit 31 of IX_RESULT's extra field to select blocking. The low bits
hold 0..60000 guest ticks, with zero selecting indefinite waiting. Publication
of a finished command or a rejected start wakes the root client. Session hangup
also wakes the root, which receives S_CLOSED if the session has closed. The Shell
uses this path while an external command is outstanding. Each result remains
single-consumer, and expiry leaves the result available for a later query.

The boot catalog permits root ID1 and worker ID2, both resolved to the read-only
boot brokerqa.snx probe. ID3 is the Shell root (`gsh.snx`). Worker IDs 4, 5
and 6 select `gcmd.snx`, `tview.snx` and `writer.snx` respectively. ID4 accepts
no directory grant, and ID5 accepts read-only grants. Runtime writable files and caller-provided
executable paths have no catalog entry. See [file-tools.md](file-tools.md).

The first root has request id zero. Its subsequent intents use consecutive
nonzero ids, without wrap. Only that session's foreground root can submit.
Pending/running/ready requests block another submission. A controller launch
must match the copied intent exactly. A worker requesting nonzero directory
rights uses the same pinned directory object as the root, with rights no greater
than the root grant. A zero-rights request instead passes directory handle zero
and receives no directory grant. A worker cannot submit
root requests. Cross-session observers cannot finish a job. Arbitrary PIDs and
capability selections are absent from this protocol.

The root may query its own pending result while in the background, while input,
output and mode changes remain denied. Finish validates the durable worker
completion, restores the existing root client and only then exposes the result.
Accepted startup failure produces a ready negative result without changing the
foreground. Invalid or tampered controller requests remain correctable pending
requests. Results are consumed once and replayed ids are rejected.

## I/O and cleanup

The primary terminal client retains F's bidirectional terminal API. Its stdin
stream has read rights and stdout/stderr have write rights. Role handles cannot
change terminal mode, submit requests or read results. Readiness masks respect
their directions. Closing or duplicating handles follows existing reference
rules. All displayed output uses the shared terminal byte stream.

Root Ctrl+C becomes TERM_INTERRUPT=7 after the event can be queued. A full
queue returns S_AGAIN without clearing input. The frontend retains a pending
cancel for retry. Worker Ctrl+C uses the existing owned foreground cancellation.
Foreground transitions clear old input, canonical state and queued terminal
events. The console host clears pending typeahead on foreground changes.

The last controller reference cancels both registered interactive PIDs, including
clients that ignore hangup and make no terminal I/O calls. This ownership policy
applies to interactive starts. Closing an observer alone does not cancel a task. Controller exit or fault
releases its controls through the kernel cleanup path. The console host closes
the session when the root exits.

## Verification

The native suite checks atomic first-instruction CliInfo/terminal/directory
visibility, role rights and attenuation, empty and UTF-8 argv, denied strict
spawn, two sessions, request tampering/replay, C quota independence, full process
and controller handle tables, result delivery after startup failure, controller
duplication and cancellation on last control close. The host compares page and
object baselines after cleanup. Run `regression.ps1 -Suite interactive` and
inspect the report with `test-summary.ps1`.

The [workspace contract](workspace-shell.md) describes the boot console's
fixed-home grant. The [text console checks](text-console.md#验证) cover keyboard
cancellation and session restart.
