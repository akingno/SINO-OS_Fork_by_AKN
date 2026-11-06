# Boot image capacity

The system boot payload is a 2.88 MB FAT12 image, 2949120 bytes and 5760
512-byte sectors. `src32/boot-layout.h` defines the shared layout. C includes it,
build32.ps1 passes its definitions to NASM, and tools/boot-layout.js reads it for
the image builders. The ISO uses El Torito media type 3, with its README placed
after the complete payload.

The layout uses two sectors per cluster, nine sectors per FAT, two FAT copies,
224 root entries and 321 reserved sectors. FAT12 cluster count stays below 4085.
The BIOS geometry is 36 sectors per track and two heads. The loader reads in
18-sector chunks and copies the entire image to physical address 0x200000.
A compile-time assertion in `src32/boot-ramdisk.h` keeps its end below graphics
assets at 0x700000.

`src32/fs/boot_fat12.c` derives cluster bytes and declared volume size from the
BPB and checks them against the reserved RAM image. Whole-file and range reads
use that cluster size. VFS mounts the files read-only under `/boot`. The system
installer uses the raw image in `src32/boot-ramdisk.h` to copy and read back
`BOOT_SECTORS` sectors before committing its MBR. The offline update helper checks
the new payload size and partition capacity and restores the same full range on
rollback.

## Verification

First run root dev.ps1 -BuildOnly and set SINO_TOOLCHAIN to the directory containing
gcc.exe and SINO_QEMU_DIR to the QEMU directory. Run the dedicated test with
`node "Source Code/tests/host/test-boot-expansion.js"` from the repository root.
Use a matching completed build and run this test without concurrent builds.

The test prints a compact result and saves expansion-report.json under
Source Code/build32/vm-tests/boot-expansion. It creates its own image and 128 MB
virtual disk. A fragmented fixture extends beyond the former 1.44 MB boundary
and reaches the final complete data cluster. It checks the full RAM image on CD
and HDD boots, exact installed bytes, whole-file output and FileReadAt across
fragments. It also checks ISO layout and rejection of truncated/mismatched images.
All installation writes target that private virtual disk.

Validated on 2026-11-05 with ordinary build dev-20261105-055251-124e8ffc,
kernel 148256 bytes, BSS end 1884768. Expansion checks passed 8/8 and the existing
test-iso.js passed catalog, payload and actual CD-only application boot checks.
Stage 2 gate stage2-20261105-055040-d4594dd7 passed 8/8 groups, 371 checks, exit 0.
Its saved result was inspected through stage2-summary.ps1. The expansion test
and ISO test VMs stopped on completion. Physical hardware and exhaustive BIOS
compatibility remain outside these QEMU results.
