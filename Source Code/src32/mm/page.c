#include "../kernel.h"
#include "../klog.h"
#include "page.h"

enum { PAGE_SIZE = 4096, MAX_RUN = 64, E820_MAX = 32 };
enum { PAGE_FREE, PAGE_USED, PAGE_RESERVED };

typedef struct PACKED {
  u32 base, base_hi, length, length_hi, type, attributes;
} E820Entry;

typedef struct {
  u32 start, end, type;
} Range;

static u8* mem_map;
static u32 nr_pages, metadata_pages;
u32 free_pages, usable_memory;

static int available(const Range* ranges, u32 count, u32 start) {
  u32 end = start + PAGE_SIZE;
  int usable = 0;
  for (u32 i = 0; i < count; i++) {
    const Range* r = &ranges[i];
    if (r->start >= end || r->end <= start)
      continue;
    if (r->type != 1)
      return 0;
    if (r->start <= start && r->end >= end)
      usable = 1;
  }
  return usable;
}

void page_init(void) {
  u32 count = *(volatile u16*)0x4f00;
  const E820Entry* map = (const E820Entry*)0x5000;
  if (!count || count > E820_MAX)
    panic("Invalid E820 memory map", count, 0);

  Range ranges[E820_MAX];
  u32 range_count = 0;
  for (u32 i = 0; i < count; i++) {
    const E820Entry* e = &map[i];
    if (!(e->attributes & 1) || e->base_hi || e->base >= BOOT_IDENTITY_BYTES ||
        (!e->length_hi && !e->length))
      continue;
    /* Clamp the length before addition so a large E820 entry cannot wrap. */
    u32 end = e->length_hi || e->length > BOOT_IDENTITY_BYTES - e->base
                  ? BOOT_IDENTITY_BYTES : e->base + e->length;
    ranges[range_count++] = (Range){e->base, end, e->type};
    if (e->type == 1 && end > usable_memory)
      usable_memory = end;
  }

  nr_pages = usable_memory / PAGE_SIZE;
  metadata_pages = (nr_pages + PAGE_SIZE - 1) / PAGE_SIZE;
  /* Lower memory contains the kernel, boot tables, RAM disk and graphics. */
  u32 run = 0, metadata_base = 0;
  for (u32 pfn = BOOT_POOL_START / PAGE_SIZE; pfn < nr_pages; pfn++) {
    if (!available(ranges, range_count, pfn * PAGE_SIZE)) {
      run = 0;
      continue;
    }
    if (++run == metadata_pages) {
      metadata_base = (pfn + 1 - run) * PAGE_SIZE;
      break;
    }
  }
  if (!metadata_base)
    panic("No boot space for page metadata", usable_memory, metadata_pages);

  mem_map = (u8*)metadata_base;
  memset(mem_map, PAGE_RESERVED, metadata_pages * PAGE_SIZE);
  u32 metadata_end = metadata_base + metadata_pages * PAGE_SIZE;
  for (u32 pfn = BOOT_POOL_START / PAGE_SIZE; pfn < nr_pages; pfn++) {
    u32 addr = pfn * PAGE_SIZE;
    if ((addr < metadata_base || addr >= metadata_end) &&
        available(ranges, range_count, addr)) {
      mem_map[pfn] = PAGE_FREE;
      free_pages++;
    }
  }
  if (!free_pages)
    panic("No allocatable physical pages", usable_memory, metadata_pages);
  pr_debug("PAGE metadata base=");
  pr_debug_hex(metadata_base);
  pr_debug(" pages=");
  pr_debug_hex(metadata_pages);
  pr_debug("\n");
}

void* page_alloc_run(u32 count) {
  if (!count || count > MAX_RUN)
    return 0;
  for (u32 i = BOOT_POOL_START / PAGE_SIZE; i + count <= nr_pages; i++) {
    u32 j = 0;
    while (j < count && mem_map[i + j] == PAGE_FREE)
      j++;
    if (j == count) {
      memset(mem_map + i, PAGE_USED, count);
      free_pages -= count;
      void* p = (void*)(i * PAGE_SIZE);
      memset(p, 0, count * PAGE_SIZE);
      return p;
    }
    i += j;
  }
  return 0;
}

void* page_alloc(void) { return page_alloc_run(1); }

int page_free_run(void* p, u32 count) {
  u32 addr = (u32)p;
  u32 pfn = addr / PAGE_SIZE;
  if (!count || count > MAX_RUN || addr % PAGE_SIZE || addr < BOOT_POOL_START ||
      pfn >= nr_pages || count > nr_pages - pfn)
    return 0;
  for (u32 i = 0; i < count; i++)
    if (mem_map[pfn + i] != PAGE_USED)
      return 0;
  memset(mem_map + pfn, PAGE_FREE, count);
  free_pages += count;
  return 1;
}

int page_free(void* p) { return page_free_run(p, 1); }

void page_allocator_test(void) {
  u32 before = free_pages;
  for (u32 i = 0; i < metadata_pages; i++)
    if (mem_map[(u32)mem_map / PAGE_SIZE + i] != PAGE_RESERVED)
      panic("Page metadata reservation", (u32)mem_map, i);
  if (page_free(mem_map) || page_free((void*)0x200000) || free_pages != before)
    panic("Reserved page free validation", free_pages, before);

  u32* a = page_alloc();
  u32* b = page_alloc();
  if (!a || !b || a == b || ((u32)a | (u32)b) % PAGE_SIZE)
    panic("Page allocation", (u32)a, (u32)b);
  *a = 0x46454c49;
  *b = 0x58333221;
  if (*a != 0x46454c49 || *b != 0x58333221)
    panic("Memory read/write", 0, 0);
  if (!page_free(a) || !page_free(b) || page_free(a) || free_pages != before)
    panic("Page free validation", free_pages, before);

  u8* block = page_alloc_run(2);
  if (!block || (u32)block % PAGE_SIZE || free_pages + 2 != before)
    panic("Page run allocation", (u32)block, free_pages);
  for (u32 i = 0; i < 2 * PAGE_SIZE; i++)
    if (block[i])
      panic("Page run zeroing", (u32)block, i);
  block[PAGE_SIZE] = 1;
  if (!page_free_run(block, 2) || page_free_run(block, 2) || free_pages != before)
    panic("Page run release", (u32)block, free_pages);
  pr_debug("PAGE allocator self-check passed\n");
}
