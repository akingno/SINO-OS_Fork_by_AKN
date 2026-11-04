# AKN host Shell entry

From the repository root on Windows, run `powershell -NoProfile -ExecutionPolicy Bypass -File .\test.ps1 -SelfTest` to build a command-enabled image and check the COM1 Shell transport in a private headless VM. Use `-Command 'echo hello'` for a single command, `-ExpectedExitCode 1` for an expected Shell error, and `-TimeoutSeconds 5` for a command deadline. Results and raw output are under the printed `Build Output/dev/runs/shell-...` directory. See [Shell-Testing.md](../../Documentation/Shell-Testing.md).

This entry is separate from the upstream regression gate. The gate's suite count is unchanged. Normal builds leave the command bridge disabled. Existing GUI/keyboard regression tests remain applicable.

# Build, execution, and regression testing

## Stage 2 text compatibility baseline

Stage A coverage additionally runs poltest.snx in compatibility and strict
policies; its fixed-wire queries, denied ambient calls, local object operations,
normal completion and reclamation are checked by stage2-guest.js. See
[os-interface.md](os-interface.md).

From the repository root, run `powershell -NoProfile -ExecutionPolicy Bypass -File .\stage2-test.ps1` after each Stage 2 implementation step. It builds a new normal kernel with the explicit Shell bridge, holds the build lock through verification, and uses matching symbol snapshots and private headless VMs. It combines the existing infrastructure/RTC/Shell contracts with real Ring 3 pointer/permission/handle/file/resource checks, application logic selftests, loopback networking, and cold-boot persistence/install recovery. A deliberately failing guest separately verifies rejection and VM cleanup.

The baseline excludes Paint, GUI text editor workflows, visual designer interactions, and scaling/dragging/pixel assertions. It does not replace the complete 22-suite gate. Results and raw output are under the printed `Build Output/dev/runs/stage2-.../` directory; any missing, failed or unexecuted group prevents success. See [Stage2-Testing.md](../../Documentation/Stage2-Testing.md) for the command, report layout, exclusions and current exit-status/old-binary coverage limits.

## src32 infrastructure contracts

Run `node 'Source Code/tools/test-src32-refactor.js'` from the repository root. Set `SINO_TOOLCHAIN` to the GCC/Binutils bin directory; Python is selected through `SINO_PYTHON` or the existing host-tools lookup. Optional `SINO_REFACTOR_REPORT_DIR` selects the output folder; by default a new `Build Output/dev/runs/src32-contract-*` folder is used.

This standalone suite has 43 checks. It compiles the actual `storage.c` with only BIOS handoff address reads replaced by host variables, using an in-memory disk and a controlled VFS boundary. It also tests the actual klog byte/hex output and locale query/format code, including UTF-8 names and capacity errors. The resource generator runs in a private copy, where missing/duplicate keys and bad parameters must be rejected. No runtime source or daily disk is modified. Source hashes and each result are saved to `src32-refactor-report.json`.

The seven pre-refactor defects are repaired and no longer exempted. Every failed assertion exits nonzero, including when the historical `--baseline` option is supplied. This suite remains separate from the 22-suite system gate. See [runtime-infrastructure.md](runtime-infrastructure.md) for the resource source and logging contracts.

For the system baseline, build both normal and SELFTEST images, use the ISO and matching symbols from that build, then run `verify-system.js`; run both host Shell self-test suites separately. VM suites use private disks as described below.

## Build environment and existing regression gate

Host: existing w64devkit GCC/Binutils (override with SINO_TOOLCHAIN), bundled NASM 3.02, Node.js and Python for build-time locale validation/generation (override with SINO_PYTHON). Pillow is used for screenshots. No new large library is required. Guest: 32-bit x86, 64 MiB RAM, standard VGA/VBE, PS/2, BIOS/MBR, and ATA IDE. Current QEMU runs with networking disabled; the guest loopback stack is independent.

    powershell -NoProfile -ExecutionPolicy Bypass -File build.ps1 -TestImage
    node build-iso.js
    node verify-system.js
    powershell -NoProfile -ExecutionPolicy Bypass -File tools/test-launcher.ps1
    powershell -NoProfile -ExecutionPolicy Bypass -File tools/test-setup-entry.ps1

The first command must succeed before building an ISO, so a stale image is not tested. Build checks enforce a flat kernel of at most 159,744 bytes, BSS below reserved page tables, user files no larger than 64 KiB, and image/data limits of 192 KiB. PE/COFF is only an intermediate host link format. build-release.ps1 builds the normal release and ISO without a test image.

