# OS interface: Stages A through F backend

Stage A adds discovery and an opt-in strict process policy. Stage B adds bounded,
nonblocking completion objects. Stage C adds finite argv/input/output and a
supervised host protocol; see [bounded-cli.md](bounded-cli.md). Stage D adds
directory grants, range I/O and private publication/commit; see
[file-objects.md](file-objects.md), including its remaining failure-injection
limits. Stage E adds bounded streaming and user runtime waiting; see
[stream-pipes.md](stream-pipes.md). The kernel supports thread-level blocking waits.
Stage F adds the [text session backend](terminal-sessions.md), with frontend
integration still pending. SYS_TERMINAL=0x14a is allowed by strict policy; its
control/client authority checks still apply, including denial of strict CREATE.
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
Current values after the first three fields: 100, 8, 4, 24, 64, 65536, 16777216,
16384, 0. Limits describe current hard maxima, not available capacity or a promise
that allocation will succeed. ticks_hz is the current PIT frequency.

StartInfo (24 bytes): size, version, policy, caps, features, reserved. This is a
query of immutable launch policy and current granted capability bits, not an
input request. It does not change the launch page or expose kernel pointers.
Arguments()/Executable() still use the original immutable startup strings.
Stage C provides structured arguments through a separate CliInfo query.

Feature bits: SYSTEM_INFO=1, STRICT_POLICY=2, START_INFO=4, COMPLETION=8,
BOUNDED_RUN=16, FILE_OBJECTS=32, PIPES=64, TERMINALS=128, CALENDAR=256,
WORKSPACE=512, LINE_EDIT=1024, ANON_MEMORY=2048, ANON_LAZY=4096, SNX_V2=8192,
THREAD_WAIT=16384.
`FEATURE_EVENT_WAIT=32768` advertises explicit blocking manual-reset event waits.
`FEATURE_THREAD_JOIN=65536` advertises argument-bearing threads and join/detach.
The current feature mask is 131071. Clients detect features by testing individual bits.
These indicate implemented mechanisms, not grants to use every mechanism.
Unknown feature bits must not be interpreted as authority. Unknown syscalls
return S_INVALID. Add future syscall IDs before SYS_CALL_END; the sentinel is not
a callable interface. Strict policy remains a default-deny allowlist.

## Thread-level waiting

### Manual-reset events

Legacy `SYS_EVENT_WAIT=0x115` remains a nonblocking query: read right 1 is
required; clear returns S_AGAIN and set returns S_OK. Its unused arguments
retain their legacy behavior. `SYS_EVENT_SET` requires write right 2; a nonzero
value sets the state and broadcasts queued event waiters, while zero resets it.
Success does not consume or reset the event.

`SYS_EVENT_WAIT_BLOCK=0x151` uses EBX=owned event handle, ECX=timeout in ticks
(0..60000, zero means indefinite), EDX=0. Invalid timeout/reserved arguments
return S_INVALID before handle lookup. Invalid, stale, wrong-type or non-readable
handles return S_BADHANDLE. A pre-set event returns S_OK immediately. Otherwise
only the calling thread parks using the common generation-keyed wait queue.
Strict and compatibility callers may use this local mechanism; no cross-process
handle lookup or new delegation is provided. Old syscall numbers and query
structure layouts are unchanged; SYS_CALL_END is now 0x156 after the thread calls below.

Notification permits a retry under the waiting process's address space. Reset
before retry causes the wait to resume with its original deadline. Expiry is
checked before retry, returns S_TIMEOUT and does not alter the event. Closing
the exact local waiting handle completes that wait with S_BADHANDLE, including
an already-notified wait; closing another duplicate does not cancel it.
Exit, cancellation and faults revoke all associated queue/timer membership.

The C++ SDK exposes `sino::event::create`, `set`, `query`, `wait(handle, timeout=0)`
and `available` in sdk.h. `available` checks FEATURE_EVENT_WAIT; `wait` is one
kernel call and has no polling fallback on older kernels.

### Other blocking mechanisms

