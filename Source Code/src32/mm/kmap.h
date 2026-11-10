#ifndef SINO_MM_KMAP_H
#define SINO_MM_KMAP_H
#include "addr.h"

/* Borrow a kernel pointer to a user backing page; the pointer is valid only
 * until the matching kunmap. While the direct mapping covers every allocatable
 * page both are trivial. Pages above the direct mapping will use mapping
 * slots instead, without changing the callers. */
static inline void* kmap(phys_t page) { return phys_to_virt(page); }
static inline void kunmap(void* pointer) { (void)pointer; }

#endif
