#ifndef SINO_MM_ADDR_H
#define SINO_MM_ADDR_H
#include "../types.h"
#include "../panic.h"
#ifdef __cplusplus
extern "C" {
#endif

#define PHYS_NULL ((phys_t)0)

#define PAGE_SHIFT 12
#define PAGE_SIZE (1u << PAGE_SHIFT)      // 4096
#define PAGE_OFFSET_MASK (PAGE_SIZE - 1)  // 4095，低12位全1，高20位全0
#define PAGE_MASK (~PAGE_OFFSET_MASK)     // 低12位全0，高20位全1

// 页内偏移，去掉高位，只留低12位的结果
static inline u32 page_offset(u32 address) {
  return address & PAGE_OFFSET_MASK;
}
// 清除低12位，留高20位（和上面相反）
static inline u32 page_floor(u32 address) {
  return address & PAGE_MASK;
}
/* Fail without changing *out if the aligned byte value cannot fit in u32.
 * Physical range endpoints at 4 GiB use wide arithmetic/PFNs in e820.h. */
static inline int page_ceil_checked(u32 address, u32* out) {
  if (!out || address > 0xffffffffu - PAGE_OFFSET_MASK) return 0;
  *out = (address + PAGE_OFFSET_MASK) & PAGE_MASK;
  return 1;
}
// 如果低 12 位都是0说明对齐了
static inline int page_aligned(u32 address) {
  return !page_offset(address);
}

/* Direct mapping: virtual == physical for the kernel's low region.
 * direct_map_end is zero until paging_kernel_init sets it; the bounds
 * check is skipped during early bootstrap. */
static inline void* phys_to_virt(phys_t address) {
  extern u32 direct_map_end;
  if (direct_map_end && address >= direct_map_end)
    panic("phys_to_virt above direct mapping", address, direct_map_end);
  return (void*)address;
}
static inline phys_t virt_to_phys(const void* pointer) {
  return (phys_t)(__UINTPTR_TYPE__)pointer;
}

#ifdef __cplusplus
}
#endif
#endif
