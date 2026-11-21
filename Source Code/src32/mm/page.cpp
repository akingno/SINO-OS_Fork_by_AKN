#include "page_internal.h"
#include "addr.h"
#include "e820.h"
#include "kmap.h"
#include "paging.h"
#include "test_fault.h"
#include "../klog.h"
#include "../lib/kstring.h"
#include "../panic.h"

namespace mm {
void BuddyZone::link(u32 pfn, u32 order) {
  PageFrame& head = frame(pfn);
  head.setFreeHead(order);
  head.setNext(heads[order]);
  if (heads[order]) {
    frame(heads[order]).setPrev(pfn);
  }
  heads[order] = pfn;
}

void BuddyZone::unlink(u32 pfn, u32 order) {
  PageFrame& head = frame(pfn);
  const u32 prev = head.prev(), next = head.next();
  if (prev) {
    frame(prev).setNext(next);
  } else {
    heads[order] = next;
  }
  if (next) {
    frame(next).setPrev(prev);
  }
  head.reset(PageState::Free);
}

void BuddyZone::merge(u32 pfn, u32 order) {
  while (order < MaxOrder) {
    const u32 peer = pfn ^ (1u << order);
    if (!contains(peer, 1u << order) || !frame(peer).isFreeHead(order)) {
      break;
    }
    unlink(peer, order);
    pfn = pfn < peer ? pfn : peer;
    ++order;
  }
  link(pfn, order);
}

// Cover an unlisted free interval with the largest aligned blocks that fit.
void BuddyZone::addBlocks(u32 first, u32 last) {
  while (first < last) {
    u32 order = 0;
    while (order < MaxOrder) {
      const u32 nextSize = 2u << order;
      if ((first & (nextSize - 1)) || nextSize > last - first) {
        break;
      }
      ++order;
    }
    merge(first, order);
    first += 1u << order;
  }
}

void BuddyZone::addFreeRange(u32 first, u32 last) {
  for (u32 pfn = first; pfn < last; ++pfn) {
    frame(pfn).reset(PageState::Free);
  }
  addBlocks(first, last);
  freeCount += last - first;
}

u32 BuddyZone::takeAligned(u32 order) {
  for (u32 found = order; found <= MaxOrder; ++found) {
    const u32 pfn = heads[found];
    if (!pfn) {
      continue;
    }
    unlink(pfn, found);
    while (found > order) {
      --found;
      link(pfn + (1u << found), found);
    }
    return pfn;
  }
  return NoPfn;
}

u32 BuddyZone::findRun(u32 count) const {
  u32 run = 0;
  for (u32 pfn = start; pfn < end; ++pfn) {
    if (frame(pfn).state() != PageState::Free) {
      run = 0;
    } else if (++run == count) {
      return pfn + 1 - count;
    }
  }
  return NoPfn;
}

// findRun starts at the first page of a free span, which is a block head.
// Consume successive free blocks and return any tail of the last one.
void BuddyZone::takeRun(u32 first, u32 last) {
  while (first < last) {
    const PageFrame& head = frame(first);
    const u32 order = head.order();
    if (order > MaxOrder || !head.isFreeHead(order)) {
      panic("PAGE free interval without buddy head", first, last);
    }
    unlink(first, order);
    first += 1u << order;
  }
  addBlocks(last, first);
}

u32 BuddyZone::takeExact(u32 count) {
  if (count == 1) {
    return NoPfn;
  }
  const u32 first = findRun(count);
  if (first) {
    takeRun(first, first + count);
  }
  return first;
}

static u32 order_for_count(u32 count) {
  u32 order = 0;
  while ((1u << order) < count) {
    ++order;
  }
  return order;
}

void BuddyZone::markAllocated(u32 first, u32 count, PageState state) {
  for (u32 pfn = first; pfn < first + count; ++pfn) {
    frame(pfn).reset(state);
  }
  freeCount -= count;
}

u32 BuddyZone::allocate(u32 count, PageState state) {
  if (!count || count > freeCount || count > (1u << MaxOrder)) {
    return NoPfn;
  }
  const u32 order = order_for_count(count);
  u32 first = takeAligned(order);
  if (first) {
    addBlocks(first + count, first + (1u << order));
  } else {
    first = takeExact(count);
  }
  if (!first) {
    return NoPfn;
  }

  markAllocated(first, count, state);
  return first;
}

// Keep the whole aligned block on the fast path. Under fragmentation, take
// only the requested pages and record their actual count for kfree.
u32 BuddyZone::allocateHeap(u32 pages) {
  if (!pages || pages > MaxPageRun || pages > freeCount) {
    return NoPfn;
  }
  const u32 order = order_for_count(pages);
  u32 first = takeAligned(order);
  if (first) {
    pages = 1u << order;
  } else {
    first = takeExact(pages);
  }
  if (first) {
    markAllocated(first, pages, PageState::Heap);
    frame(first).setHeapHead(pages);
  }
  return first;
}

u32 BuddyZone::releaseHeap(u32 first) {
  if (!contains(first, 1)) {
    return 0;
  }
  const PageFrame& head = frame(first);
  if (head.state() != PageState::Heap || !head.isHead()) {
    return 0;
  }
  const u32 pages = head.heapPages();
  return pages <= MaxPageRun && release(first, pages, PageState::Heap) ? pages : 0;
}

bool BuddyZone::release(u32 first, u32 count, PageState state) {
  if (!count || !contains(first, count)) {
    return false;
  }
  const u32 last = first + count;
  for (u32 pfn = first; pfn < last; ++pfn) {
    const PageFrame& page = frame(pfn);
    if (page.state() != state || page.isSlab() || (pfn != first && page.isHead())) {
      return false;
    }
  }
  addFreeRange(first, last);
  return true;
}

// Count free blocks from the frame map before walking the lists. The two
// views must agree on shape, membership and counts, including free interiors.
u32 BuddyZone::check() const {
  u32 blocks[MaxOrder + 1]{}, free = 0, published = 0;
  for (u32 pfn = start; pfn < end;) {
    const PageFrame& head = frame(pfn);
    const PageState state = head.state();
    if (state != PageState::Free && state != PageState::Heap) {
      if (head.isHead()) {
        panic("PAGE allocated buddy head", pfn, head.order());
      }
      if (state != PageState::Reserved) {
        ++published;
      }
      ++pfn;
      continue;
    }
    const u32 order = head.order();
    const bool isHeap = state == PageState::Heap;
    const u32 count = isHeap ? head.heapPages() : 1u << order;
    if (!head.isHead() || head.isSlab() || !count || !contains(pfn, count) ||
        (isHeap ? count > MaxPageRun : order > MaxOrder || (pfn & (count - 1)))) {
      panic("PAGE invalid free block or heap run", pfn, count);
    }
    for (u32 offset = 1; offset < count; ++offset) {
      const PageFrame& tail = frame(pfn + offset);
      if (tail.state() != state || tail.isHead() || tail.isSlab()) {
        panic("PAGE invalid buddy interior", pfn, offset);
      }
    }
    if (state == PageState::Free) {
      const u32 peer = pfn ^ count;
      if (order < MaxOrder && contains(peer, count) && frame(peer).isFreeHead(order)) {
        panic("PAGE unmerged buddy blocks", pfn, peer);
      }
      ++blocks[order];
      free += count;
    }
    published += count;
    pfn += count;
  }

  for (u32 order = 0; order <= MaxOrder; ++order) {
    u32 prev = NoPfn;
    for (u32 pfn = heads[order]; pfn; pfn = frame(pfn).next()) {
      if (!contains(pfn, 1u << order) || !frame(pfn).isFreeHead(order) ||
          frame(pfn).prev() != prev || !blocks[order]) {
        panic("PAGE invalid buddy list", pfn, order);
      }
      --blocks[order];
      prev = pfn;
    }
    if (blocks[order]) {
      panic("PAGE incomplete buddy list", order, blocks[order]);
    }
  }
  if (free != freeCount) {
    panic("PAGE buddy zone count", free, freeCount);
  }
  return published;
}
}

