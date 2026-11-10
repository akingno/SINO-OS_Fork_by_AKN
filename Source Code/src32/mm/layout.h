#ifndef SINO_MM_LAYOUT_H
#define SINO_MM_LAYOUT_H
#include "../types.h"

/* ── Fixed low-memory physical layout ──
 *
 * | Physical range              | Usage                             |
 * |-----------------------------|-----------------------------------|
 * | 0x00000000 - 0x00001000     | Null page, unmapped               |
 * | 0x00004F00                  | E820 entry count (loader.asm)     |
 * | 0x00005000 - 0x00005300     | E820 table, up to 32 entries      |
 * | ...       - 0x00080000      | Kernel main stack (grows down)    |
 * | 0x00090000 - 0x00091000     | Bootstrap page directory          |
 * | 0x000B8000 - 0x000B9000     | VGA text buffer                   |
 * | 0x001E0000 - 0x001F0000     | Bootstrap page tables (up to 16)  |
 * | 0x00200000 - 0x004D0000     | FAT12 RAM disk (boot volume)      |
 * | 0x03400000 - usable         | Page allocator pool               |
 *
 * Everything below BOOT_POOL_START is reserved and not managed by the
 * page allocator.  The pool extends to the end of usable memory;
 * paging_kernel_init identity-maps at least BOOT_IDENTITY_BYTES. */

#define E820_COUNT_ADDRESS  ((phys_t)0x4f00u)
#define E820_TABLE_ADDRESS  ((phys_t)0x5000u)
#define KERNEL_STACK_TOP    0x80000u
#define BOOT_PD             ((phys_t)0x90000u)
#define VGA_TEXT_BASE       ((phys_t)0xb8000u)
#define BOOT_PT             ((phys_t)0x1e0000u)
#define RAMDISK_BASE        ((phys_t)0x200000u)
#define BOOT_POOL_START     ((phys_t)0x3400000u)
#define BOOT_IDENTITY_BYTES 0x4000000u

/* ── Kernel virtual address layout ──
 *
 * 0x00000000 - direct_map_end   Direct mapping (virt == phys)
 * USER_BASE  - USER_V2_END      User space (per-process, defined in api.h)
 * KMAP_BASE  - KMAP_BASE+4MiB   KMAP window (PDE 1022, shared page table) */
#define PHYS_LIMIT_PAGES    0x100000u   /* 4 GiB in page frames */
#define KMAP_PDE_INDEX      1022u
#define KMAP_BASE           0xFF800000u
#define KMAP_SLOTS          8u

#endif
