# Build and run the AKN development version / 开发运行

For headless execution of existing Shell commands, use root `test.ps1 -Command 'echo hello'` or `test.ps1 -SelfTest`. This entry builds a dedicated command-enabled ISO and uses a fresh private test disk. See [Shell testing](Shell-Testing.md) for output, status and timeout semantics.

Modify the canonical `Source Code/` tree. On Windows, install Node.js, GCC/GNU Binutils capable of i386 output, and QEMU; NASM is bundled under its retained license. Python/Pillow is needed for font generation and some upstream tests.

在仓库根目录双击 `启动开发版.cmd`，或执行：

```powershell
powershell.exe -NoProfile -ExecutionPolicy Bypass -File .\dev.ps1
```

Save guest files and close the previous development VM first. This rebuilds and restarts the system; it does not hot-update a running guest. A failed build does not launch an older ISO.

```powershell
# Build only; no VM is started.
.\dev.ps1 -BuildOnly

# Build, boot without a window, run the Ring 3 probe, then stop the test VM.
.\dev.ps1 -VerifyOnly
```

The default launch leaves a visible development desktop open. The launcher checks for an existing recorded development VM to avoid two instances writing the same development disk. `-BuildOnly` can build a separate ISO while the existing VM remains open.

## Tools and storage

- `SINO_TOOLCHAIN`: optional GCC/Binutils `bin` directory; otherwise use PATH.
- `SINO_QEMU_DIR`: optional QEMU directory; otherwise search the standard Windows installation and PATH.
- `Build Output/dev/runs/<build-id>/`: immutable-per-run ISO, logs, source fingerprints, build metadata, screenshots and verification report.
- `Build Output/dev/data.img`: persistent 16 MiB development-only disk; save guest files before closing QEMU.
- `Source Code/build32/`: intermediate output, shared by builds in this checkout. The development entry uses a lock; avoid invoking other builders concurrently.

These files and personal `akn_learning/` notes are not tracked. Extracted upstream distributions are also ignored. The launcher does not update their disks or images.

## What the checks establish

The launcher checks unchanged build inputs, the current boot image inside the ISO, a matching guest build ID, desktop readiness and the actual QEMU disk attachments. `-VerifyOnly` additionally sends keyboard input to run `/boot/probe.snx` and checks its result. This is a smoke check, not the complete current 21-suite regression gate. See [testing.md](<../Source Code/docs/testing.md>) for subsystem tests and use private test disks.

Changes to an ISO do not replace already-installed programs under `/home/user/Programs`. For early application checks, run the intended `/boot` version; use the installer when testing package updates. A fresh development disk does not contain the upstream release's preinstalled IDE.

## Licenses

New images include the project MIT text as `/boot/license.txt`, plus existing font/Pinyin notices. See [fork provenance and third-party licensing](Licenses/Fork-Provenance.md). Creating source commits is separate from publishing binary releases; the historical upstream release exporter requires its own audit before reuse with this fork's layout.
