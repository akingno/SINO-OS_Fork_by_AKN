# OS interface: Stages A through E

Stage A adds discovery and an opt-in strict process policy. Stage B adds bounded,
nonblocking completion objects. Stage C adds finite argv/input/output and a
supervised host protocol; see [bounded-cli.md](bounded-cli.md). Stage D adds
directory grants, range I/O and private publication/commit; see
[file-objects.md](file-objects.md). Stage D's minimum version passed compilation
and the 8-group Stage 2 text gate; remaining failure-injection limits are listed
there. Stage E adds bounded streaming and user runtime waiting; see
[stream-pipes.md](stream-pipes.md). General delegation and event-driven blocking
wait remain future work.
Existing SNX1 headers, startup strings and all 47 old syscall numbers remain
unchanged. Existing negative errors and whole-file Write's success value 0 are
unchanged. The policy test pins old syscall numbers with compile-time assertions.

## Versioned read-only queries

`QuerySystemInfo(SystemInfo*)` calls SYS_SYSTEM_INFO (0x12f).
`QueryStartInfo(StartInfo*)` calls SYS_START_INFO (0x130).
Registers: EBX = output address, ECX = capacity in bytes, EDX = 0 (reserved).
Both return 0 on success, S_INVALID on insufficient capacity, invalid/wrong-rights
output or nonzero reserved flags. Failure leaves output unchanged. On success,
only the defined structure is written; any capacity beyond it remains untouched.
Older kernels return S_INVALID for these unknown calls. Both policies may query.
The zero-flags v1 calls retain these fixed sizes; a future larger version needs
an explicit selector or new call, not a larger minimum for existing callers.

All fields below are little-endian u32, in listed order, aligned to four bytes.
`size` is the bytes returned, `version` is 1, and reserved output fields are zero.

SystemInfo (48 bytes): size, version, features, ticks_hz, max_processes,
max_threads, max_handles, max_objects, max_file, image_bytes, stack_bytes, reserved.
Current values after the first three fields: 100, 8, 4, 24, 64, 65536, 196608,
16384, 0. Limits describe current hard maxima, not available capacity or a promise
that allocation will succeed. ticks_hz is the current PIT frequency.

StartInfo (24 bytes): size, version, policy, caps, features, reserved. This is a
query of immutable launch policy and current granted capability bits, not an
input request. It does not change the launch page or expose kernel pointers.
Arguments()/Executable() still use the original immutable startup strings.
Stage C provides structured arguments through a separate CliInfo query.

Feature bits: SYSTEM_INFO=1, STRICT_POLICY=2, START_INFO=4, COMPLETION=8,
BOUNDED_RUN=16, FILE_OBJECTS=32, PIPES=64; currently features=127. Old clients mask known bits, not equality.
These indicate implemented mechanisms, not grants to use every mechanism.
Unknown feature bits must not be interpreted as authority. Unknown syscalls
return S_INVALID. Add future syscall IDs before SYS_CALL_END; the sentinel is not
a callable interface. Strict policy remains a default-deny allowlist.

## Trusted selection and old application compatibility

PROCESS_COMPAT=0 retains existing behavior. All existing launch paths continue
to use it. Existing caps=0 calculators/clocks retain their old public read APIs;
caps=0 alone must not be described as a confidentiality sandbox.

PROCESS_STRICT=1 is selected only by trusted kernel launch code through
process_launch_strict(path, args). It grants caps=0. The kernel Shell exposes
`run -s native.snx [argument]`; ordinary `run native.snx [argument]` is unchanged.
The Shell remains trusted Ring 0 code; this command is not a user syscall for
changing one's own policy. Policy cannot be selected by filenames, manifests,
argument strings or writable copies of an executable. The normal SNX loader
still validates the resolved image; the strict entry has no legacy Ring 0
executable fallback. A launch response still reports creation, not final exit.

Strict policy allows only EXIT, YIELD, LOG, TICKS, CLOSE, DUP, THREAD,
THREAD_EXIT, EVENT_CREATE, EVENT_SET, EVENT_WAIT, TIME, UI_LANGUAGE, SYSTEM_INFO
and START_INFO, plus Stage B PROCESS_RESULT and PROCESS_CANCEL for owned handles.
Stage C also allows CLI_INFO, STREAM_READ/WRITE, RUN_RESULT/READ/LIMITS; RUN_START
is still denied to strict callers. Streams and captures require owned handles.
Stage D also allows START_DIRECTORY, OPEN_AT, FILE_READ_AT/WRITE_AT/STAT,
DIR_READ, REMOVE_AT, RENAME_AT, FILE_STAGE/PUBLISH/COMMIT. They require explicit
owned directory/file/staging handles; DIR_GRANT remains denied. RunSpecV2 lets a
compatibility launcher explicitly attenuate one directory grant to a strict child.
Stage E adds local PIPE_CREATE, STREAM_READY and PIPE_LIMITS; StreamRead/Write
accept owned pipe endpoints as well as run-bound finite streams. RunSpecV3 lets
a compatibility launcher bind explicit pipe endpoints to strict child stdio.
Strict RUN_START/Spawn remain denied, and other handles are not inherited.
Existing pointer, object-type, rights and resource checks still
apply. LOG remains diagnostics, not stdout; time and language are public data.

