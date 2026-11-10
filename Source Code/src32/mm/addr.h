#ifndef SINO_MM_ADDR_H
#define SINO_MM_ADDR_H
#include "../types.h"

#define PHYS_NULL ((phys_t)0)

#define PAGE_SHIFT 12
#define PAGE_SIZE (1u << PAGE_SHIFT)
#define PAGE_OFFSET_MASK (PAGE_SIZE - 1)
#define PAGE_MASK (~PAGE_OFFSET_MASK)

static inline u32 page_offset(u32 address) { return address & PAGE_OFFSET_MASK; }
static inline u32 page_floor(u32 address) { return address & PAGE_MASK; }
static inline u32 page_ceil(u32 address) { return (address + PAGE_OFFSET_MASK) & PAGE_MASK; }
static inline int page_aligned(u32 address) { return !page_offset(address); }

/* Kernel memory is identity mapped, so both conversions are the identity. They
 * still mark every place where a physical address becomes a pointer or back. */
static inline void* phys_to_virt(phys_t address) { return (void*)address; }
static inline phys_t virt_to_phys(const void* pointer) { return (phys_t)pointer; }

#endif