u32 free_pages;
u32 nr_pages;

namespace {
using mm::BuddyZone;
using mm::MaxPageRun;
using mm::PageFrame;
using mm::PageState;

constexpr u32 E820Max = 32;
constexpr u32 LowStart = BOOT_POOL_START / PAGE_SIZE;
constexpr u32 BootEnd = BOOT_IDENTITY_BYTES / PAGE_SIZE;
static_assert(LowStart > BuddyZone::NoPfn, "PFN zero is not a managed page");
static_assert(MaxPageRun <= (1u << BuddyZone::MaxOrder) &&
              !(MaxPageRun & (MaxPageRun - 1)), "heap limit must be a buddy order");

BootRange ranges[E820Max];
u32 range_count;
PageFrame* page_frames;
u32 metadata_pfn, metadata_pages, published_pages;
BuddyZone low, high;
bool logged_extended_allocation, logged_high_allocation;

struct Watermarks {
  u32 criticalBelow, normalAbove;

  PageWatermark classify(u32 available) const {
    if (available < criticalBelow) { return PageWatermark::Critical; }
    return available <= normalAbove ? PageWatermark::Low : PageWatermark::Normal;
  }
};
Watermarks watermarks;
bool watermarks_ready;
#ifdef SINO_SHELL_BRIDGE
Watermarks test_watermarks;
bool test_watermarks_active;
#endif

void read_boot_memory_map() {
  const u32 count = *static_cast<volatile u16*>(phys_to_virt(E820_COUNT_ADDRESS));
  const auto* map = static_cast<const E820Entry*>(phys_to_virt(E820_TABLE_ADDRESS));
  if (!count || count > E820Max) {
    panic("Invalid E820 memory map", count, 0);
  }
  for (u32 i = 0; i < count; ++i) {
    BootRange range;
    if (e820_range(&map[i], &range)) {
      ranges[range_count++] = range;
      if (range.type == 1 && range.end_pfn > nr_pages) {
        nr_pages = range.end_pfn;
      }
    }
  }
}

/* The metadata must fit in one usable span inside the bootstrap mapping. */
u32 find_boot_metadata(u32 boot_end) {
  u32 run = 0;
  for (u32 pfn = LowStart; pfn < boot_end; ++pfn) {
    if (!e820_page_usable(ranges, range_count, pfn)) {
      run = 0;
    } else if (++run == metadata_pages) {
      return pfn + 1 - run;
    }
  }
  panic("page_init: No boot space for page metadata", nr_pages, metadata_pages);
  return 0;
}

bool is_pool_page(u32 pfn) {
  return (pfn < metadata_pfn || pfn >= metadata_pfn + metadata_pages) &&
         e820_page_usable(ranges, range_count, pfn);
}

void publish_pages(BuddyZone& zone, u32 first) {
  const u32 before = zone.freeCount;
  while (first < zone.end) {
    if (!is_pool_page(first)) {
      ++first;
      continue;
    }
    u32 last = first + 1;
    while (last < zone.end && is_pool_page(last)) {
      ++last;
    }
    zone.addFreeRange(first, last);
    first = last;
  }
  const u32 added = zone.freeCount - before;
  free_pages += added;
  published_pages += added;
}

void zero_run(phys_t page, u32 count) {
  if (!direct_map_end || page / PAGE_SIZE + count <= direct_map_end / PAGE_SIZE) {
    memset(phys_to_virt(page), 0, count * PAGE_SIZE);
  } else {
    for (u32 i = 0; i < count; ++i) {
      void* mapped = kmap(page + i * PAGE_SIZE);
      memset(mapped, 0, PAGE_SIZE);
      kunmap(mapped);
    }
  }
}

bool free_run(phys_t page, u32 count, PageState state) {
  if (!count || count > MaxPageRun || !page_aligned(page)) {
    return false;
  }
  const u32 pfn = page / PAGE_SIZE;
  BuddyZone& zone = pfn < low.end ? low : high;
  if (!zone.release(pfn, count, state)) {
    return false;
  }
  free_pages += count;
  return true;
}
}

