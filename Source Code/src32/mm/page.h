#ifndef SINO_MM_PAGE_H
#define SINO_MM_PAGE_H

/* The temporary mapping reaches the initial pool below 64 MiB. */
#define BOOT_IDENTITY_BYTES 0x4000000u
/* The temporary mapping must cover the entire initial allocator pool. */
#define BOOT_POOL_START 0x3400000u

void page_init(void);
int page_boot_usable(u32 address);
void page_publish_extended(void);
void page_allocator_test(void);

#endif
