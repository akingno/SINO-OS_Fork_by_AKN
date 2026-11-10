#include "../api.h"
#include "../klog.h"
#include "addr.h"
#include "page.h"
#include "paging.h"
#include "kmap.h"
#include "../lib/kstring.h"
#include "../panic.h"

enum { MAX_RUN = 64, E820_MAX = 32 };
enum { PAGE_FREE, PAGE_USED, PAGE_RESERVED };

typedef struct PACKED {
  u32 base, base_hi, length, length_hi, type, attributes;
} E820Entry;

typedef struct {
  u32 start_pfn, end_pfn, type;
} Range;

static u8* mem_map;
static u32 metadata_pages;
static Range ranges[E820_MAX];
static u32 range_count;
static int logged_extended_allocation, logged_high_allocation;
u32 free_pages, nr_pages;

#define LOW_START_PFN (BOOT_POOL_START / PAGE_SIZE)
static u32 low_hint = LOW_START_PFN;
static u32 high_hint;

int page_boot_usable(phys_t start) {
  u32 pfn = start / PAGE_SIZE;
  int usable = 0;
  for (u32 i = 0; i < range_count; i++) {
    const Range* r = &ranges[i];
    if (r->start_pfn > pfn || r->end_pfn <= pfn)
      continue;
    if (r->type != 1)
      return 0;
    usable = 1;
  }
  return usable;
}

static void publish_pages(u32 start_pfn, u32 end_pfn) {
  u32 metadata_pfn = virt_to_phys(mem_map) / PAGE_SIZE;
  for (u32 pfn = start_pfn; pfn < end_pfn; pfn++)
    if ((pfn < metadata_pfn || pfn >= metadata_pfn + metadata_pages) &&
        page_boot_usable(pfn * PAGE_SIZE)) {
      mem_map[pfn] = PAGE_FREE;
      free_pages++;
    }
}

void page_init(void) {
  u32 count = *(volatile u16*)phys_to_virt(E820_COUNT_ADDRESS);
  const E820Entry* map = phys_to_virt(E820_TABLE_ADDRESS);
  if (!count || count > E820_MAX)
    panic("Invalid E820 memory map", count, 0);

  for (u32 i = 0; i < count; i++) {
    const E820Entry* e = &map[i];
    if (!(e->attributes & 1) || e->base_hi ||
        (!e->length_hi && !e->length))
      continue;
    u32 base_pfn = e->base / PAGE_SIZE;
    unsigned long long end64 =
        (unsigned long long)e->base +
        ((unsigned long long)e->length_hi << 32 | e->length);
    if (end64 > (unsigned long long)PHYS_LIMIT_PAGES << PAGE_SHIFT)
      end64 = (unsigned long long)PHYS_LIMIT_PAGES << PAGE_SHIFT;
    u32 end_pfn = (u32)(end64 >> PAGE_SHIFT);
    if (base_pfn >= end_pfn)
      continue;
    ranges[range_count++] = (Range){base_pfn, end_pfn, e->type};
    if (e->type == 1 && end_pfn > nr_pages)
      nr_pages = end_pfn;
  }

  metadata_pages = (nr_pages + PAGE_SIZE - 1) / PAGE_SIZE;
  u32 boot_end_pfn = nr_pages < BOOT_IDENTITY_BYTES / PAGE_SIZE
                       ? nr_pages : BOOT_IDENTITY_BYTES / PAGE_SIZE;
  u32 run = 0, metadata_base = 0;
  for (u32 pfn = LOW_START_PFN; pfn < boot_end_pfn; pfn++) {
    if (!page_boot_usable(pfn * PAGE_SIZE)) {
      run = 0;
      continue;
    }
    if (++run == metadata_pages) {
      metadata_base = (pfn + 1 - run) * PAGE_SIZE;
      break;
    }
  }
  if (!metadata_base)
    panic("No boot space for page metadata", nr_pages, metadata_pages);

  mem_map = phys_to_virt(metadata_base);
  memset(mem_map, PAGE_RESERVED, metadata_pages * PAGE_SIZE);
  publish_pages(LOW_START_PFN, boot_end_pfn);
  if (!free_pages)
    panic("No allocatable physical pages", nr_pages, metadata_pages);
  pr_debug("PAGE metadata base=");
  pr_debug_hex(metadata_base);
  pr_debug(" pages=");
  pr_debug_hex(metadata_pages);
  pr_debug(" nr_pages=");
  pr_debug_hex(nr_pages);
  pr_debug("\n");
}

void page_publish_extended(void) {
  u32 before = free_pages;
  publish_pages(BOOT_IDENTITY_BYTES / PAGE_SIZE, nr_pages);
  pr_debug("PAGE extended pages=");
  pr_debug_hex(free_pages - before);
  pr_debug("\n");
}

static void zero_run(phys_t page, u32 count) {
  u32 dm_end_pfn = direct_map_end / PAGE_SIZE;
  if (!dm_end_pfn || page / PAGE_SIZE + count <= dm_end_pfn) {
    memset(phys_to_virt(page), 0, count * PAGE_SIZE);
  } else {
    for (u32 k = 0; k < count; k++) {
      void* mapped = kmap(page + k * PAGE_SIZE);
      memset(mapped, 0, PAGE_SIZE);
      kunmap(mapped);
    }
  }
}