PageWatermark page_watermark() {
#ifdef SINO_SHELL_BRIDGE
  if (test_watermarks_active) { return test_watermarks.classify(free_pages); }
#endif
  return watermarks_ready ? watermarks.classify(free_pages) : PageWatermark::Normal;
}

#ifdef SINO_SHELL_BRIDGE
bool mmtest::overrideWatermarks(u32 criticalBelow, u32 normalAbove) {
  if (normalAbove > PHYS_LIMIT_PAGES || criticalBelow > normalAbove) {
    return false;
  }
  test_watermarks = {criticalBelow, normalAbove};
  test_watermarks_active = true;
  return true;
}
void mmtest::restoreWatermarks() { test_watermarks_active = false; }
#endif

int page_boot_usable(phys_t address) {
  return e820_page_usable(ranges, range_count, address / PAGE_SIZE);
}

void page_init(void) {
  read_boot_memory_map();
  metadata_pages = (nr_pages * sizeof(PageFrame) + PAGE_SIZE - 1) / PAGE_SIZE;
  const u32 boot_end = nr_pages < BootEnd ? nr_pages : BootEnd;
  metadata_pfn = find_boot_metadata(boot_end);
  page_frames = static_cast<PageFrame*>(phys_to_virt(metadata_pfn * PAGE_SIZE));
  for (u32 pfn = 0; pfn < nr_pages; ++pfn) {
    page_frames[pfn].reset(PageState::Reserved);
  }

  low = {page_frames + LowStart, LowStart, boot_end, 0, {}};
  publish_pages(low, LowStart);
  const u32 paging_pages = paging_bootstrap_page_budget(nr_pages);
  if (low.freeCount < paging_pages) {
    panic("page_init: Not enough boot pages for permanent paging", low.freeCount, paging_pages);
  }

  pr_debug("PAGE metadata base=");
  pr_debug_hex(metadata_pfn * PAGE_SIZE);
  pr_debug(" pages=");
  pr_debug_hex(metadata_pages);
  pr_debug(" nr_pages=");
  pr_debug_hex(nr_pages);
  pr_debug("\n");
}

