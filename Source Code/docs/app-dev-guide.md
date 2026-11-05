# Native application development

Stage C finite CLI runtime: see [bounded-cli.md](bounded-cli.md) for CliArguments,
opaque standard stream handles, binary I/O, retained results and the root run.ps1
host entry. It keeps main(void)/SNX1 and all older launch semantics intact.

Stage A adds versioned SystemInfo/StartInfo queries and trusted strict launches
through Shell `run -s`; old launches remain compatible. See
[os-interface.md](os-interface.md) for exact layouts, permissions and limitations.

Stage B adds SpawnTracked/GetProcessResult/CancelProcess and bounded completion
objects. A compatibility launcher gets an owned handle for a strict child; normal
exit codes and faults remain readable after cleanup. This does not change old
Spawn/run or add standard streams. Query FEATURE_COMPLETION and handle S_AGAIN;
closing an observer does not terminate the child. Details are in the same contract.

Use the existing w64devkit GCC/Binutils and bundled NASM; no host CRT is linked. `user/sdk.h` and `user/runtime.c` define ABI 1, bounded UI commands and file/process/message wrappers. Application entry is `int main(void)`. Static data and code are private; use Yield to wait for events. The PIT also preempts noncooperative user loops. Exit or return terminates the application and releases its handles/address space.

Example: `user/hello.c`. Add its build tuple to `tools/build-user.js`, describe files in a JSON manifest like `packages/hello.json`, and call `tools/package.js`. `build.ps1 -TestImage` builds both kernel variants, user applications and example package with the same toolchain. Build checks every required linker symbol and rejects files over 64 KiB or memory over 192 KiB. SNX1 stores magic/ABI/image size/entry/data offset/memory size in a 32-byte header. The host link uses PE/COFF as an intermediate only; the guest never loads a PE image.

Window/Scene/Event provide original UI presentation. The kernel copies a validated maximum of 256 commands per canvas (the original UiScene holds 64; UiCanvas holds 256); no framebuffer or kernel pointers are shared. `Arguments()` reads the immutable launch string. Shared threads must use globals/events rather than stack pointers to unmapped memory. Thread functions must call SYS_THREAD_EXIT, not return. Events are manual-reset and nonblocking: wait returns S_AGAIN until signaled. Yield bounds CPU use; application loops must handle failed API calls.

ABI restrictions: integer C only (no FPU/SIMD, libc, exceptions or dynamic linking), 8 processes, 4 threads each, 24 local handles, 15 pending input events per process, 8 desktop windows, 16 KiB stack per thread. Read/write returns unified negative errors from `src32/api.h`. Home write capability does not authorize writes to `/boot`, `/etc` or kernel memory. Untrusted writable legacy F32 programs are refused.

Handle duplication reduces rights and keeps a separate reference; cookies are unique per handle allocation, so duplicate handles cannot revive a closed slot. Clipboard APIs are shared with legacy desktop apps. SYS_WINDOW_POLICY enables close-request delivery so an editor can show its own unsaved-change dialog. Public data types are in src32/types.h and api.h, separate from kernel declarations. Native apps use Chinese-first UI; full per-application localization and adaptive layouts remain pending. SysBASIC currently needs roughly 1024x768 logical space for its complete fixed layout.

## Calculator migration / ABI-1 additions (2026-11-03)

`user/calculator.c` is the independent `/boot/calc.snx` application. F4, the SINO menu and `/Applications/Calculator.app` all launch it; each invocation has its own process and arithmetic state. It uses integer hundredths, a 44-command UiScene, and a 306x406 window. `run /boot/calc.snx /selftest` runs the same arithmetic implementation plus permission assertions and logs `CALCULATOR SELFTEST PASS`. Shell `run` still reports creation, not completion; tests wait for the actual user-mode completion marker. `/spawn-test` and `/title-test` support child-launch and cross-process title regressions without granting extra rights.

Two syscalls are appended after SYS_DUP; existing values and layouts are unchanged:

- `UiLanguage()` / SYS_UI_LANGUAGE: returns the current applied UI language (0 Chinese, 1 English). Read-only, no capability required.
- `WindowTitle(handle, title)` / SYS_WINDOW_TITLE: requires write rights on a window in the current process handle table. Copies a validated NUL-terminated user string of at most 63 UTF-8 bytes. Invalid/unreadable or unterminated strings return S_INVALID; foreign, stale, reduced-rights or wrong-type handles return S_BADHANDLE. Failure preserves the old title.

Window policy flags are WINDOW_CLOSE_REQUEST=1, WINDOW_APP_KEYS=2 and WINDOW_NO_IME=4, combinable as a mask. WINDOW_NO_IME keeps physical key messages available to non-text applications while the global Pinyin mode is enabled; text editors and older clients retain their default input behavior. The calculator polls the language every ten guest ticks and changes its own title/text without changing arithmetic state. Do not expect kernel literal translation to translate user-owned scene strings.

