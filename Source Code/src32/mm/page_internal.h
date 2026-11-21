#ifndef SINO_MM_PAGE_INTERNAL_H
#define SINO_MM_PAGE_INTERNAL_H

#include "page.h"

namespace mm {
struct BuddyZone {
  static constexpr u32 MaxOrder = 10;
  static constexpr u32 NoPfn = 0;

  PageFrame* frames;  // Metadata for start, not for PFN 0.
  u32 start, end;
  u32 freeCount;
  u32 heads[MaxOrder + 1];

  bool contains(u32 pfn, u32 count) const {
    return pfn >= start && pfn < end && count <= end - pfn;
  }
  PageFrame& frame(u32 pfn) const { return frames[pfn - start]; }

  void addFreeRange(u32 first, u32 last);
  u32 allocate(u32 count, PageState state);
  u32 allocateHeap(u32 pages);
  u32 releaseHeap(u32 first);
  bool release(u32 first, u32 count, PageState state);
  u32 check() const;

private:
  void link(u32 pfn, u32 order);
  void unlink(u32 pfn, u32 order);
  void merge(u32 pfn, u32 order);
  void addBlocks(u32 first, u32 last);
  void markAllocated(u32 first, u32 count, PageState state);
  u32 takeAligned(u32 order);
  u32 takeExact(u32 count);
  u32 findRun(u32 count) const;
  void takeRun(u32 first, u32 last);
};
}
#endif
