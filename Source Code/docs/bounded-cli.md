# Stage C: bounded CLI execution

This is a finite-input, finite-capture runtime for real SNX1 Ring 3 programs.
It preserves old run/Spawn and ABI-1 startup strings. This document describes the
finite backend; Stage D adds [file authority](file-objects.md), and Stage E adds
[pipe-backed streams](stream-pipes.md). Interactive terminals, arbitrary handle
inheritance, cwd/env and blocking syscalls remain outside these deliveries.

## Public ABI

Stage C feature: FEATURE_BOUNDED_RUN=16 (mask 31 at that stage; A-F now use 255).
Old feature bits and v1 layouts remain unchanged; clients mask known bits.
All structures use little-endian u32 fields, four-byte alignment, except the
signed completion code. User pointers below are 32-bit virtual addresses.

| ID | SDK wrapper | Registers after EAX |
| --- | --- | --- |
| 0x134 | StartRun | EBX=RunSpec*, ECX=48, EDX=0 |
| 0x135 | QueryCliInfo | EBX=output, ECX=capacity >=1120, EDX=0 |
| 0x136 | StreamRead | EBX=handle, ECX=output, EDX=capacity |
| 0x137 | StreamWrite | EBX=handle, ECX=input, EDX=bytes |
| 0x138 | GetRunResult | EBX=handle, ECX=output, EDX=capacity >=48 |
| 0x139 | ReadRunOutput | EBX=handle, ECX=RunRead*, EDX=16 |
| 0x13a | QueryRunLimits | EBX=output, ECX=capacity >=40, EDX=0 |

Control/query success is 0; stream/capture success is the actual byte count.
Errors retain existing negative values. Invalid flags, sizes, versions, pointers
or bounds return S_INVALID. Wrong-type/stale/foreign/insufficient-rights handles
return S_BADHANDLE. Capacity/record/object/process exhaustion returns S_LIMIT,
page allocation failure S_NOMEM. Failed queries do not modify output; larger
output capacities leave the tail untouched. These are fixed v1 layouts.

### Launch and arguments

RunSpec is twelve words (48 bytes): size=48, version=1, flags=0, path, argc,
argv_bytes, argv, offsets, input_bytes, input, timeout_ticks, reserved=0.

path is an absolute NUL-terminated guest path of at most 127 UTF-8 bytes. argv is
a packed byte block, not an array of pointers; offsets points to argc u32 values.
Each offset must equal the previous string's terminating NUL position plus one;
the first is zero and the final NUL must end argv_bytes exactly. This permits
empty arguments and embedded spaces/newlines, but no embedded NUL within one
argument. UTF-8 is a convention; the kernel transports bytes without recoding.
Zero arguments requires zero argv_bytes. The caller supplies the exact argument
list; no executable name is prepended automatically. No second Shell parse occurs.

Input is copied before the child runs and may contain any bytes, including NUL.
All nonempty user ranges are validated and copied; no user input pointer is kept
across scheduling. timeout_ticks is zero to disable the per-process deadline, or
1..60000 at the current 100 Hz rate. Deadlines use bounded modular tick differences.

Only compatibility launchers may StartRun. Success returns a WAIT|CANCEL process
handle for a caps=0 strict child. Strict callers cannot use StartRun, old Spawn,
or SpawnTracked to acquire a compatibility child. The child receives three newly
created stream handles, not the launcher's handle table, files or network grants.
The trusted development bridge uses the same launch transaction and limits.

### Discovery and standard streams

RunLimits is ten words (40 bytes): size=40, version=1, runs=2, argc=16,
argv_bytes=1024, input_bytes=4096, out_bytes=4096, err_bytes=4096,
io_bytes=65536, timeout_ticks=60000. These are maxima, not allocation promises.

CliInfo is 1120 bytes: size=1120, version=1, argc, in, out, err, argv_bytes,
reserved=0; then offsets[16] and bytes[1024]. Unused bytes/offsets are zero.
QueryCliInfo copies this process's immutable launch context into caller-owned
storage. It returns S_NOTFOUND, without output changes, for old/unbound launches.
Arguments()/Executable() keep their original meaning and addresses; managed
launches use an empty legacy argument string. No kernel pointers are exposed.