void page_publish_extended(void) {
  const u32 before = free_pages, boot_end = low.end;
  const u32 direct_end = direct_map_end / PAGE_SIZE;
  low.end = nr_pages < direct_end ? nr_pages : direct_end;
  high = {page_frames + low.end, low.end, nr_pages, 0, {}};
  publish_pages(low, boot_end);
  publish_pages(high, high.start);
  pr_debug("PAGE extended pages=");
  pr_debug_hex(free_pages - before);
  pr_debug("\n");

  // Non-PAE 4 GiB bounds published_pages to 1 Mi pages, so these products fit u32.
  watermarks = {(published_pages * 5 + 99) / 100, published_pages * 15 / 100};
  watermarks_ready = true;
  pr_debug("PAGE watermarks total=");
  pr_debug_hex(published_pages);
  pr_debug(" criticalBelow=");
  pr_debug_hex(watermarks.criticalBelow);
  pr_debug(" normalAbove=");
  pr_debug_hex(watermarks.normalAbove);
  pr_debug("\n");
}

void* kpage_alloc_run(u32 count) {
  if (count > MaxPageRun) {
    return nullptr;
  }
  const u32 pfn = low.allocate(count, PageState::Kernel);
  if (!pfn) {
    return nullptr;
  }
  free_pages -= count;
  const phys_t page = pfn * PAGE_SIZE;
  zero_run(page, count);
  if (page >= BOOT_IDENTITY_BYTES && !logged_extended_allocation) {
    logged_extended_allocation = true;
    pr_debug("PAGE first extended allocation=");
    pr_debug_hex(page);
    pr_debug("\n");
  }
  return phys_to_virt(page);
}

void* kpage_alloc(void) {
  return kpage_alloc_run(1);
}

int kpage_free_run(void* page, u32 count) {
  const phys_t address = virt_to_phys(page);
  if (address / PAGE_SIZE >= low.end) {
    return 0;
  }
  return free_run(address, count, PageState::Kernel);
}

int kpage_free(void* page) {
  return kpage_free_run(page, 1);
}

void* mm::slab_page_alloc(u32 cache) {
  if (cache >= SlabCacheCount) {
    return nullptr;
  }
  void* page = kpage_alloc();
  if (page) {
    page_frames[virt_to_phys(page) / PAGE_SIZE].setSlab(cache);
  }
  return page;
}

