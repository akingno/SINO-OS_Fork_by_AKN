#ifndef SINO_BOOT_RAMDISK_H
#define SINO_BOOT_RAMDISK_H
#include "boot-layout.h"
#include "types.h"

#define RAMDISK ((const u8*)0x200000)
#define RAMDISK_SIZE (BOOT_SECTORS * 512u)
_Static_assert(0x200000u + RAMDISK_SIZE <= 0x700000u, "RAM disk overlaps graphics assets");

#endif
