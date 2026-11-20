# SINO code and third-party sources

SINO is an independently maintained fork of [SkysoftStudio-Felix/SINO-OS](https://github.com/SkysoftStudio-Felix/SINO-OS) at commit `bf0cad19002adf8f8f6d8be3efce1c044b05e252`. The bootloader, kernel, drivers and user programs use the project [MIT license](LICENSE). The boot image includes it at `/boot/license.txt`.

| Area | Source |
| --- | --- |
| BIOS boot and FAT12 layout | `src32/boot.asm`, `src32/loader.asm`, `src32/boot-layout.h` |
| Kernel and memory management | `src32/kernel.cpp`, `src32/mm/`, `src32/task/` |
| Files and snapshot storage | `src32/fs/`, `src32/vfs.cpp`, `src32/storage.cpp` |
| Text console and Shell | `src32/console.cpp`, `user/console_host.cpp`, `user/gsh.cpp` |
| Loopback networking | `src32/net.cpp`, `user/netlib.cpp` |

## Third-party components

| Component | Role |
| --- | --- |
| NASM 3.02 | Bundled assembler under BSD-2-Clause. See its [license](tools/nasm-3.02/LICENSE). |
| QEMU and its SeaBIOS/VGA firmware | External emulator and firmware installed by the user. [QEMU](https://www.qemu.org/) |
| GCC/GNU Binutils, Node.js and Python | External tools for compilation, image building, testing and language-resource validation. |

## Protocol and architecture references

- [Intel architecture manuals](https://www.intel.com/content/www/us/en/developer/articles/technical/intel-sdm.html) cover protected mode, paging, interrupts and exceptions.
- [QEMU QMP documentation](https://www.qemu.org/docs/master/interop/qemu-qmp-ref.html) covers host control and VM inspection.
- RFC 791, 792, 768, 9293 and 1035 describe protocols used by the loopback networking code.

See [fork provenance](<../Documentation/Licenses/Fork-Provenance.md>) for copyright and distribution notices.
