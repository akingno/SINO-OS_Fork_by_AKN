# GUI Removal Plan (`cli` branch)

## Context
Remove the entire GUI subsystem (desktop, window manager, graphics compositor, built-in apps, GUI test suites) to simplify kernel development and testing. The system becomes ShellBridge-only for interaction. All CLI/terminal Ring 3 apps (gsh, gcmd, tview, writer) stay.

## Stage 1 — Decouple kernel callers from GUI

Remove GUI `#include` and function calls from 6 kernel files. After this stage, gui.c/graphics.c/settings.c still compile but are dead code.

### kernel.c
- Remove `#include "gui.h"`, `#include "settings.h"`
- Define `#define SCREEN_WIDTH 1024` / `#define SCREEN_HEIGHT 768` at top
- Replace `GUI_WIDTH`/`GUI_HEIGHT` with `SCREEN_WIDTH`/`SCREEN_HEIGHT` in `mouse_emit()` and `boot_framebuffer()`
- In `kmain()`: remove `settings_init()`, `gui_init()`, `gui_tests()` (SELFTEST), `gui_step()` calls
- Change boot message to "SINO CLI ready"

### console.c
- Remove `#include "gui.h"`, `#include "graphics.h"`
- Remove the `if (gui_active) gui_panic(...)` branch — always use VGA text panic

### filesys.c
- Remove `#include "gui.h"`
- Remove all 5 `gui_files_changed()` calls (lines 160, 210, 229, 265, 321)

### object.c
- Remove `#include "gui.h"`
- Remove `gui_user_close()` call in `object_unref()` for OBJ_WINDOW

### process.c
- Remove `#include "gui.h"`, `#include "settings.h"`
- Stub all GUI syscalls to return error codes:
  - SYS_WINDOW → `S_INVALID`, SYS_SCENE → `S_BADHANDLE`
  - SYS_WINDOW_TITLE/POLICY/SIZE → `S_BADHANDLE`
  - SYS_CLIP_GET → `return 0`, SYS_CLIP_SET → `S_OK`
  - SYS_OPEN_PATH/FILE_CLIP/DROP_ZONES → `S_INVALID`
- Remove all 4 `gui_files_changed()` calls (lines 1347, 1455, 1461, 1467)
- Keep SYS_UI_LANGUAGE (uses `settings_language` — still in locale.c)

### locale.c
- Change `extern int settings_language;` → `int settings_language;` (take ownership, default 0 = Chinese)

### shell_bridge.c
- Remove `#include "graphics.h"` (no actual symbol dependency)

**Verify**: build succeeds, `test.ps1 -SelfTest` passes.

## Stage 2 — Delete GUI kernel sources + build script

### Delete (7 files):
- `src32/gui.c`, `src32/gui.h`
- `src32/graphics.c`, `src32/graphics.h`
- `src32/settings.c`, `src32/settings.h`
- `src32/theme.h` (if exists)

### Modify:
- **build32.ps1** line 41: remove `'settings'`, `'graphics'`, `'gui'` from `$kernelSources`

**Verify**: build succeeds, `test.ps1 -SelfTest` passes.

## Stage 3 — Remove GUI Ring 3 apps + disk assets

### Delete (2 source files):
- `user/terminal_frontend.c` (term.snx / termqa.snx)
- `user/files.c` (files.snx)

### Delete (disk assets, optional):
- `disk32/UIASSET.BIN`, `disk32/PINYIN.TXT`, `disk32/CJK_OFL.TXT`

### Modify:
- **tools/build-user.js**: remove entries for `term`, `termqa`, `files` and their frontend-only compile/link branches
- **build-image32.js**: remove `term`, `files` from image entries; remove `termqa` from ShellBridge extras; remove `UIASSET.BIN`, `PINYIN.TXT`, `CJK_OFL.TXT` from assets

Correction after dependency review: keep `brokerqa` in both build-user.js and ShellBridge extras. It is the headless `tests/guest/interactive_tests.c` probe, used by the stable `interactive` and `shell-waits` suites and the kernel's `IX_PROBE` launch path, not a GUI app.

