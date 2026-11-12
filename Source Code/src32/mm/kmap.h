#ifndef SINO_MM_KMAP_H
#define SINO_MM_KMAP_H
#include "addr.h"
#ifdef __cplusplus
extern "C" {
#endif

/* Map a user backing page into kernel virtual address space; the pointer is
 * valid only until the matching kunmap.  Pages inside the direct mapping use
 * the identity window; pages above it occupy a KMAP slot with invlpg. */
void kmap_init(u32* table);
void* kmap(phys_t page);
void kunmap(void* pointer);
u32 kmap_free_slots(void);
void kmap_selftest(void);

#ifdef __cplusplus
}
#endif
#endif
