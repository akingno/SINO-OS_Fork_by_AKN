#ifndef SINO_MM_PAGE_H
#define SINO_MM_PAGE_H
#include "../types.h"
#include "layout.h"
#ifdef __cplusplus
extern "C" {
#endif

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

#ifdef __cplusplus
}

namespace mm {
enum class PageState : u8 { Free, Used, Reserved, Kernel, Guarded, Heap };
inline constexpr u32 MaxPageRun = 128;
inline constexpr u32 SlabCacheCount = 8;

struct PageFrame {
  static constexpr u32 PfnMask = PHYS_LIMIT_PAGES - 1;

  PageState state() const { return static_cast<PageState>((lo >> 20) & 7u); }
  u32 order() const { return (lo >> 23) & 15u; }
  bool isHead() const { return (lo & HeadBit) != 0; }
  bool isSlab() const { return (lo & SlabBit) != 0; }
  u32 slabCache() const { return (hi >> 20) & (SlabCacheCount - 1); }
  u32 heapPages() const { return (hi >> HeapCountShift) & HeapCountMask; }
  bool isFreeHead(u32 expected) const {
    return isHead() && state() == PageState::Free && order() == expected;
  }

  u32 prev() const { return lo & PfnMask; }
  u32 next() const { return hi & PfnMask; }
  void setPrev(u32 pfn) { lo = (lo & ~PfnMask) | pfn; }
  void setNext(u32 pfn) { hi = (hi & ~PfnMask) | pfn; }
  void reset(PageState state) {
    lo = static_cast<u32>(state) << 20;
    hi = 0;
  }
  void setFreeHead(u32 order) { lo = HeadBit | (order << 23); }
  void setHeapHead(u32 pages) {
    lo |= HeadBit;
    hi = (hi & ~(HeapCountMask << HeapCountShift)) | (pages << HeapCountShift);
  }
  void setSlab(u32 cache) {
    lo |= SlabBit;
    hi = (hi & PfnMask) | (cache << 20);
  }

private:
  // lo: prev[19:0], state[22:20], order[26:23], head[27], slab[28].
  // hi: next[19:0], slab cache index[22:20], heap head page count[30:23].
  static constexpr u32 HeadBit = 1u << 27;
  static constexpr u32 SlabBit = 1u << 28;
  static constexpr u32 HeapCountShift = 23;
  static constexpr u32 HeapCountMask = 0xff;
  static_assert(MaxPageRun <= HeapCountMask, "heap page count must fit in the frame");
  u32 lo, hi;
};

static_assert(sizeof(PageFrame) == 8 && PageFrame::PfnMask == 0xfffffu,
              "page metadata must fit the 20-bit PFN layout and boot budget");

// Slab-owned pages cannot be freed or guarded via the regular page API.
void* slab_page_alloc(u32 cache);
int slab_page_cache(const void* address);  // -1 unless contained in a live slab.
bool slab_page_free(void* base, u32 cache);

// Heap allocations own a contiguous run. Only its original base frees it.
void* heap_page_alloc(u32 pages);
bool heap_page_free(void* base);
}

enum class PageWatermark : u8 { Normal, Low, Critical };
PageWatermark page_watermark();
#endif
#endif