The loader removes all capability bits for the resolved, read-only FAT12 nodes `/boot/calc.snx` and `/boot/clock.snx`, including canonical/case aliases and launches from shortcuts, Shell and SYS_SPAWN. It does not apply this policy to arbitrary writable files with the same name. caps=0 prevents home writes, network operations and management of other processes; existing public read/clipboard interfaces remain available. Other legacy built-ins remain separate pending migrations.

## Clock migration / time and canvas additions (2026-11-03)

`user/clock.c` is the independent `/boot/clock.snx` application. F6, the SINO menu and Clock.app share a desktop singleton: repeated launches restore/focus the same owner PID, including while its first window is still pending. A dead PID is discarded on the next launch. Direct Shell or SYS_SPAWN launches can create separate instances; this is not a system-wide singleton service.

SYS_TIME is appended after SYS_WINDOW_TITLE. `Time(TimeOfDay *out)` passes an output capacity of 12 bytes. The struct has three u32 fields: hour (0..23), minute and second (0..59). Success copies a validated cached local RTC time. No capability is required. Invalid or non-writable user ranges and capacities below 12 return S_INVALID; absent or older-than-200-guest-tick samples return S_AGAIN. Failures do not modify output. The call performs no CMOS I/O and exposes no device registers, shared kernel pointers or setting operation. It is wall time, not SYS_TICKS uptime, and has no timezone conversion.

`src32/rtc.c` owns CMOS register selection with NMI enabled. It checks UIP, mode consistency, two matching snapshots, BCD/binary and 12/24-hour conversion, calendar ranges and leap years. Four attempts and at most 1024 UIP reads per snapshot bound a poll. The main loop polls at 100 guest ticks; the top bar refreshes from this cache when its displayed text changes. Shell date/time can request a due poll before formatting the same cache, including from synchronous scripts. Missing century registers (0 or 255) use century 20. The clock retains its last displayed sample on S_AGAIN and labels it unavailable; before a first valid sample it displays --:--:--.

UI_ELLIPSE is appended to the existing drawing enum; positive width/height define a filled ellipse inside the bounding rectangle, clipped to the client area. UI_LINE keeps its start-plus-delta layout and now accepts signed deltas bounded to +/-1000 horizontally and +/-714 vertically. Old positive lines extending beyond the client area remain accepted and clipped. There is no blanket permission to use negative rectangle/grid sizes. Rendering is integer-only and bounded; ellipse products use integer 64-bit multiplication, not floating point or 64-bit division.

`ui_canvas_begin/fill/text/line/ellipse` explicitly build a UiCanvas of at most 256 commands and report S_LIMIT instead of silently dropping overflow or text longer than 79 bytes. The old UiScene builders retain their 64-command limit. The clock submits 69 commands for a valid sample: background, two dial ellipses, 60 ticks, three hands, hub and two texts. Angle tables, geometry, formatting, language and refresh decisions live in the application; the kernel only supplies generic rendering/time mechanisms.

Clock `/selftest` validates time and denied capabilities; `/spawn-test` tests child launch. `/fault-test` renders its first scene then deliberately raises UD2 in Ring 3 to verify cleanup and desktop containment. These diagnostic modes grant no additional rights. Normal launch uses no arguments. The clock uses WINDOW_NO_IME and polls time/language every ten guest ticks, submitting scenes only when something changes.

## Files migration / desktop integration (2026-11-04)

F1, the SINO menu, Files.app, directory opens, sidebar targets and the standard desktop disk/application/trash icons now start `/boot/files.snx` with a directory argument. The kernel refuses legacy FILES window allocation; its file-list renderer, navigation and selection handlers are removed. The enum and Window layout remain stable for ABI/test compatibility. The native program owns listing, filtering, merged application-library policy, selection, scrolling, dialogs, clipboard naming/copy policy, rename, cut, trash/restore and deletion. Each browser window is an independent process, subject to the existing eight-process/window limits. A missing/bad boot Files image reports failure instead of using a Ring 0 browser.

The resolved read-only FAT12 `/boot/files.snx` node has capabilities restricted to the launcher's CAP_HOME bit; this never grants CAP_HOME to a parent that lacks it, and removes network/manage bits. Normal desktop launches supply CAP_HOME. Ordinary file opening still uses the existing TextEdit/other desktop dispatch: this stage does not migrate the editor. User-installed Files packages remain independent copies and need explicit update; default desktop browsing uses boot Files without requiring package installation.

Four syscalls are appended after SYS_TIME; earlier numbers and structures are unchanged:

