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
