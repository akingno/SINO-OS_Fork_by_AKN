#ifndef SINO_MM_PAGE_H
#define SINO_MM_PAGE_H
#include "../types.h"
#include "layout.h"

void page_init(void);
int page_boot_usable(phys_t address);
void page_publish_extended(void);
void page_allocator_test(void);

/* Kernel pages: zeroed, inside the kernel's direct mapping, used through the
 * returned pointer. Page tables, kernel stacks and every kernel structure. */
void* kpage_alloc(void);
void* kpage_alloc_run(u32 count);
int kpage_free(void* page);
int kpage_free_run(void* page, u32 count);

/* User backing pages: zeroed and addressed physically. The kernel reaches
 * their contents only through kmap. */
phys_t page_alloc(void);
int page_free(phys_t page);

extern u32 free_pages, usable_memory;

#endif
