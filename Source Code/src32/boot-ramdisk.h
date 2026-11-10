#ifndef SINO_BOOT_RAMDISK_H
#define SINO_BOOT_RAMDISK_H
#include "boot-layout.h"
#include "mm/layout.h"

#define RAMDISK ((const u8*)RAMDISK_BASE)
#define RAMDISK_SIZE (BOOT_SECTORS * 512u)
_Static_assert(RAMDISK_BASE + RAMDISK_SIZE <= BOOT_POOL_START,
               "RAM disk overlaps allocator pool");

#endif