verify-system.js currently runs 22 suites: the prior 18-suite gate plus calculator, clock, Paint hiding and test-files.js. QEMU instances use separate disk images and dynamically allocated local QMP ports. Core regressions do not open Data/system.img or Data/data.img. GUI input uses actual keyboard/mouse events; assertions inspect guest memory and CRC-valid persisted snapshots. Screenshots are real QEMU framebuffers, not mockups. A passing test exits zero; failed assertions are not counted as success.

Calculator tests: `node tools/test-calculator.js`, with SINO_ISO and matching build32/kernel-normal.exe from the same build. `--legacy` is only for recording the pre-migration baseline; current images must pass the default native mode. The shared calculator-test.js helper checks USERAPP ownership against the actual calc.snx process and reads the copied display list, rather than the obsolete Window.value/error fields. Coverage includes all twenty buttons, keyboard arithmetic, multiple instances, real launch routes, zero-capability selftests, foreign-title rejection, Pinyin exclusion, full process/window tables, F10 termination, page/object reclamation, bilingual changes and 800x600. The user-mode probe covers the title syscall's string, type, rights and stale-handle boundaries. The regression cache fingerprints include the shared calculator helper.

run-selftest.js still checks the remaining kernel desktop/editor selftests; calculator arithmetic is now tested in Ring 3 by the same code used by the GUI. A Shell run response is not accepted as a native selftest pass. The historical 18-suite statements below describe earlier releases.

Clock tests: `node tools/test-clock.js` requires the current ISO and matching normal symbols. `--legacy` records only the pre-migration baseline. The default checks the actual clock.snx owner and user display list, hand geometry and backing-surface ellipse pixels; checking the kernel top-bar string alone does not establish a user clock. It covers desktop singleton/restore, launch aliases, zero capabilities, pause/resume, fault and F10 cleanup, exhausted tables, bilingual/800x600 display and independent top-bar time. Desktop regression uses the same owner/scene helper.

The clock suite also compiles `tools/test-rtc.c` against the actual rtc.c driver with a deterministic register backend. It tests decoder/sampler/cache failures and recovery, 12/24h, BCD/binary, dates, century fallback, UIP bounds and ticks wrap; no second decoder is copied into tests. `user/probe.c` checks SYS_TIME capacity, write ranges, guard crossing, output canaries and the signed-line/ellipse/Canvas boundaries. Cache fingerprints include clock-test.js and test-rtc.c. Direct RTC I/O remains privileged and is covered by the user-mode fault suite.

Paint tests: `node tools/test-paint-hidden.js` checks the absent built-in registry entry, global F5, menu row routing, filtered desktop/application aliases, direct legacy paths, lowercase/uppercase drawing associations, native Files with Editor installed, bilingual notices and byte-for-byte drawing/alias preservation across reboot. `--legacy` records the pre-hide baseline only. Fixtures use a private CRC-valid disk; the shared paint-fixture.js is included in regression cache fingerprints. Desktop regression also preserves a seeded drawing instead of creating one through Paint. IDE regression still requires application-owned F5 to run previews.

Coverage includes original GUI boot/apps, pointer, cached dragging, no-SSE2 fallback, file/clipboard/undo, snapshot-corruption recovery, Chinese/Pinyin and drag/drop, settings/mode timeout/1920 display, OS partition planning and exact system-sector readback, CD-free installed boot; CPL3 memory/I/O/opcode faults, spinning-process preemption, thread events, handle rights/references/stale reuse, private-resource reclamation; install/upgrade/downgrade/repair/uninstall/rollback/reboot recovery; packet checksum/loss/duplicate/window/bounds and UDP/TCP/ICMP/DNS/HTTP local tests; visual designer, breakpoints/step, packaged independent BASIC app, reboot project load; and native editor/file/task operations.