in/out/err are opaque local handles, not the numbers 0/1/2. stdin has READ=1;
stdout/stderr have WRITE=2. Existing Close/Dup rules apply. Duplicates share the
input cursor or output capture. Finite stream operations check the current run
binding; pipes instead check owned endpoint type/rights, as specified in Stage E.
Closing a stream then using its stale handle is an error, not EOF.

StreamRead returns up to the requested bytes, and 0 after finite input is
consumed. Repeated EOF stays EOF. StreamWrite appends bytes independently to the
two captures; no cross-stream ordering or text formatting is promised. Zero-byte
operations validate the handle/direction then return 0 without accessing data.
Nonempty operations validate the full user range and accept at most 65536 bytes
per call, with actual progress bounded by the finite buffers.

Exactly 4096 output bytes is allowed. A subsequent nonempty write beyond the
remaining capacity captures the fitting prefix, marks PROCESS_OUTPUT_LIMIT=5,
and terminates the process before it resumes. This is terminal capture overflow,
not pipe backpressure; data is never silently reported as complete success.

CliArguments(info, argv, capacity) queries context and builds pointers into info;
argv needs argc+1 slots and ends with null. The caller must keep info alive.
CliWriteAll wraps finite StreamWrite and propagates errors instead of spinning.
Normal applications can retain main(void), for example:

```c
#include "sdk.h"
int main(void) {
  static CliInfo info;
  char *argv[17];
  if (CliArguments(&info, argv, 17) < 0) return 1;
  u8 bytes[256];
  for (;;) {
    int n = StreamRead(info.in, bytes, sizeof(bytes));
    if (n < 0) return 1;
    if (!n) return 0;
    if (CliWriteAll(info.out, bytes, (u32)n) != n) return 1;
  }
}
```

### Results, ownership and cleanup

RunResult is 48 bytes: unchanged ProcessCompletion (32 bytes), followed by
input_bytes (consumed), out_bytes, err_bytes, incomplete. Normal nonzero exits
still have complete output. Fault/cancel/kill/overflow/timeout set incomplete=1;
successful bytes before termination remain available. Reason 6 is PROCESS_TIMEOUT.
The first terminal event still wins; a late cancel cannot erase exit/fault status.

