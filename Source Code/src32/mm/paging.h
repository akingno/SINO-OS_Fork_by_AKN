#ifndef SINO_MM_PAGING_H
#define SINO_MM_PAGING_H

#include "../types.h"

enum { PAGE_PRESENT = 1, PAGE_WRITE = 2, PAGE_USER = 4 };
#define BOOT_KERNEL_PD ((u32*)0x90000u)

/* Phase B has one private user table at user_base. Other PDEs are shared. */
u32* paging_user_create(const u32* kernel_pd, u32 user_base);
void paging_user_destroy(u32* pd, u32 user_base);

/* A successful map transfers ownership of page to pd. Unmap returns it. */
int paging_user_map(u32* pd, u32 va, void* page, int write);
void* paging_user_unmap(u32* pd, u32 va);
u32 paging_user_flags(const u32* pd, u32 va);
u32 paging_user_count(const u32* pd, u32 user_base);
void paging_activate(const u32* pd);

#endif