- `OpenPath(path)` / SYS_OPEN_PATH: requires CAP_HOME, copies/canonicalizes an absolute path of at most 127 bytes and requests existing desktop dispatch for a directory, legacy .app, .snx, .sxp, .lnk or ordinary document. It is not a kernel command evaluator. Missing paths return S_NOTFOUND; a zero response indicates dispatch was requested, not that a child completed. Subsequent desktop failures can have a visible notice. Existing Paint/SNP denial stays active.
- `FileClip(op, FileClipboard*)` / SYS_FILE_CLIP: shared typed file clipboard, separate from text clipboard. FileClipboard is 132 bytes (path[128], u32 cut). op=0 reads a fully initialized value into a writable buffer; op=1 sets a copied/canonical path with cut=0/1; op=2 clears it. The buffer must be accessible for the operation (clear ignores its contents). Set/clear require CAP_HOME; a cut source must be in the writable home subtree. The kernel binds its own node generation; deleted/reused sources fail rather than selecting an unrelated file. Copying/naming is performed by the client, not this syscall.
- `DropZones(window,zones,count)` / SYS_DROP_ZONES: requires a writable owned window handle and CAP_HOME. Up to 32 UiDropZone values specify client rectangles, source/target flags and NUL-terminated absolute paths. Target paths must resolve to directories; rectangles must lie within the client window. The kernel validates the complete update before replacing the old registration, keeps internal generation-bound references and releases registrations when the window closes or process faults. count=0 clears registration. Invalid pointers/count/geometry/flags/type are rejected. No Node ID, kernel pointer or framebuffer is exposed.
- `GetWindowSize(window,WindowSize*)` / SYS_WINDOW_SIZE: readable owned window handle, writable 8-byte output and capacity >=8; copies the current logical client width/height. Wrong/stale/foreign/reduced-rights handles and malformed output ranges are checked using the same object table as other window operations.

WINDOW_DESKTOP_COMMANDS=8 opts a window into desktop command messages and generic resize/zoom interaction. MSG_COMMAND carries existing desktop command numbers (1 new window, 2 new directory, 3 new document, 6 open, 8 rename, 9 trash/delete, 10 home, 11 applications, 12 refresh, 14 move, 15 open path). Global refresh (12) and Save Home (13) remain shared desktop/storage actions; close remains a window-manager action unless WINDOW_CLOSE_REQUEST is also enabled. Menu edit actions arrive as key codes. MSG_FILES_CHANGED is a coalesced invalidation hint; applications relist and own their state. MSG_SCROLL carries a signed row delta. Event queue capacity remains bounded; these are notifications, not durable completion records.

Cross-window/desktop drag presentation and the actual checked VFS transfer remain shared desktop infrastructure. Clients register explicit hit regions; the kernel checks stale references, readonly targets, writable home scope and name conflicts at drop time. Readonly sources may be copied into home; writable home sources move. Native Files does not receive raw disk privileges. The desktop's own icons/context/file actions still reside in Ring 0; this does not claim migration of the complete desktop service.

`files.snx /selftest` runs actual user-side rights, pointer, geometry, clipboard and capability checks and emits FILES SELFTEST PASS only after assertions. `/fault-test` draws the real application and deliberately executes UD2 to verify cleanup/isolation. `/boot/files.snx` is always provided by normal image packaging. Full file bytes remain limited to 64 KiB; native copying uses bounded static storage, not the 16-KiB user stack. Folder copy remains unsupported (as with the old file-copy path); moving folders and permanent deletion of empty folders retain VFS rules.

## Stage D explicit file authority

Stage D adds GrantDirectory, StartRunWithDirectory/StartDirectory, OpenAt,
FileReadAt/FileWriteAt, versioned enumeration, and private FileStage/Publish/Commit.
Strict programs need explicit directory handles; legacy global Read/Write/Sync
remain denied to them. Existing GUI applications continue using their current
compatibility APIs. Full layouts, rights, failure semantics and limits are in
[file-objects.md](file-objects.md). The implementation and fstest.snx regression
client passed compilation and the Stage 2 text gate, including cold-boot byte
checks. Remaining fault-injection coverage is documented there. Publication
alone does not confirm durability.

## Stage E byte pipes and bounded runtime waits

PipeCreate and StreamReady add bounded streams; StreamRead/StreamWrite accept
owned pipe endpoints. StartRunWithStreams (RunSpecV3) explicitly binds those
endpoints to strict child stdio; unused parent copies must be closed. Include
`user/stream.h` for StreamReadFor/WriteFor, StreamWriteAllFor, StreamWaitFor and
RunWaitFor/ProcessWaitFor. These helpers use monotonic deadlines and Yield, not
blocking kernel stacks. The Stage 2 text gate exercises real concurrent binary
transport, reference-controlled EOF, early close, faults, cancellation and timeout.
See [stream-pipes.md](stream-pipes.md) for rights, ABI, quotas, partial progress
and remaining coverage limits. Existing Shell pipes and host finite capture are
unchanged; interactive terminal sessions belong to Stage F.
