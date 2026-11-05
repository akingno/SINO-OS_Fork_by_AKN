# G2 interactive tasks and session broker

G2 provides an interactive launch transaction and a session-bound broker. The
diagnostic frontend is `run /boot/term.snx /g2`. Its strict root probe accepts
`go`, `fault` and `spin` to exercise worker completion, fault and cancellation.
Ctrl+C at the root produces an interrupt event and clears pending canonical
input. Ctrl+C during a worker cancels that worker. G3 supplies the application
Shell parser and command policy.

## Startup and resource ownership

The compatibility frontend has caps=0 and owns its window, terminal controller,
read-only boot directory grant and process observers. A new interactive task
has PROCESS_STRICT and caps=0. Its private CliInfo, attenuated directory handle,
terminal client and three role-specific stream handles exist before it can run.
Startup is a non-yielding kernel transaction. The terminal bind prepares all
client handles before committing the foreground change. Failure closes partial
resources while preserving the old foreground and input state.

CliInfo is backed by one per-process metadata page, freed with execution
resources. Interactive tasks use B completion records and consume zero C RunData
slots or finite capture pages. GetProcessResult observes their final result.
The C StartRun family, finite captures and host v2 protocol retain their contracts.

Each session has one root and at most one external task. Two complete sessions
use six of eight process slots and four of sixteen completion records. Each
frontend, root and worker uses at most five local handles in the current path.
With separate frontend directory grants, the two sessions use fourteen objects
for windows, controllers, grants, clients and completions. Standard-stream
handles share each process's client object through separate references. Metadata
includes one terminal page (3664 bytes used), one CliInfo page per task and one
client page per task, plus the directory grant object. Normal process address
spaces, stacks, page tables and copied window scenes have their existing costs.
Startup also uses one temporary specification page. Fixed global limits remain
enforced. The native test keeps both C capture slots occupied while two session
roots and their workers run.

## Session protocol

Operations append to SYS_TERMINAL without changing TerminalRequest's 24-byte ABI.
Definitions are shared in src32/interactive.h and user/interactive.h. Older
kernels reject the new operation numbers. FEATURE_TERMINALS remains the coarse
session feature flag.

InteractiveSpec is 1116 bytes. Its fields are size, version=1, program, id, argc,
bytes, directory rights, sixteen contiguous string offsets, and 1024 argv bytes.
Up to sixteen NUL-terminated arguments are copied and retain empty/UTF-8 values.
Only READ/LOOKUP/ENUM directory rights are accepted. The controller must own a
grant with those rights and DELEGATE. Children receive only requested rights.

InteractiveResult is 48 bytes. It holds size, version=1, request id, signed
startup status and a 32-byte ProcessCompletion. Startup errors have a negative
status. A completed task has status=0 and its typed exit/fault/cancel reason.

| Operation | Caller and inputs | Result |
| --- | --- | --- |
| IX_START=12 | Controller, exact Spec in buffer; value=directory handle, extra=0 root or 1 worker | Owned WAIT/CANCEL observer handle |
| IX_REQUEST=13 | Controller, Spec output buffer; value/extra zero | Peek at pending typed intent, or S_AGAIN |
| IX_SUBMIT=14 | Foreground root's primary client, exact Spec; value/extra zero | One pending intent accepted |
| IX_RESULT=15 | Root primary client, Result output; value=request id, extra zero | Consume one ready result, S_AGAIN while running, S_NOTFOUND after consumption |
| IX_FINISH=16 | Controller; value=owned completed worker handle, other fields zero | Restore root foreground, then publish its result |

The current boot catalog permits program IDs 1 and 2, both resolved to the
read-only boot g2test.snx probe. ID1 is the root, ID2 the worker. Runtime writable
files and caller-provided executable paths have no catalog entry. Extending the
catalog for gsh/tview is a separate application integration change.

The first root has request id zero. Its subsequent intents use consecutive
nonzero ids, without wrap. Only that session's foreground root can submit.
Pending/running/ready requests block another submission. A controller launch
must match the copied intent exactly and use the same pinned directory object
as the root, with rights no greater than the root grant. A worker cannot submit
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

G2 root Ctrl+C becomes TERM_INTERRUPT=7 after the event can be queued. A full
queue returns S_AGAIN without clearing input. The frontend retains a pending
cancel for retry. Worker Ctrl+C uses the existing owned foreground cancellation.
Foreground transitions clear old input, canonical state and queued terminal
events. The frontend also discards transition typeahead while honoring close
requests. General desktop event overflow and controlled paste remain subsequent
work alongside G3.

The last controller reference cancels both registered interactive PIDs, including
clients that ignore hangup and make no terminal I/O calls. This ownership policy
applies to interactive starts. Ordinary observer Close and legacy F controller
loss keep their prior contracts. Normal window close and frontend fault both
release controls through the same kernel cleanup. Root exit causes the frontend
to cancel any remaining worker.

## Verification

The native suite checks atomic first-instruction CliInfo/terminal/directory
visibility, role rights and attenuation, empty and UTF-8 argv, denied strict
spawn, two sessions, request tampering/replay, C quota independence, full process
and controller handle tables, result delivery after startup failure, controller
duplication and cancellation on last control close. QMP checks root Ctrl+C,
successful return, worker fault recovery, two simultaneous foreground workers,
directed cancellation, normal close and frontend #UD with noncooperative workers.
The host compares exact page/object baselines after cleanup.

Each step runs stage2-test.ps1 and results are inspected with stage2-summary.ps1.
Per-allocation OOM injection, exhaustive malformed-wire tests, the complete GUI
gate and physical hardware validation retain their separate verification scope.

Final gate stage2-20261105-161008-aaf190f7 passed 8/8 groups, 374 checks,
failed=0, not-run=0, exit=0. Ordinary build dev-20261105-161524-ccac3597
passed with kernel 151264 bytes and BSS end 1884800. A visible private-VM smoke
passed root startup, worker completion, Ctrl+C and foreground restoration.
The screenshot was inspected and the VM stopped.