int mm::slab_page_cache(const void* address) {
  const u32 pfn = virt_to_phys(address) / PAGE_SIZE;
  if (!low.contains(pfn, 1) || !page_frames[pfn].isSlab()) {
    return -1;
  }
  return static_cast<int>(page_frames[pfn].slabCache());
}

bool mm::slab_page_free(void* base, u32 cache) {
  const phys_t address = virt_to_phys(base);
  if (!page_aligned(address) || cache >= SlabCacheCount ||
      slab_page_cache(base) != static_cast<int>(cache)) {
    return false;
  }
  const u32 pfn = address / PAGE_SIZE;
  low.addFreeRange(pfn, pfn + 1);
  ++free_pages;
  return true;
}

void* mm::heap_page_alloc(u32 pages) {
  const u32 pfn = low.allocateHeap(pages);
  if (!pfn) {
    return nullptr;
  }
  free_pages -= low.frame(pfn).heapPages();
  return phys_to_virt(pfn * PAGE_SIZE);
}

bool mm::heap_page_free(void* base) {
  const phys_t address = virt_to_phys(base);
  if (!page_aligned(address)) {
    return false;
  }
  const u32 pages = low.releaseHeap(address / PAGE_SIZE);
  free_pages += pages;
  return pages != 0;
}

/* Guard transitions preserve ownership. Paging restores the PTE before
 * changing Guarded back to Kernel so the owner can free the original run. */
int kpage_guard_claim(void* page) {
  const phys_t address = virt_to_phys(page);
  const u32 pfn = address / PAGE_SIZE;
  if (!page_aligned(address) || !direct_map_end || !low.contains(pfn, 1) ||
      page_frames[pfn].state() != PageState::Kernel || page_frames[pfn].isSlab()) {
    return 0;
  }
  page_frames[pfn].reset(PageState::Guarded);
  return 1;
}

void kpage_guard_release(void* page) {
  const phys_t address = virt_to_phys(page);
  const u32 pfn = address / PAGE_SIZE;
  if (!page_aligned(address) || !low.contains(pfn, 1) ||
      page_frames[pfn].state() != PageState::Guarded) {
    panic("Invalid guarded page release", address, pfn);
  }
  page_frames[pfn].reset(PageState::Kernel);
}

phys_t page_alloc(void) {
  u32 pfn = high.allocate(1, PageState::Used);
  if (!pfn) {
    pfn = low.allocate(1, PageState::Used);
  }
  if (!pfn) {
    return PHYS_NULL;
  }
  --free_pages;
  const phys_t page = pfn * PAGE_SIZE;
  zero_run(page, 1);
  if (direct_map_end && page >= direct_map_end && !logged_high_allocation) {
    logged_high_allocation = true;
    pr_debug("PAGE first high allocation=");
    pr_debug_hex(page);
    pr_debug("\n");
  }
  return page;
}

int page_free(phys_t page) {
  return free_run(page, 1, PageState::Used);
}

