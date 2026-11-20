# Build and run the development version

Install Node.js, Python, GCC/GNU Binutils with i386 output, and QEMU for Windows. Python validates the language resources during builds. NASM 3.02 is included in the source tree.

From the repository root, open the development launcher or run:

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File .\dev.ps1
```

This builds an ISO and opens a QEMU window at the SINO text Shell. Save any pending guest changes and close the previous development VM before starting another run.

| Command | Purpose |
| --- | --- |
| `.\dev.ps1 -BuildOnly` | Build an ordinary ISO without starting a VM |
| `.\dev.ps1 -VerifyOnly` | Run the eight process checks in a headless VM with a private disk |
| `.\test.ps1 -Command 'echo hello'` | Send one command to a private ShellBridge VM |

## Tools and storage

- `SINO_TOOLCHAIN` points to the GCC/Binutils bin directory when the tools are outside PATH.
- `SINO_QEMU_DIR` points to the QEMU directory. The launcher also searches PATH and the standard Windows installation directory.
- `Build Output/dev/runs/<build-id>/` contains the ISO, source fingerprints, build metadata and any logs produced for that run.
- `Build Output/dev/data.img` is the persistent development disk. The release launcher uses its own `Data/data.img`.
- `Source Code/build32/` contains shared build intermediates. Use the root entry points to serialize builds in the checkout.

See [testing](<../Source Code/docs/testing.md>) for targeted checks and [Shell testing](Shell-Testing.md) for command results and timeouts. The boot image includes the project MIT license at `/boot/license.txt`. See [fork provenance](Licenses/Fork-Provenance.md) for the source license and NASM notice.
