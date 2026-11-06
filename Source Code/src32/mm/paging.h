#ifndef SINO_MM_PAGING_H
#define SINO_MM_PAGING_H

#include "../types.h"

enum { PAGE_PRESENT = 1, PAGE_WRITE = 2, PAGE_USER = 4 };
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