namespace {
void page_frame_test() {
  PageFrame frame;
  frame.reset(PageState::Guarded);
  frame.setPrev(PageFrame::PfnMask);
  frame.setNext(0xabcdeu);
  if (frame.state() != PageState::Guarded || frame.isHead() ||
      frame.prev() != PageFrame::PfnMask || frame.next() != 0xabcdeu) {
    panic("PAGE frame state and links", frame.prev(), frame.next());
  }
  frame.reset(PageState::Free);
  frame.setFreeHead(BuddyZone::MaxOrder);
  frame.setPrev(0x54321u);
  frame.setNext(PageFrame::PfnMask);
  if (!frame.isFreeHead(BuddyZone::MaxOrder) || frame.prev() != 0x54321u ||
      frame.next() != PageFrame::PfnMask) {
    panic("PAGE frame order and links", frame.prev(), frame.next());
  }
  frame.reset(PageState::Heap);
  frame.setHeapHead(MaxPageRun);
  if (frame.state() != PageState::Heap || !frame.isHead() ||
      frame.heapPages() != MaxPageRun || frame.next()) {
    panic("PAGE frame heap count", frame.heapPages(), frame.next());
  }
  frame.reset(PageState::Used);
  if (frame.state() != PageState::Used || frame.isHead() || frame.order() ||
      frame.heapPages() ||
      frame.prev() != BuddyZone::NoPfn || frame.next() != BuddyZone::NoPfn) {
    panic("PAGE frame reset", frame.prev(), frame.next());
  }
}

void buddy_run_test() {
  constexpr u32 base = 16, count = 16;
  PageFrame frames[count];
  BuddyZone zone{frames, base, base + count, 0, {}};
  zone.addFreeRange(base, base + count);

  if (zone.allocateHeap(3) != base || zone.frame(base).heapPages() != 4 ||
      zone.freeCount != count - 4 ||
      zone.check() != count || zone.release(base, 4, PageState::Kernel) ||
      zone.releaseHeap(base + 1) || zone.releaseHeap(base) != 4 ||
      !zone.frame(base).isFreeHead(4)) {
    panic("PAGE heap block ownership", zone.freeCount, zone.heads[4]);
  }
  if (zone.allocate(9, PageState::Kernel) != base || zone.freeCount != 7 ||
      zone.check() != count || !zone.release(base, 9, PageState::Kernel) ||
      !zone.frame(base).isFreeHead(4) || zone.freeCount != count) {
    panic("PAGE exact 9-page buddy run", zone.freeCount, zone.heads[4]);
  }

  // Isolate the free span so an aligned block elsewhere cannot mask fallback.
  if (zone.allocate(count, PageState::Kernel) != base ||
      !zone.release(base + 1, 2, PageState::Kernel) || zone.heads[1] ||
      zone.allocateHeap(2) != base + 1 || zone.freeCount ||
      zone.releaseHeap(base + 1) != 2 ||
      zone.allocate(2, PageState::Kernel) != base + 1 || zone.freeCount) {
    panic("PAGE unaligned 2-page buddy run", zone.freeCount, zone.heads[1]);
  }
  if (!zone.release(base + 1, 3, PageState::Kernel) ||
      !zone.release(base + 5, 1, PageState::Kernel) ||
      zone.allocateHeap(4) || zone.freeCount != 4 ||
      zone.allocateHeap(3) != base + 1 || zone.freeCount != 1 ||
      zone.frame(base + 1).heapPages() != 3 || zone.check() != count ||
      zone.releaseHeap(base + 2) || zone.releaseHeap(base) ||
      zone.release(base + 1, 3, PageState::Kernel) ||
      zone.releaseHeap(base + 1) != 3 || zone.releaseHeap(base + 1) ||
      zone.frame(base + 4).state() != PageState::Kernel || zone.freeCount != 4 ||
      zone.allocate(3, PageState::Kernel) != base + 1 ||
      zone.allocate(1, PageState::Kernel) != base + 5) {
    panic("PAGE exact heap run and release", zone.freeCount, zone.heads[1]);
  }
  if (!zone.release(base + 1, 5, PageState::Kernel) ||
      zone.allocate(4, PageState::Kernel) != base + 1 ||
      zone.freeCount != 1 || !zone.frame(base + 5).isFreeHead(0) ||
      zone.check() != count || zone.allocate(2, PageState::Kernel) ||
      !zone.release(base + 1, 4, PageState::Kernel) ||
      !zone.release(base, 1, PageState::Kernel) ||
      !zone.release(base + 6, 10, PageState::Kernel) ||
      !zone.frame(base).isFreeHead(4) || zone.freeCount != count) {
    panic("PAGE cross-block buddy run", zone.freeCount, zone.heads[4]);
  }

  // Merge with a buddy in the middle of a list, exercising both link updates.
  if (zone.allocate(count, PageState::Kernel) != base ||
      !zone.release(base, 1, PageState::Kernel) ||
      !zone.release(base + 2, 1, PageState::Kernel) ||
      !zone.release(base + 4, 1, PageState::Kernel) ||
      !zone.release(base + 3, 1, PageState::Kernel) ||
      zone.frame(base + 4).next() != base ||
      zone.frame(base).prev() != base + 4 ||
      !zone.frame(base + 2).isFreeHead(1) ||
      !zone.release(base + 1, 1, PageState::Kernel) ||
      !zone.release(base + 5, 11, PageState::Kernel) ||
      !zone.frame(base).isFreeHead(4) || zone.freeCount != count ||
      zone.check() != count) {
    panic("PAGE buddy unlink and merge", zone.freeCount, zone.heads[4]);
  }
}

void buddy_boundary_test() {
  constexpr u32 base = 16;
  PageFrame frames[4];
  for (PageFrame& frame : frames) {
    frame.reset(PageState::Reserved);
  }
  BuddyZone lower{frames, base, base + 1, 0, {}};
  BuddyZone upper{frames + 1, base + 1, base + 4, 0, {}};
  lower.addFreeRange(base, base + 1);
  upper.addFreeRange(base + 1, base + 2);
  upper.addFreeRange(base + 3, base + 4);

  if (!lower.frame(base).isFreeHead(0) || !upper.frame(base + 1).isFreeHead(0) ||
      upper.frame(base + 2).state() != PageState::Reserved ||
      lower.allocate(2, PageState::Kernel) || upper.allocate(2, PageState::Kernel) ||
      lower.release(base, 2, PageState::Free) || lower.check() != 1 ||
      upper.check() != 2) {
    panic("PAGE buddy zone and hole isolation", lower.freeCount, upper.freeCount);
  }
}
}

