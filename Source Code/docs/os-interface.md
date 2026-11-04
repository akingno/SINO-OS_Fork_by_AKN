# OS interface: Stage A

Stage A adds discovery and an opt-in strict process policy. It does not yet add
standard streams, argv, file objects, delegation or waitable completion records.
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
Structured arguments belong to a later stage.

Feature bits: SYSTEM_INFO=1, STRICT_POLICY=2, START_INFO=4; currently features=7.
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
and START_INFO. Existing pointer, object-type, rights and resource checks still
apply. LOG remains diagnostics, not stdout; time and language are public data.

All other known calls return S_DENIED before any operation-specific side effect
or user-buffer access: path reads/listing/writes, Sync, Spawn, Kill, process
enumeration, named Lease, networking, clipboard, OpenPath, windows and desktop
drag/drop integration. Even a strict process cannot use legacy Spawn to start a
compatibility child, including through aliases or writable copies. Stage A
rejects spawning entirely; later delegated spawn must preserve strict authority.

This confines explicitly strict processes, not every legacy application or the
remaining Ring 0 desktop. No file selection grants, stream bindings or general
revocation system are claimed. New CLI launch mechanisms should default to
strict policy when introduced, without silently changing old launch semantics.

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
