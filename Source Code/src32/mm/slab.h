#ifndef SINO_MM_SLAB_H
#define SINO_MM_SLAB_H

#include "../types.h"

namespace mm {
struct SlabCache;

// Return the shared cache for a power-of-two object size from 8 to 1024 bytes.
SlabCache* slab_cache_create(u32 objectSize);
// Return the cache owning this page, or null for addresses outside live slabs.
SlabCache* slab_cache_for(const void* address);
void* slab_alloc(SlabCache& cache);  // Uninitialized on success.
bool slab_free(SlabCache& cache, void* object);  // False for invalid objects.
void slab_selftest();
}

#endif
