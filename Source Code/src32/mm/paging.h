#ifndef SINO_MM_PAGING_H
#define SINO_MM_PAGING_H

#include "../types.h"
#include "addr.h"

/* Two-level x86 paging: one page table maps 4 MiB with 1024 entries. */
#define TABLE_SHIFT 22
#define TABLE_BYTES (1u << TABLE_SHIFT)
#define TABLE_ENTRIES (TABLE_BYTES / PAGE_SIZE)

enum {
  PAGE_PRESENT = 1,
  PAGE_WRITE = 2,
  PAGE_USER = 4,
  PAGE_WRITE_THROUGH = 8,
  PAGE_CACHE_DISABLE = 16
};

static inline u32 pde_index(u32 va) { return va >> TABLE_SHIFT; }
static inline u32 pte_index(u32 va) { return (va >> PAGE_SHIFT) & (TABLE_ENTRIES - 1); }
static inline phys_t entry_address(u32 entry) { return entry & PAGE_MASK; }
static inline u32 make_entry(phys_t page, u32 flags) { return page | flags; }

void paging_bootstrap(void);
void paging_kernel_init(u32 identity_bytes, u32 framebuffer);
void paging_activate_kernel(void);

/* User PDEs are private and created on first use. Other PDEs are shared. */
u32* paging_user_create(void);
void paging_user_destroy(u32* pd);

/* A successful map transfers ownership of page to pd. Unmap returns it. */
int paging_user_map(u32* pd, u32 va, void* page, int write);
void* paging_user_unmap(u32* pd, u32 va);
u32 paging_user_flags(const u32* pd, u32 va);
u32 paging_user_count(const u32* pd);
/* Search a page-aligned subrange of the dynamic window for unmapped pages. */
u32 paging_user_find_gap(const u32* pd, u32 start, u32 end, u32 bytes);
void paging_activate(const u32* pd);

#endif
