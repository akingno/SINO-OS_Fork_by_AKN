#ifndef SINO_MM_KMALLOC_H
#define SINO_MM_KMALLOC_H

#include "../types.h"

#ifdef __cplusplus
extern "C" {
#endif

// Zero size, an unsupported size or allocation failure returns null.
void* kmalloc(u32 size);
void* kzalloc(u32 size);
// Null is a no-op. Any other pointer must be a live heap allocation base.
void kfree(void* pointer);

#ifdef __cplusplus
}

namespace mm {
void kmalloc_selftest();
}
#endif

#endif