All other known calls return S_DENIED before any operation-specific side effect
or user-buffer access: path reads/listing/writes, Sync, Spawn, Kill, process
enumeration, named Lease, networking, clipboard, OpenPath, windows and desktop
drag/drop integration. Even a strict process cannot use legacy Spawn to start a
compatibility child, including through aliases or writable copies. Strict policy
rejects both old Spawn and Stage B SpawnTracked; later delegation must preserve authority.

This confines explicitly strict processes, not every legacy application or the
remaining Ring 0 desktop. No GUI file selection grants, arbitrary delegation or general
revocation system are claimed. New CLI launch mechanisms should default to
strict policy when introduced, without silently changing old launch semantics.

## Stage B: completion objects

New IDs: SYS_SPAWN_TRACKED=0x131, SYS_PROCESS_RESULT=0x132,
SYS_PROCESS_CANCEL=0x133. All prior IDs and v1 query sizes remain unchanged.
The global completion pool is PROCESS_COMPLETIONS=16; it shares the existing
64-object and 24-local-handle budgets. S_LIMIT reports full records, objects,
handles or execution slots; S_NOMEM reports page allocation failure.

`SpawnTracked(path, arguments)` is a narrow compatibility-launcher API, not
SpawnEx. EBX is an absolute path, ECX a NUL-terminated string of at most 255 bytes
(or null for empty), EDX must be zero. Success returns an owned OBJ_PROCESS
handle with WAIT=1 and CANCEL=2. It always launches a strict caps=0 child, with
no inherited handles, streams or file grants. Strict callers receive S_DENIED.
The legacy Spawn and Shell run results are unchanged. A manifest or argv cannot
select compatibility policy for the child.

Inputs are copied before side effects; completion/object/observer slots are
reserved before image allocation. A failed launch rolls back both ownership
references and all partially allocated execution resources. No child is
published before all fallible setup has succeeded. The execution reference is
attached before making the process runnable; fault/exit needs no new allocation.

`GetProcessResult(handle, ProcessCompletion*)`: EBX=handle, ECX=output,
EDX=capacity (at least 32). WAIT rights and correct object type are required,
otherwise S_BADHANDLE. An invalid output or short capacity returns S_INVALID.
While running or awaiting cleanup, S_AGAIN leaves output unchanged. Success
copies exactly 32 bytes and leaves any tail untouched. Results are repeatable,
not consumed by one reader; duplicated WAIT handles observe the same result.

ProcessCompletion contains eight 32-bit fields, in order: size=32, version=1,
pid, reason, signed code, vector, reserved[0]=0, reserved[1]=0. Reasons:

| Reason | Value | code | vector |
| --- | --- | --- | --- |
| PROCESS_EXITED | 1 | signed program return/Exit value | 0 |
| PROCESS_FAULTED | 2 | 0 | CPU exception vector |
| PROCESS_CANCELED | 3 | 0 | 0 |
| PROCESS_KILLED | 4 | 0 | 0 |

Thus Exit(-14) and page fault 14 are different. Last-thread THREAD_EXIT records
normal exit 0. Old ProcessInfo retains its legacy code representation.

`CancelProcess(handle)` requires CANCEL rights; EBX=handle, ECX=EDX=0.
Reserved flags return S_INVALID. Cancellation marks only the bound running
child for termination; cleanup happens in process_step. Repeated cancellation
and cancellation after completion return 0. The first terminal event wins:
neither late cancellation nor old Kill overwrites a prior exit/fault/kill.
No CAP_MANAGE or arbitrary PID control is granted by this API.

Publication of completion occurs after owned handles, image pages, page tables
and kernel/thread stacks are reclaimed. The small immutable record survives
execution-slot reuse, until its last observer closes. Handles retain the
existing generation/cookie checks; PID is diagnostic, not observation authority.

Closing an observer, normal observer exit or observer fault does not cancel a
running child. The child owns one execution reference, released after cleanup;
if no observers remain the record then disappears automatically. A running
orphan still consumes a slot/record until it terminates. Completed records with
observers cannot be silently evicted: a full pool rejects new tracked launches.
Untracked old launches do not consume completion records. Job supervision and
disconnect cancellation belong to Stage C, not this close behavior.

The API is nonblocking: use S_AGAIN and bounded Yield polling. Kernel C stacks
and user pointers are not suspended. Old Shell run still reports creation;
Stage C's separate v2 managed protocol reports the final result and captures.

## Verification

Run repository-root `stage2-test.ps1`. The new poltest.snx executes in actual
CPL3, checks fixed wire layouts, output canaries, invalid capacities/flags,
readonly/guard-page output, every denied legacy entry, meaningful private-file
and deputy requests, spawn downgrade routes, local events/rights/threads and
immutable policy. The host checks actual completion/fault counters, PID cleanup,
resource recovery and a subsequent compatibility launch. Existing application,
Shell, storage, recovery and network checks remain in the same gate.

The recorded test-first run on the old kernel failed at `system query available`;
the original 336-check baseline had passed before adding these tests. Runtime limits and
feature changes require corresponding contract review, not just relaxed tests.
This does not replace historical-binary fixtures or excluded GUI regressions.
