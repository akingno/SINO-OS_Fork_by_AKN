#include "slab.h"
#include "addr.h"
#include "page.h"
#include "paging.h"
#include "../api.h"
#include "../klog.h"
#include "../panic.h"

namespace mm {
struct SlabCache {
  u16 objectSize, slots;
  u32 partial, full;
  u32 totalSlabs, totalAllocated;
};

namespace {
constexpr u32 MinSize = 8;
constexpr u32 MaxSize = MinSize << (SlabCacheCount - 1);
constexpr u16 NoSlot = 0xffffu;

struct SlabHeader {
  u32 next, prev;
  u16 freeCount, firstFree;
};
static_assert(sizeof(SlabHeader) == 12, "slab page footer layout");

SlabCache caches[SlabCacheCount];

u8* page_base(u32 pfn) {
  return static_cast<u8*>(phys_to_virt(pfn * PAGE_SIZE));
}

SlabHeader& slab_header(u8* base) {
  return *reinterpret_cast<SlabHeader*>(base + PAGE_SIZE - sizeof(SlabHeader));
}

u8& allocation_byte(u8* base, u32 slots, u32 slot) {
  u8* bitmap = base + PAGE_SIZE - sizeof(SlabHeader) - (slots + 7) / 8;
  return bitmap[slot / 8];
}

void link_slab(u32& head, u32 pfn, SlabHeader& slab) {
  slab.prev = 0;
  slab.next = head;
  if (head) {
    slab_header(page_base(head)).prev = pfn;
  }
  head = pfn;
}

void unlink_slab(u32& head, const SlabHeader& slab) {
  if (slab.prev) {
    slab_header(page_base(slab.prev)).next = slab.next;
  } else {
    head = slab.next;
  }
  if (slab.next) {
    slab_header(page_base(slab.next)).prev = slab.prev;
  }
}

u16 next_slot(const u8* object) {
  return static_cast<u16>(object[0] | (static_cast<u16>(object[1]) << 8));
}

void set_next_slot(u8* object, u16 slot) {
  object[0] = static_cast<u8>(slot);
  object[1] = static_cast<u8>(slot >> 8);
}

bool grow(SlabCache& cache) {
  auto* base = static_cast<u8*>(slab_page_alloc(static_cast<u32>(&cache - caches)));
  if (!base) {
    return false;
  }
  // The page allocator already cleared the allocation bitmap.
  SlabHeader& slab = slab_header(base);
  slab.freeCount = cache.slots;
  slab.firstFree = 0;
  for (u16 slot = 0; slot < cache.slots; ++slot) {
    set_next_slot(base + slot * cache.objectSize, slot + 1 < cache.slots ? slot + 1 : NoSlot);
  }
  link_slab(cache.partial, virt_to_phys(base) / PAGE_SIZE, slab);
  ++cache.totalSlabs;
  return true;
}
}

SlabCache* slab_cache_create(u32 objectSize) {
  if (objectSize < MinSize || objectSize > MaxSize || (objectSize & (objectSize - 1))) {
    return nullptr;
  }
  u32 index = 0;
  for (u32 size = MinSize; size < objectSize; size <<= 1) {
    ++index;
  }
  SlabCache& cache = caches[index];
  if (!cache.objectSize) {
    cache.objectSize = static_cast<u16>(objectSize);
    // Largest N with N * objectSize + ceil(N / 8) bytes before the header.
    cache.slots = (PAGE_SIZE - sizeof(SlabHeader)) * 8 / (objectSize * 8 + 1);
  }
  return &cache;
}

SlabCache* slab_cache_for(const void* address) {
  const int index = slab_page_cache(address);
  return index < 0 ? nullptr : &caches[index];
}

void* slab_alloc(SlabCache& cache) {
  if (!cache.partial && !grow(cache)) {
    return nullptr;
  }

  const u32 pfn = cache.partial;
  u8* base = page_base(pfn);
  SlabHeader& slab = slab_header(base);
  const u16 slot = slab.firstFree;
  if (!slab.freeCount || slot >= cache.slots) {
    panic("SLAB invalid free slot", pfn, slot);
  }
  u8& allocated = allocation_byte(base, cache.slots, slot);
  const u8 bit = 1u << (slot % 8);
  if (allocated & bit) {
    panic("SLAB free slot is allocated", pfn, slot);
  }

  u8* object = base + slot * cache.objectSize;
  slab.firstFree = next_slot(object);
  allocated |= bit;
  --slab.freeCount;
  ++cache.totalAllocated;
  if (!slab.freeCount) {
    unlink_slab(cache.partial, slab);
    link_slab(cache.full, pfn, slab);
  }
  return object;
}

bool slab_free(SlabCache& cache, void* object) {
  if (slab_cache_for(object) != &cache) {
    return false;
  }
  const phys_t address = virt_to_phys(object);
  const u32 offset = page_offset(address);
  if (offset % cache.objectSize || offset >= cache.slots * static_cast<u32>(cache.objectSize)) {
    return false;
  }

  const u32 pfn = address / PAGE_SIZE, slot = offset / cache.objectSize;
  u8* base = page_base(pfn);
  SlabHeader& slab = slab_header(base);
  u8& allocated = allocation_byte(base, cache.slots, slot);
  const u8 bit = 1u << (slot % 8);
  if (!(allocated & bit)) {
    return false;
  }

  allocated &= ~bit;
  set_next_slot(static_cast<u8*>(object), slab.firstFree);
  slab.firstFree = static_cast<u16>(slot);
  ++slab.freeCount;
  --cache.totalAllocated;
  if (slab.freeCount == 1) {
    unlink_slab(cache.full, slab);
    link_slab(cache.partial, pfn, slab);
  }
  if (slab.freeCount == cache.slots) {
    unlink_slab(cache.partial, slab);
    --cache.totalSlabs;
    if (!slab_page_free(base, static_cast<u32>(&cache - caches))) {
      panic("SLAB backing page owner mismatch", pfn, cache.objectSize);
    }
  }
  return true;
}

namespace {
void check_size_classes() {
  constexpr u16 expected[SlabCacheCount] = {502, 253, 127, 63, 31, 15, 7, 3};
  if (slab_cache_create(0) || slab_cache_create(3) || slab_cache_create(12) ||
      slab_cache_create(MaxSize * 2)) {
    panic("SLAB unsupported cache size", 0, 0);
  }

  for (u32 index = 0; index < SlabCacheCount; ++index) {
    const u32 size = MinSize << index;
    SlabCache* cache = slab_cache_create(size);
    SlabCache* other = slab_cache_create(size == MinSize ? MinSize * 2 : MinSize);
    if (!cache || !other || slab_cache_create(size) != cache || cache->slots != expected[index]) {
      panic("SLAB cache capacity", size, expected[index]);
    }

    const u32 before = free_pages;
    auto* a = static_cast<u8*>(slab_alloc(*cache));
    auto* b = static_cast<u8*>(slab_alloc(*cache));
    if (!a || !b || !page_aligned(virt_to_phys(a)) || b != a + size ||
        slab_cache_for(b) != cache || free_pages + 1 != before) {
      panic("SLAB slot layout", size, free_pages);
    }
    KernelGuard guard{};
    if (kpage_free(a) || kpage_free_run(a, 1) || page_free(virt_to_phys(a)) ||
        paging_kernel_guard(a, &guard) != S_INVALID || guard.id || slab_free(*other, a) ||
        slab_free(*cache, nullptr) || slab_free(*cache, a + 1) ||
        slab_free(*cache, a + size * cache->slots) ||
        slab_free(*cache, b + size)) {
      panic("SLAB invalid slot or page release", size, virt_to_phys(a));
    }

    a[0] = 0x13;
    a[size - 1] = 0x34;
    b[0] = 0x57;
    b[size - 1] = 0x79;
    if (a[0] != 0x13 || a[size - 1] != 0x34 || !slab_free(*cache, a) ||
        slab_free(*cache, a) || slab_alloc(*cache) != a ||
        b[0] != 0x57 || b[size - 1] != 0x79 || !slab_free(*cache, a) ||
        !slab_free(*cache, b) || slab_cache_for(a) || slab_free(*cache, a) ||
        cache->totalAllocated || cache->totalSlabs || cache->partial || cache->full ||
        free_pages != before) {
      panic("SLAB isolation, slot reuse or page return", size, free_pages);
    }
  }
}

void check_slab_growth() {
  SlabCache& cache = *slab_cache_create(MaxSize);
  const u32 before = free_pages;
  void* objects[7];
  for (void*& object : objects) {
    object = slab_alloc(cache);
    if (!object) {
      panic("SLAB growth failed", cache.totalSlabs, free_pages);
    }
  }
  const u32 first = virt_to_phys(objects[0]) / PAGE_SIZE;
  const u32 second = virt_to_phys(objects[3]) / PAGE_SIZE;
  const u32 third = virt_to_phys(objects[6]) / PAGE_SIZE;
  if (first == second || first == third || second == third || cache.partial != third ||
      cache.full != second || slab_header(page_base(second)).next != first ||
      free_pages + 3 != before || cache.totalSlabs != 3 || cache.totalAllocated != 7 ||
      !slab_free(cache, objects[1]) || slab_header(page_base(second)).next ||
      cache.partial != first || slab_alloc(cache) != objects[1]) {
    panic("SLAB partial and full lists", cache.partial, cache.full);
  }
  for (void* object : objects) {
    if (!slab_free(cache, object)) {
      panic("SLAB growth release", virt_to_phys(object), free_pages);
    }
  }
  if (cache.full || cache.partial || cache.totalSlabs || cache.totalAllocated || free_pages != before) {
    panic("SLAB growth teardown", free_pages, before);
  }
}
}

void slab_selftest() {
  check_size_classes();
  check_slab_growth();
  pr_debug("SLAB allocator self-check passed\n");
}
}
