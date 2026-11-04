# Native application development

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

The loader removes all capability bits for the resolved, read-only FAT12 node `/boot/calc.snx`, including canonical/case aliases and launches from shortcuts, Shell and SYS_SPAWN. It does not apply this policy to arbitrary writable files with the same name. caps=0 prevents home writes, network operations and management of other processes; existing public read/clipboard interfaces remain available. Clock and the other legacy built-ins are separate pending migrations.
