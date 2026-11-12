#ifndef SINO_MM_E820_H
#define SINO_MM_E820_H
#include "addr.h"
#include "layout.h"
#ifdef __cplusplus
extern "C" {
#endif
typedef struct PACKED {
  u32 base, base_hi, length, length_hi, type, attributes;
} E820Entry;
typedef struct { u32 start_pfn, end_pfn, type; } BootRange;
/* Usable records admit only complete pages; any reserved byte excludes its page.
 * End PFNs may equal 0x100000 (4 GiB), unlike 32-bit byte addresses. */
static inline int e820_range(const E820Entry* e, BootRange* out) {
  if (!(e->attributes & 1) || e->base_hi || (!e->length_hi && !e->length))
    return 0;
  unsigned long long top = (unsigned long long)PHYS_LIMIT_PAGES << PAGE_SHIFT;
  unsigned long long base = e->base;
  unsigned long long length = ((unsigned long long)e->length_hi << 32) | e->length;
  unsigned long long end = length >= top - base ? top : base + length;
  u32 start_pfn, end_pfn;
  if (e->type == 1) {
    start_pfn = (u32)((base + PAGE_OFFSET_MASK) >> PAGE_SHIFT);
    end_pfn = (u32)(end >> PAGE_SHIFT);
  } else {
    start_pfn = (u32)(base >> PAGE_SHIFT);
    end_pfn = (u32)((end + PAGE_OFFSET_MASK) >> PAGE_SHIFT);
  }
  if (start_pfn >= end_pfn) return 0;
  *out = (BootRange){start_pfn, end_pfn, e->type};
  return 1;
}
static inline int e820_page_usable(const BootRange* ranges, u32 count, u32 pfn) {
  int usable = 0;
  for (u32 i = 0; i < count; i++) {
    const BootRange* r = &ranges[i];
    if (pfn < r->start_pfn || pfn >= r->end_pfn) continue;
    if (r->type != 1) return 0;
    usable = 1;
  }
  return usable;
}
#ifdef __cplusplus
}
#endif
#endif