Artifacts: build32/manifest-normal.json, manifest-test.json, system-verification.json, and vm-tests/*/report.json plus PNGs. Host launcher/setup results are under build32/launcher-qa and setup-entry-qa. Current release records and full-disk backup information are under the neighboring Documentation/Licenses folder and external SINO archive. Git stage reports document checks performed at each commit.

Limitations: no hardware-in-the-loop, external NIC/interoperability tests, security certification, or comprehensive malformed-package/BASIC fuzz campaign. The UI is a hybrid migration; crashes in remaining Ring 0 legacy applications can still affect the kernel. Snapshot storage has bounded catalogs/files and no power-loss hardware simulator beyond interrupted-transaction/reboot tests. Package CRCs detect corruption, not signatures.

## SysBASIC 0.2 / 2026-10-24

node tools/test-ide.js launches the IDE from the desktop software library and validates control movement/resizing, Chinese properties, event generation, F5 preview, F8 breakpoint/step/continue, and a single-file .snx. It moves the output to an empty directory and runs it. For .sxp it asserts that the application is not registered before installation, installs through the GUI, launches it from the software entry, uninstalls it, then reboots and verifies it remains absent with no recovery log. It also tests project save/reopen and unsaved-close protection. Screenshots are in build32/vm-tests/ide-qa.

Ring 3 probes submit both the original 64-command scene and a 256-command scene. They check rejection of oversized counts, invalid user pointers, and panel bounds while retaining reclamation/preemption regressions. SYS_SCENE validates the count before the full command range and does not require older clients to know the newer canvas size.

SysBASIC 0.2 release addendum: copies of the daily-use disk use random QA project names so an existing Untitled.prj is not blocked by overwrite protection. One InstallBegin disk-commit failure was observed; the original file remained intact and the log was recoverable. PIO idle waits now check both BSY=0 and DRQ=0. SDK Sync retries S_IO at most three times and yields; it returns success only after the kernel confirms the commit. SYS_SYNC records failure reasons in the kernel log. Passing retries do not establish the unique cause of the original failure; longer cold-disk/stress stability testing remains necessary.

## SysBASIC 0.3 release validation (2026-10-24)

Build with powershell -NoProfile -ExecutionPolicy Bypass -File build.ps1 -TestImage, then node build-iso.js. The full node verify-system.js run now contains 18 suites: the original 15 plus keyboard decoding, real key input/designer, and right-panel tests. Every failing suite exits nonzero. --resume reuses a passing result only when ISO, test scripts, and shared test-dependency signatures match.

SINO_TOOLCHAIN selects GCC/Binutils bin; SINO_QEMU_DIR selects the QEMU directory; SINO_PYTHON selects Python with Pillow; SINO_ISO overrides the test ISO path. The three-folder release layout automatically finds ../Compiled Runtime/SINO.iso; the development layout remains supported.

node tools/prepare-release-disk.js starts with a zero-filled private 128 MiB virtual disk and uses the real OS installer and native package service to make a clean preinstalled disk. It validates the First project, preview, standalone executable, GUI installation, and cold boot without an ISO. It never reads or writes the daily Data/system.img.

Run node tools/package-release.js <new absolute output directory> only after all 18 regression suites and the clean release disk pass. The exporter checks ISO/IDE/disk hashes and writes exactly three folders; it refuses to overwrite an existing directory. The delivery excludes personal files, Git, build32, old test disks, and third-party QEMU binaries. The SHA256 manifest's disk hash refers to the pristine pre-first-boot release state.

The release launcher is also tested through Windows CMD → PowerShell → visible GTK window, repeated launch/restore, F11 IDE, First execution, and actual keyboard input. Tests operate on private copies; the pristine delivery disk stays sealed.

## Stage 07: SysBASIC 0.2 workbench and two output modes

Completed: classic feature layout, directly runnable output, separate GUI-installer output, friendly software-library names, project-overwrite/close protection, and installer close protection. The BASIC interpreter and existing platform were extended incrementally; this does not claim VB6 compatibility.

All 15 regression groups, Windows startup/install entry, three populated disk copies, and actual deployment passed. Investigation notes, resource limits, rollback backups, build commands, and the full file list are in REPORT-IDE-0.2.md.

## Stage 08: interaction fixes

New projects are blank. The right-side tree, properties, object/event selectors, and operation panel have real behavior. Dedicated click/persistence checks, all 15 regression groups, populated disk copies, and the actual release passed. See REPORT-IDE-0.2.1.md.

## Files migration tests (2026-11-04)

`node tools/test-files.js --legacy` records the pre-migration Ring 0 baseline only. Current images use the default native mode. files-test.js verifies the actual files.snx owner and reads the user display list; legacy Window.folder/selected are not accepted as evidence of a user application. Existing desktop, Chinese drag/drop, settings, library, native-package and Paint tests now inspect native browser scenes and wait for real asynchronous results.

The private files-fixture.js snapshot seeds exact 64-KiB binary content and a software-library shortcut. The suite covers launch aliases, global Save Home and independent View-menu directory windows, new documents/directories, copy/rename/cut, in-place navigation, trash/restore/permanent deletion, cold reboot, binary copy, readonly source handling, conflicts, merged application launch, actual user ABI selftest, repeated close, deliberate fault with an independent clock, and full-window failure without kernel fallback. The shared scene/Files/fixture helpers are included in regression cache signatures. Full system verification also exercises the normal user-mode probe's foreign window/drop-handle checks.

Native Files owns its prompts and context menu, so the kernel modal/context globals are not a valid assertion for those surfaces. Tests check their public display list and guest filesystem results. A file appearing in RAM does not establish snapshot completion or UI refresh; cold reboot/snapshot checks and explicit bounded waits remain required. Core regressions use boot Files; native-apps testing separately installs and runs the newly built Files package. Updating an ISO alone does not upgrade an already installed user package.
