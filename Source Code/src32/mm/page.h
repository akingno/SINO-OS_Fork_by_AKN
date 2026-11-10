#ifndef SINO_MM_PAGE_H
#define SINO_MM_PAGE_H
#include "../types.h"

/* The temporary mapping reaches the initial pool below 64 MiB. */
#define BOOT_IDENTITY_BYTES 0x4000000u
/* The temporary mapping must cover the entire initial allocator pool. */
#define BOOT_POOL_START 0x3400000u

void page_init(void);
int page_boot_usable(phys_t address);
void page_publish_extended(void);
void page_allocator_test(void);
void* page_alloc(void);
void* page_alloc_run(u32 count);
int page_free(void* page);
int page_free_run(void* page, u32 count);
extern u32 free_pages, usable_memory;

#endif
