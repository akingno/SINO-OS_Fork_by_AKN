#ifndef SINO_MM_PAGE_H
#define SINO_MM_PAGE_H

/* The current boot page tables identity-map the first 64 MiB. */
#define BOOT_IDENTITY_BYTES 0x4000000u

void page_init(void);
void page_allocator_test(void);

#endif
