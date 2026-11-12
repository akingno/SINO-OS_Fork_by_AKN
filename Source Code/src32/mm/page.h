#ifndef SINO_MM_PAGE_H
#define SINO_MM_PAGE_H
#include "../types.h"
#include "layout.h"

void page_init(void);
int page_boot_usable(phys_t address);
void page_publish_extended(void);
void page_allocator_test(void);
void* kpage_alloc(void);
void* kpage_alloc_run(u32 count);
int kpage_free(void* page);
int kpage_free_run(void* page, u32 count);
/* Internal guard ownership transition; only paging may call these. */
int kpage_guard_claim(void* page);
void kpage_guard_release(void* page);
phys_t page_alloc(void);
int page_free(phys_t page);

extern u32 free_pages, nr_pages;

#endif