GetRunResult requires WAIT rights and returns S_AGAIN, without output changes,
until execution resources have been reclaimed. It is repeatable. The kernel also
allows the raw SYS_RUN_RESULT call to opt into [blocking](os-interface.md#stage-b-completion-objects)
with the fixed 48-byte RunResult. ReadRunOutput
also requires WAIT rights and completion; it never drains a running program.
RunRead is four words: role (1 stdout, 2 stderr), offset, buffer, capacity.
It returns min(capacity, captured_length-offset) bytes, 0 at EOF, and S_INVALID
past EOF. Captures are immutable and can be read repeatedly at explicit offsets.

Each retained run owns one metadata page and three buffer pages (16 KiB), plus
ordinary process resources. The global two-run budget includes completed runs
whose observation handles remain open. Completion records still share Stage B's
16-record pool and existing object/handle limits. Child stream objects pin their
backing completion; cleanup releases streams, execution pages, then execution
ownership, while observers retain results/captures until closed.

Public StartRun retains Stage B close semantics: closing an observer, or observer
exit/fault, does not cancel a running child. The supervised host layer below
explicitly cancels its own tasks on release/session expiry. It never grants global
PID management or changes unrelated desktop tasks.

## Host entry

From the repository root in PowerShell:

```powershell
.\run.ps1 -Program /boot/clitest.snx -Arguments @('args', '', 'a b', '你好')
.\run.ps1 -Program /boot/clitest.snx -Arguments echo -InputFile .\input.bin
.\run.ps1 -Program /boot/clitest.snx -Arguments exit -ExpectedExitCode -17
```

clitest.snx is a development fixture; real programs use the same SDK. The entry
builds a fresh explicit-bridge ISO, holds the build lock, and uses a private disk
and headless VM. It does not attach the daily disk.
TimeoutSeconds defaults to 5, range 1..600. InputFile is read as at most 4096 raw
bytes; arguments are checked before building. It is not an interactive stdin pipe.

The printed cli-... directory contains stdout.out/stderr.out as raw byte files,
cli-report.json with actual typed status/counts, and shell-report.json with
source/ISO/mount/VM-cleanup evidence. Exit 0 means a normal, complete program exit
matching ExpectedExitCode; all faults, limits, guest timeouts, unexpected codes,
launch/transport/cleanup failures return 1. A fault's code=0 is not normal success.
Fault/limit output is saved before the expectation fails. Transport timeout is
reported as unconfirmed host failure, not invented guest cancellation.

## Development UART protocol v2

Old SCS1/SCR1 framing and test.ps1 commands retain their original semantics.
v2 requests are SCS2, id:u32, payload_length:u32, payload. Replies are SCR2 with
the old 20-byte envelope (id, RPC status, flags=0, payload_length), then payload.
The reply header status is an operation status, not the child's exit code.

Request payload begins opcode:u32 and a fresh nonzero 64-bit host nonce (two
words). Reply payload echoes that nonce, followed by the operation data:

| Opcode | Request body after nonce | Success body after echoed nonce |
| --- | --- | --- |
| 1 HELLO | empty; id=1, fresh nonce | RunLimits |
| 2 START | timeout, argc, argv_bytes, input_bytes; path[128]; offsets[16]; argv bytes; input bytes | opaque token:u32 |
| 3 POLL | token | RunResult, or S_AGAIN with no data |
| 4 CANCEL | token | empty; completion is polled separately |
| 5 RELEASE | token | cancel if running, then close this observation |
| 6 HEARTBEAT | empty | empty |
| 7 READ | token, role, offset, capacity<=4096 | raw captured bytes; status=byte count |

START's fixed body prefix is 208 bytes, path includes a NUL and zero padding,
unused offsets are zero. Body lengths are exact; maximum v2 payload is 5340 bytes
(5352 including envelope). A malformed header too short to contain a nonce may
return S_INVALID with no payload. An incomplete frame idle for 200 guest ticks
is discarded; the host must not reinterpret a late reply as another request.

IDs increase without wrap per session; replay returns S_INVALID. Wrong/inactive
session returns S_DENIED. A fresh HELLO cancels/closes only the previous bridge
owner's runs. Clients match magic, id and nonce before accepting typed results.
Nonce identifies a session, not authentication; this is an explicit development
channel on loopback, not an Internet service or authorization prompt.

The session lease is 200 guest ticks (2 guest seconds). Complete valid control
requests/heartbeats keep it alive. On expiry, only this session's pending tasks
are canceled and all its observations are closed. A dropped TCP connection needs
no guest socket notification: absence of heartbeats triggers cleanup. There is
no reconnect/resume promise; fresh sessions use fresh nonces. Guest scheduling
or synchronous kernel work can delay deadlines; none is a hard wall-clock SLA.
The host allows 5 wall-clock seconds per protocol request; this is independent
of the default 5-guest-second program execution deadline. After an incomplete
frame idles beyond two seconds, the old session also expires: recovery needs a
fresh HELLO/nonce, not another heartbeat on the expired session.

Only explicit ShellBridge builds enable UART IRQ4 and allocate an 8192-byte RX
ring plus an 8192-byte frame buffer. The IRQ drains at most 16 bytes without
parsing, allocation or launch. Main-loop RX/TX work is bounded to 128 bytes per
poll; TX uses 16-byte FIFO bursts. Overflow aborts the session rather than execute
an ambiguous frame. Normal builds leave this device path disabled.
The main loop also drains one interrupt-masked RX burst to handle coalesced edges
without giving the single-producer ring a concurrent writer.

## Validation boundary

Stage D adds a separate 64-byte RunSpecV2 for an explicit directory grant; the
48-byte v1 and host v2 protocol remain unchanged. See [file-objects.md](file-objects.md).
The core regression covers directory-grant contracts. Host run.ps1 does not supply grants.

regression.ps1 -Suite 'cli,managed-cli' includes real CPL3 CLI tests and a dedicated managed-protocol VM:
exact/max sizes, NUL/UTF-8, short reads and EOF, pointer/role/rights failures,
separate stderr, output limits, retained fault output, negative exits, quotas,
deadline/cancel, failed descriptors, replay/session isolation, truncated/oversize
frames, and disconnect cleanup while an unrelated user process remains runnable.
Old Shell/app/storage/network/cold-boot checks remain. This is not full POSIX,
hardware UART validation, exhaustive page-allocation fault injection, or the
excluded GUI editor/visual regression gate.