Note: `runtime.c` UI helpers (Window, Scene, ui_canvas_*) become dead code but compile harmlessly. Clean up later.

**Verify**: ordinary and ShellBridge builds succeed; packaged file bytes/clusters decrease and removed GUI entries are absent. The FAT12 image remains fixed at 2.88 MB, so the image/ISO file size does not shrink. Use the ordinary headless boot check (1 case) and `regression.ps1 -Suite 'cli,interactive,shell-waits'` (7 cases, preview with `-List`); the 25-case `test.ps1 -SelfTest` exceeds the repository's per-suite limit of 20 and is not run automatically.

## Stage 4 — Remove GUI tests + update catalogs

### Delete (~24 test files):
**Regression suite GUI leaves (8):**
terminal-gui.js, shell-gui.js, file-tools-gui.js, shell-tools-gui.js, workspace-gui.js, line-edit-gui.js, suites/files-gui.js, suites/gui-persistence.js

**Standalone GUI tests (15):**
test-desktop.js, test-files.js, test-recovery.js, test-typography.js, test-performance.js, test-iso.js, test-terminal-replacement.js, test-chinese-files.js, test-settings.js, test-compatibility.js, test-usermode.js, test-network.js, test-launcher.ps1, test-launcher-guest.js, verify-desktop.js

**Support**: gui-context.js (if exists)

Dependency cleanup: also remove obsolete GUI forwarding entries under tools/, the top-level verify-desktop.js, run-selftest.js (which requires the removed desktop selftest), the GUI-only vm-test/ui-test/files helpers, and the now-unused terminal_probe.c. This totals 47 deleted test/forwarding/helper files. Keep terminal/editing host models and all headless guest probes, including brokerqa.

### Modify:
- **regression-catalog.js**: remove 9 `gui:true` entries + `guiLeaf` helper
- **regression-suites.js**: remove gui array and terminal-gui/workspace-gui profiles
- **regression-cases.json**: remove GUI case keys
- **verify-system.js**: remove GUI test entries (keep test-keyboard.js)
- **docs/test-catalog.md**: remove GUI test sections
- **AGENTS.md**: remove GUI test references from system map, update test conventions
- **tests/support/regression-vm.js**, **tests/host/test-core.js**, **tools/shell-test.js**: remove GUI dispatch and screenshot paths; preserve private VMs, memory-only inspection, source identity and exact case-ID checks
- **tests/host/test-regression-plan.js**, **test-report-summary.js**: reject removed GUI selections and preserve the 261-case core manifest
- **dev.ps1 -VerifyOnly**: route to the existing headless process suite (8 cases); remove keyboard/screenshot verification from tools/dev-run.js
- **tests/host/test-locale.c**, **test-src32-storage.c**: use locale.c's language-state definition instead of defining a duplicate
- **docs/testing.md**, **tests/TESTING.md**: document retained entries and current selection counts

**Verify**: do not automatically run the 261-case core or 25-case SelfTest, which exceed repository limits. Run test-regression-plan.js (12), test-report-summary.js --group selections (4), verify-system.js (1 host keyboard program), the existing locale host program (10 checks), and regression.ps1 -Suite 'process,files,terminal,interactive,shell-waits,runner-rejects-failure' (18, preview with -List): 45 cases total. Compile the storage host fixture without executing its 25-case suite. Use one fresh ShellBridge build for the VM regression, inspect saved summaries, and check retained local imports/document links.

## Dependency Order
Stage 1 → Stage 2 (must decouple before deleting). Stage 3 and Stage 4 are independent but both require Stage 2. Recommended order: 1 → 2 → 3 → 4, one commit per stage.

## What stays unchanged
- Bootloader (boot.asm/loader.asm) — still sets VBE mode, screen is blank but harmless
- keyboard.c — pure scan code decoder, zero GUI dependency
- boot_framebuffer() — validates VBE, returns FB address for paging_kernel_init (mapping is harmless)
- Mouse IRQ handling — stays in kernel.c, coordinates clamped to SCREEN_WIDTH/HEIGHT
- api.h syscall enum values — unchanged, GUI syscalls just return errors
- OBJ_WINDOW enum in object.h — unchanged to avoid shifting values