void page_allocator_test(void) {
  const u32 before = free_pages;
  const PageWatermark before_watermark = page_watermark();
  for (u32 offset = 0; offset < metadata_pages; ++offset) {
    if (page_frames[metadata_pfn + offset].state() != PageState::Reserved) {
      panic("PAGE metadata reservation", metadata_pfn, offset);
    }
  }
  if (kpage_free(page_frames) || page_free(0x200000u) || kpage_alloc_run(0) ||
      kpage_alloc_run(MaxPageRun + 1) || free_pages != before) {
    panic("PAGE reserved page and invalid run validation", free_pages, before);
  }

  auto* a = static_cast<u32*>(kpage_alloc());
  auto* b = static_cast<u32*>(kpage_alloc());
  if (!a || !b || a == b || !page_aligned(virt_to_phys(a)) ||
      !page_aligned(virt_to_phys(b)) || free_pages + 2 != before) {
    panic("PAGE one-page kernel allocation", virt_to_phys(a), virt_to_phys(b));
  }
  *a = 0x46454c49;
  *b = 0x58333221;
  if (*a != 0x46454c49 || *b != 0x58333221 || page_free(virt_to_phys(a)) ||
      !kpage_free(a) || !kpage_free(b) || kpage_free(a) || free_pages != before) {
    panic("PAGE kernel page validation", free_pages, before);
  }

  const phys_t user = page_alloc();
  if (!user || !page_aligned(user) || user < BOOT_POOL_START ||
      (high.freeCount && user < direct_map_end) || free_pages + 1 != before ||
      kpage_free(reinterpret_cast<void*>(user)) || !page_free(user) ||
      page_free(user) || free_pages != before) {
    panic("PAGE user page validation", user, free_pages);
  }

  auto* run = static_cast<u8*>(kpage_alloc_run(2));
  if (!run || !page_aligned(virt_to_phys(run)) || free_pages + 2 != before) {
    panic("PAGE two-page allocation", virt_to_phys(run), free_pages);
  }
  for (u32 i = 0; i < 2 * PAGE_SIZE; ++i) {
    if (run[i]) {
      panic("PAGE run zeroing", virt_to_phys(run), i);
    }
  }
  run[PAGE_SIZE] = 1;
  if (!kpage_free_run(run, 2) || kpage_free_run(run, 2)) {
    panic("PAGE two-page release", virt_to_phys(run), free_pages);
  }

  page_frame_test();
  buddy_run_test();
  buddy_boundary_test();
  const u32 checked = low.check() + high.check();
  if (checked != published_pages || low.freeCount + high.freeCount != free_pages ||
      free_pages != before || page_watermark() != before_watermark) {
    panic("PAGE allocator totals", checked, free_pages);
  }
  pr_debug("PAGE allocator self-check passed\n");
}
