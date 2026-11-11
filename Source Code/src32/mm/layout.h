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
 * | 0x00011000 - kernel end     | Kernel image (.text/.rdata/.data) |
 * | ...       - 0x00080000      | Kernel main stack (grows down)    |
 * | 0x00090000 - 0x00091000     | Bootstrap page directory          |
 * | 0x000A0000 - 0x00100000     | Video memory / BIOS ROM, 384 KiB  |
 * | 0x000B8000 - 0x000B9000     | VGA text buffer                   |
 * | 0x00100000 - bss end        | Kernel .bss                       |
 * | bss end    - 0x001E0000     | .bss growth room (limit 0x1D0000) |
 * | 0x001E0000 - 0x001F0000     | Bootstrap page tables (up to 16)  |
 * | 0x001F0000 - 0x00200000     | Unused, 64 KiB                    |
 * | 0x00200000 - 0x004D0000     | FAT12 RAM disk (boot volume)      |
 * | 0x03400000 - usable         | Page allocator pool               |
 *
 * kernel end and bss end change with every build (boot-manifest.json:
 * kernelBytes, bssEnd).  The boot image reserves 156 KiB for the kernel, and
 * build-image32.js requires bss end < 0x1D0000 so that .bss stays clear of
 * the bootstrap page tables.
 *
 * Everything below BOOT_POOL_START is reserved and not managed by the
 * page allocator.  The pool extends to the end of usable memory;
 * paging_kernel_init identity-maps at least BOOT_IDENTITY_BYTES. */

#define E820_COUNT_ADDRESS  ((phys_t)0x4f00u) // 如上，E820 的条目数量
// E820是一种BIOS提供的内存布局查询接口，bootloader 通过其获得内存地图并放在0x4F00, 数组从0x5000到5300
// 300字节可放32条（page.c 里的E820_MAX）、每条24字节的记录，来标明 是否可用、是否保留
#define E820_TABLE_ADDRESS  ((phys_t)0x5000u) //  E820 条目数组
#define KERNEL_STACK_TOP    0x80000u // 启动阶段の内核主栈顶
#define BOOT_PD             ((phys_t)0x90000u) // 启动阶段使用的Page Dir(1024个PDE)
#define VGA_TEXT_BASE       ((phys_t)0xb8000u)
#define BOOT_PT             ((phys_t)0x1e0000u) // 启动页表（64kb，每个4kb，共16个page table）
// Bootloader 使用这些页表，建立覆盖64Mib的恒等映射
#define RAMDISK_BASE        ((phys_t)0x200000u) // 2.81Mib 加载在内存中的 FAT12 文件系统镜像
#define BOOT_POOL_START     ((phys_t)0x3400000u) // 物理页分配开始的地方，往上分
#define BOOT_IDENTITY_BYTES 0x4000000u // 64Mib

/* ── Kernel virtual address layout ──
 *
 * 0x00000000 - direct_map_end   Direct mapping (virt == phys)
 * USER_BASE  - USER_V2_END      User space (per-process, defined in api.h)
 * KMAP_BASE  - KMAP_BASE+4MiB   KMAP window (PDE 1022, shared page table) */
#define PHYS_LIMIT_PAGES    0x100000u   /* 4 GiB in page frames */
#define KMAP_PDE_INDEX      1022u // KAMP占页目录第1022项
#define KMAP_BASE           0xFF800000u
#define KMAP_SLOTS          8u

#endif