`FEATURE_THREAD_WAIT` advertises opt-in blocking for pipe transfer, terminal
byte reads, owned completion results and the interactive broker result. The
SDK exposes `StreamReadBlocking` and `StreamWriteBlocking` for pipes,
`TerminalReadBlocking` for terminal input, and `WaitProcess` and `WaitRun`
for completion records. The timeout range is 0..60000 guest ticks. Explicit
blocking calls use zero for an indefinite wait. The `stream.h` bounded helpers
use zero for an immediate attempt and can still run on kernels without this bit.

The scheduler parks the calling thread and retries a signaled operation under
that thread's address space. Finite waits retain their deadline across retries.
See the [pipe](stream-pipes.md#opt-in-blocking-pipe-calls),
[terminal](terminal-sessions.md#blocking-byte-reads), and
[broker](interactive-runs.md) contracts for their selectors and wake conditions.

## Argument-bearing threads and join/detach

The old `SYS_THREAD=0x111` still creates a legacy worker and returns raw slot
1..3. `SYS_THREAD_EXIT=0x112` still explicitly exits with code zero and ignores
its arguments. These legacy slots, and the initial main thread, are not joinable
TIDs. New calls are local mechanisms allowed by strict policy; they do not
grant child-process delegation or cross-process thread access.

| Call | Registers / result |
| --- | --- |
| THREAD_CREATE=0x152 | EBX=ThreadCreateSpec pointer, ECX=24, EDX=0; positive TID or S_* |
| THREAD_JOIN=0x153 | EBX=TID, ECX=ThreadResult pointer; EDX>=16 for nonblocking join, or EDX=THREAD_JOIN_BLOCK_FLAG=1 and ESI=timeout for blocking join |
| THREAD_DETACH=0x154 | EBX=TID, ECX=EDX=0 |
| THREAD_EXIT_CODE=0x155 | EBX=signed 32-bit code, ECX=EDX=0; does not return on success |

ThreadCreateSpec consists of six u32 fields: size=24, version=1, entry,
argument, trampoline, flags. ThreadCreateFlags is a u32 enum class: Joinable=0,
Detached=1; unknown flags are rejected. Entry and trampoline must point into
the caller's readonly code region. Argument is opaque; the kernel does not
dereference it. Invalid pointers, sizes, versions or reserved values fail
before publishing a thread. Stack preparation or record-allocation failure
rolls back pages and does not consume a TID.

For exclusive stack top T, the kernel prepares `[T-12]=0`, `[T-8]=entry`,
`[T-4]=argument`, then starts at the supplied trampoline with ESP=T-12. The
current compiler contract requires 4-byte alignment. The SDK's C ABI
`sino_thread_start` calls `int entry(void*)`, sends its return value to
THREAD_EXIT_CODE, and terminates the process with an error if that syscall
unexpectedly returns. User stacks retain 16 KiB mappings and unmapped spacing.

ThreadResult consists of size=16, version=1, tid and signed code. Both blocking
and nonblocking join consume a completed result exactly once, only after the
entire output has been validated and copied. Live nonblocking join returns
S_AGAIN without changing output or claiming the target. Blocking timeout
is 0..60000 ticks, zero indefinite; timeout returns S_TIMEOUT without consuming
the result. The existing timeout-before-retry rule and its review TODO remain.

Only one blocked join consumer may claim a target. Other join attempts or
detach return S_BUSY while that claim exists. A timed-out, canceled or failed
join releases the claim; output is revalidated on retry, so unmapping it does
not lose the target's result. Self-join and joining an active detached target
return S_INVALID. Stale, foreign, consumed or invalid TIDs return S_BADHANDLE.
Detaching a live joinable target discards its future join obligation; detaching
an exited target consumes its record. Repeated detach of a live detached target
is S_INVALID; after reclamation the old TID is stale.

Joinable exit immediately unmaps the worker stack but retains an EXITED record
in its thread slot, backed by one private THREAD Object. It retains no extra
user handle. The record competes for the existing global Object quota and is
freed by successful join, detach, or process reclamation. Detached/legacy exit
reclaims the slot directly. Records count against the fixed slot quota but
not as live execution: the last live thread ends the process even if records
remain, with its explicit exit code (zero for the legacy exit call). SYS_EXIT
still ends the whole process. Exit, kill and fault cleanup revoke all claims,
queues, timers, records and mappings.

TIDs are positive `(generation << 3) | (slot + 1)`, with a global nonwrapping
28-bit generation and three low slot bits. Slot zero remains reserved for the
main thread; the three worker slots are reusable after join/detach. Generation
is assigned only at publication, and exhaustion returns S_LIMIT. The token
is resolved only in the current process; this does not promise unlimited
generation reuse or enlarge the 8-process / 4-thread limits.

SDK namespace `sino::thread` provides `create`, `query`, `join`, `detach`, `exit`
and `available`. `query` is a nonblocking join and consumes success, not a peek.
`create` uses the SDK trampoline and defaults to Joinable; `join` defaults to
an indefinite wait. No polling fallback or POSIX compatibility layer is added.

## Anonymous memory

`FEATURE_ANON_MEMORY` advertises anonymous mapping. Both strict and compatibility
processes may use it for their own address space. With `FEATURE_ANON_LAZY`, a
successful map reserves virtual addresses and charges the per-process quota.
Each page gets private zero-filled backing on first access. Kernels advertising
only `FEATURE_ANON_MEMORY` allocate and zero the entire mapping up front.

- `SYS_ANON_MAP` (`0x14e`) takes the requested byte count in EBX. The kernel
  rounds up to 4096 bytes and chooses a page-aligned base in
  `[0x40400000, 0x80000000)` and returns the positive base.
- `SYS_ANON_UNMAP` (`0x14f`) takes the exact live base in EBX and releases the
  entire region, returning `S_OK`. A released base may be reused by a later map.
- Both calls require ECX and EDX to be zero. Invalid inputs, including zero
  map size, rounding overflow, and unknown or interior unmap addresses, return
  `S_INVALID`. Each process can hold up to 32 regions and 16 MiB in total after
  rounding. Quota or address-space exhaustion returns `S_LIMIT`.

SDK `AnonMap` and `AnonUnmap` preserve the syscall status. `malloc` maps one
region per allocation and returns null on failure. `free(NULL)` is a no-op.
Valid non-null pointers passed to `free` must come from a live `malloc` in the
same process. Process teardown reclaims outstanding regions on exit or fault.

Phase E centralizes syscall user-buffer preparation. The kernel checks the
entire requested range for read or write permission before reading or writing
it. Nested request pointers are prepared before operations consume input, write
files, or publish handles or results. Bounded strings are read through their
first NUL, including a NUL at the end of a mapped page. An invalid range returns
`S_INVALID`. With lazy mappings, syscall preparation validates the full range
before allocating missing pages. On physical OOM the syscall returns `S_NOMEM`
before consuming input, modifying the file or writing the caller's output.
Any pages already prepared remain owned by the mapping and are reclaimed on
unmap or exit. A user instruction that reaches physical OOM at first touch
terminates the process with `PROCESS_FAULTED`, vector 14. Protection violations
and references outside live regions follow the usual fault path. Program images,
argument pages and thread stacks still receive backing at creation.

## SNX image layout and physical pool

SNX v1 and v2 use the same magic, 32-byte header and link base `0x40000000`.
The `abi` field selects the layout. v1 images retain the 192 KiB limit and
their original argument page and stack slots. v2 `memory_bytes` may reach
16 MiB, including BSS. Both versions retain the 64 KiB on-disk file limit.
`SystemInfo.image_bytes` reports the largest supported in-memory image.

v2 places its read-only argument and executable strings at `0x80000000` and
its first stack top at `0x80010000`. The remaining stacks are spaced 32 KiB
apart with 16 KiB mapped per stack. The dynamic allocator skips pages occupied
by the actual image. New v2 programs build with `SINO_SNX_ABI=2` and link with
`--defsym=snx_abi=2`. The repository build selects this through the app's ABI
entry. The daily applications `conhost`, `gsh`, `gcmd`, `tview` and `writer`,
plus the `memv2` probe, use v2. The remaining test programs stay on v1,
including the explicit v1 compatibility and address-layout probes.

The kernel publishes E820 RAM above 64 MiB after switching to its formal
supervisor page tables. The identity window ends at 1 GiB, where SNX user
addresses begin. Pages marked unavailable, boot reserves and allocator metadata
stay outside the pool. Development, release and acceptance VMs default to 4096 MiB
and one virtual CPU, verified with QMP. There is no 64 MiB runtime compatibility
requirement. Non-PAE ignores RAM remapped above 4 GiB; installed RAM is not the
allocatable page count. ShellBridge-only read-only probes inject allocation failures
for their own page directories; ordinary kernels contain no injection backend.

## Trusted selection and old application compatibility

PROCESS_COMPAT=0 retains existing behavior. All existing launch paths continue
to use it. Existing caps=0 applications retain their old public read APIs;
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
The strict policy also allows ANON_MAP and ANON_UNMAP for the calling process.
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

The kernel supports opt-in blocking for SYS_PROCESS_RESULT and SYS_RUN_RESULT. Keep the
handle in EBX and the output address in ECX, set EDX to `RESULT_BLOCK_FLAG` (1),
and put the timeout in ESI. This selector implies the fixed v1 output size
(32 bytes for SYS_PROCESS_RESULT or 48 bytes for SYS_RUN_RESULT). All valid
capacities continue to select the nonblocking mode.

The timeout accepts 0..60000 guest ticks, with zero selecting an indefinite
wait. If the result is pending, the calling thread sleeps until publication or
its deadline. On wake the kernel revalidates the handle and output range before
copying. Wait expiry returns S_TIMEOUT and leaves the output and child intact.
Closing the exact local handle from a sibling thread returns S_BADHANDLE to the
waiter. A duplicate handle remains a separate observer.

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
PIDs remain opaque positive integers, now `(generation << 4) | (slot + 1)`:
8 slots, a persistent nonwrapping 27-bit generation per slot, and zero reserved.
A failed launch reservation does not consume a PID. A slot becomes reusable only
after execution resources, wait nodes and ownership are released. Historical
completion records retain their original full PID; they never rebind a reused slot.
At the last generation the slot retires; unavailable slots return `S_LIMIT`.
Object generations are independent 32-bit per-object-slot counters. Handles keep
`(cookie << 8) | (slot + 1)`, with 23-bit cookies allocated globally per low-byte
slot and balanced across eligible free slots. Neither table teardown nor PID reuse
resets these counters. See [identifier lifetime](identifier-lifetime.md) for budgets.

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

Run repository-root `regression.ps1 -Suite policy`. The new poltest.snx executes in actual
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

G5a appends SYS_CALENDAR=0x14b for a versioned 32-byte read-only RTC calendar query in both policies, preserving SYS_TIME. ABI, failure-output rules and tests are documented in [shell-tools.md](shell-tools.md). G1-G5a frontend/Shell integration is now available through the opt-in user Shell entry.

G5b appends SYS_WORKSPACE=0x14c (trusted read-only boot frontend only) and SYS_DIRECTORY_COMMIT=0x14d (owned COMMIT directory required). Strict ambient Sync remains denied. See [workspace-shell.md](workspace-shell.md).

## Native text console

SYS_CONSOLE=0x150 is restricted to the conhost process created by the trusted kernel boot entry. Strict processes, writable copies and ordinary launches of conhost receive S_DENIED. EBX=0 reads one decoded PS/2 key (ECX=EDX=0; zero means no input); EBX=1 presents a ConsoleFrame at ECX with EDX=sizeof(ConsoleFrame). The kernel validates the complete read buffer and cursor before writing 2000 VGA cells. Cursor 2000 hides the caret. All existing syscall IDs and feature bits stay unchanged. See [text-console.md](text-console.md).

用户缓冲区准备、部分匿名页驻留与复制失败的内核契约见 [mm-access.md](mm-access.md)。