static phys_t alloc_from(u32 start_pfn, u32 end_pfn, u32 count, u32* hint) {
  if (start_pfn >= end_pfn || !count || count > MAX_RUN || count > free_pages)
    return PHYS_NULL;
  u32 h = *hint;
  if (h < start_pfn || h >= end_pfn) h = start_pfn;
  for (int pass = 0; pass < 2; pass++) {
    u32 from = pass ? start_pfn : h;
    u32 to   = pass ? h : end_pfn;
    for (u32 i = from; i + count <= to; i++) {
      u32 j = 0;
      while (j < count && mem_map[i + j] == PAGE_FREE)
        j++;
      if (j == count) {
        memset(mem_map + i, PAGE_USED, count);
        free_pages -= count;
        *hint = i + count;
        return (phys_t)(i * PAGE_SIZE);
      }
      i += j;
    }
  }
  return PHYS_NULL;
}

static int free_run(phys_t page, u32 count) {
  u32 pfn = page / PAGE_SIZE;
  if (!count || count > MAX_RUN || !page_aligned(page) || page < BOOT_POOL_START ||
      pfn >= nr_pages || count > nr_pages - pfn)
    return 0;
  for (u32 i = 0; i < count; i++)
    if (mem_map[pfn + i] != PAGE_USED)
      return 0;
  memset(mem_map + pfn, PAGE_FREE, count);
  free_pages += count;
  return 1;
}

void* kpage_alloc_run(u32 count) {
  u32 low_end = direct_map_end ? direct_map_end / PAGE_SIZE : nr_pages;
  phys_t page = alloc_from(LOW_START_PFN, low_end, count, &low_hint);
  if (!page) return 0;
  zero_run(page, count);
  if (page >= BOOT_IDENTITY_BYTES && !logged_extended_allocation) {
    logged_extended_allocation = 1;
    pr_debug("PAGE first extended allocation=");
    pr_debug_hex(page);
    pr_debug("\n");
  }
  return phys_to_virt(page);
}

void* kpage_alloc(void) { return kpage_alloc_run(1); }

int kpage_free_run(void* page, u32 count) { return free_run(virt_to_phys(page), count); }

int kpage_free(void* page) { return kpage_free_run(page, 1); }

phys_t page_alloc(void) {
  u32 low_end = direct_map_end ? direct_map_end / PAGE_SIZE : nr_pages;
  phys_t page = PHYS_NULL;
  if (low_end < nr_pages)
    page = alloc_from(low_end, nr_pages, 1, &high_hint);
  if (!page)
    page = alloc_from(LOW_START_PFN, low_end, 1, &low_hint);
  if (!page) return PHYS_NULL;
  zero_run(page, 1);
  if (direct_map_end && page >= direct_map_end && !logged_high_allocation) {
    logged_high_allocation = 1;
    pr_debug("PAGE first high allocation=");
    pr_debug_hex(page);
    pr_debug("\n");
  }
  return page;
}

int page_free(phys_t page) { return free_run(page, 1); }

void page_allocator_test(void) {
  u32 before = free_pages;
  for (u32 i = 0; i < metadata_pages; i++)
    if (mem_map[virt_to_phys(mem_map) / PAGE_SIZE + i] != PAGE_RESERVED)
      panic("Page metadata reservation", virt_to_phys(mem_map), i);
  if (kpage_free(mem_map) || page_free(0x200000u) || free_pages != before)
    panic("Reserved page free validation", free_pages, before);

  u32* a = kpage_alloc();
  u32* b = kpage_alloc();
  if (!a || !b || a == b || ((u32)a | (u32)b) % PAGE_SIZE)
    panic("Page allocation", (u32)a, (u32)b);
  *a = 0x46454c49;
  *b = 0x58333221;
  if (*a != 0x46454c49 || *b != 0x58333221)
    panic("Memory read/write", 0, 0);
  if (!kpage_free(a) || !kpage_free(b) || kpage_free(a) || free_pages != before)
    panic("Page free validation", free_pages, before);

  phys_t user = page_alloc();
  if (!user || !page_aligned(user) || user < BOOT_POOL_START || free_pages + 1 != before)
    panic("User page allocation", user, free_pages);
  if (!page_free(user) || page_free(user) || free_pages != before)
    panic("User page release", user, free_pages);

  u8* block = kpage_alloc_run(2);
  if (!block || (u32)block % PAGE_SIZE || free_pages + 2 != before)
    panic("Page run allocation", (u32)block, free_pages);
  for (u32 i = 0; i < 2 * PAGE_SIZE; i++)
    if (block[i])
      panic("Page run zeroing", (u32)block, i);
  block[PAGE_SIZE] = 1;
  if (!kpage_free_run(block, 2) || kpage_free_run(block, 2) || free_pages != before)
    panic("Page run release", (u32)block, free_pages);
  pr_debug("PAGE allocator self-check passed\n");
}
