#include "../api.h"
#include "../klog.h"
#include "addr.h"
#include "page.h"
#include "../lib/kstring.h"
#include "../panic.h"

enum { MAX_RUN = 64, E820_MAX = 32 };
enum { PAGE_FREE, PAGE_USED, PAGE_RESERVED };

typedef struct PACKED {
  u32 base, base_hi, length, length_hi, type, attributes;
} E820Entry;

typedef struct {
  u32 start, end, type;
} Range;

static u8* mem_map;
static u32 nr_pages, metadata_pages;
static Range ranges[E820_MAX];
static u32 range_count;
static int logged_extended_allocation;
u32 free_pages, usable_memory;

int page_boot_usable(phys_t start) {
  u32 end = start + PAGE_SIZE;
  int usable = 0;
  for (u32 i = 0; i < range_count; i++) {
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

static void publish_pages(phys_t start, phys_t end) {
  phys_t metadata = virt_to_phys(mem_map);
  for (phys_t addr = start; addr < end; addr += PAGE_SIZE)
    if ((addr < metadata || addr >= metadata + metadata_pages * PAGE_SIZE) &&
        page_boot_usable(addr)) {
      mem_map[addr / PAGE_SIZE] = PAGE_FREE;
      free_pages++;
    }
}

/* 【简化实现：物理内存布局】按 E820 建立逐页状态表，但将地址截在 USER_BASE
 * （1 GiB），并把 52 MiB 以下整体留给启动资源；usable_memory 记录最高可用
 * 区域的结束地址，不是可分配字节总和。此限制来自当前恒等映射布局。
 * 替换方向：按实际内核/启动资源逐段登记保留区，分离物理地址与内核虚拟地址，
 * 再扩展可管理物理范围；不能只放大上限，否则会与用户虚拟地址窗口冲突。 */
void page_init(void) {
  u32 count = *(volatile u16*)phys_to_virt(E820_COUNT_ADDRESS);
  const E820Entry* map = phys_to_virt(E820_TABLE_ADDRESS);
  if (!count || count > E820_MAX)
    panic("Invalid E820 memory map", count, 0);

  for (u32 i = 0; i < count; i++) {
    const E820Entry* e = &map[i];
    if (!(e->attributes & 1) || e->base_hi || e->base >= USER_BASE ||
        (!e->length_hi && !e->length))
      continue;
    /* The supervisor identity window ends at the SNX link base. Clamp
       before adding the E820 length, which may exceed 32 bits. */
    u32 end = e->length_hi || e->length > USER_BASE - e->base
                  ? USER_BASE : e->base + e->length;
    ranges[range_count++] = (Range){e->base, end, e->type};
    if (e->type == 1 && end > usable_memory)
      usable_memory = end;
  }

  nr_pages = usable_memory / PAGE_SIZE;
  metadata_pages = (nr_pages + PAGE_SIZE - 1) / PAGE_SIZE;
  /* Lower memory contains the kernel, boot tables, RAM disk and graphics. */
  u32 run = 0, metadata_base = 0;
  u32 boot_end = usable_memory < BOOT_IDENTITY_BYTES ? usable_memory : BOOT_IDENTITY_BYTES;
  boot_end = page_floor(boot_end);
  for (u32 pfn = BOOT_POOL_START / PAGE_SIZE; pfn < boot_end / PAGE_SIZE; pfn++) {
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
    panic("No boot space for page metadata", usable_memory, metadata_pages);

  mem_map = phys_to_virt(metadata_base);
  memset(mem_map, PAGE_RESERVED, metadata_pages * PAGE_SIZE);
  publish_pages(BOOT_POOL_START, boot_end);
  if (!free_pages)
    panic("No allocatable physical pages", usable_memory, metadata_pages);
  pr_debug("PAGE metadata base=");
  pr_debug_hex(metadata_base);
  pr_debug(" pages=");
  pr_debug_hex(metadata_pages);
  pr_debug("\n");
}

void page_publish_extended(void) {
  u32 before = free_pages;
  publish_pages(BOOT_IDENTITY_BYTES, nr_pages * PAGE_SIZE);
  pr_debug("PAGE extended pages=");
  pr_debug_hex(free_pages - before);
  pr_debug("\n");
}

/* 【简化实现：单一页池】kpage 和用户页仍从同一个池按线性扫描分配，最多 64 页
 * 连续；两类页目前只在类型和契约上分开。替换方向：按直接映射边界划分低/高区，
 * kpage 只取低区，用户页优先高区并经 kmap 清零；再按需要引入空闲链表或
 * 伙伴分配器。 */
static phys_t alloc_run(u32 count) {
  if (!count || count > MAX_RUN || count > free_pages)
    return PHYS_NULL;
  for (u32 i = BOOT_POOL_START / PAGE_SIZE; i + count <= nr_pages; i++) {
    u32 j = 0;
    while (j < count && mem_map[i + j] == PAGE_FREE)
      j++;
    if (j == count) {
      memset(mem_map + i, PAGE_USED, count);
      free_pages -= count;
      phys_t page = i * PAGE_SIZE;
      /* Every allocatable page is inside the direct mapping for now. */
      memset(phys_to_virt(page), 0, count * PAGE_SIZE);
      if (page >= BOOT_IDENTITY_BYTES && !logged_extended_allocation) {
        logged_extended_allocation = 1;
        pr_debug("PAGE first extended allocation=");
        pr_debug_hex(page);
        pr_debug("\n");
      }
      return page;
    }
    i += j;
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
  phys_t page = alloc_run(count);
  return page ? phys_to_virt(page) : 0;
}

void* kpage_alloc(void) { return kpage_alloc_run(1); }

int kpage_free_run(void* page, u32 count) { return free_run(virt_to_phys(page), count); }

int kpage_free(void* page) { return kpage_free_run(page, 1); }

phys_t page_alloc(void) { return alloc_run(1); }

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
