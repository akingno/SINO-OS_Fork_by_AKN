#include "../api.h"
#include "../klog.h"
#include "addr.h"
#include "page.h"
#include "../lib/kstring.h"
#include "../panic.h"

enum { MAX_RUN = 64, E820_MAX = 32 };
enum { PAGE_FREE, PAGE_USED, PAGE_RESERVED };
/* loader.asm leaves the E820 entry count and the entries at these addresses. */
#define E820_COUNT_ADDRESS ((phys_t)0x4f00u)
#define E820_TABLE_ADDRESS ((phys_t)0x5000u)

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

/* 【简化实现：页分配与地址耦合】每次从固定起点线性找连续空闲页，最多 64 页，
 * 返回的指针直接等于物理地址，清零和所有调用方都依赖恒等映射。
 * 替换方向：按实际需求采用空闲页链表/分级位图，连续页可用伙伴分配器；
 * 物理页分配返回物理地址或页描述符，内核通过显式映射获得可访问指针。
 * 分配器选型不能代替映射改造；取消恒等映射时，释放接口也必须同步调整。 */
void* page_alloc_run(u32 count) {
  if (!count || count > MAX_RUN || count > free_pages)
    return 0;
  for (u32 i = BOOT_POOL_START / PAGE_SIZE; i + count <= nr_pages; i++) {
    u32 j = 0;
    while (j < count && mem_map[i + j] == PAGE_FREE)
      j++;
    if (j == count) {
      memset(mem_map + i, PAGE_USED, count);
      free_pages -= count;
      phys_t page = i * PAGE_SIZE;
      void* p = phys_to_virt(page);
      memset(p, 0, count * PAGE_SIZE);
      if (page >= BOOT_IDENTITY_BYTES && !logged_extended_allocation) {
        logged_extended_allocation = 1;
        pr_debug("PAGE first extended allocation=");
        pr_debug_hex(page);
        pr_debug("\n");
      }
      return p;
    }
    i += j;
  }
  return 0;
}

void* page_alloc(void) { return page_alloc_run(1); }

int page_free_run(void* p, u32 count) {
  phys_t addr = virt_to_phys(p);
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
    if (mem_map[virt_to_phys(mem_map) / PAGE_SIZE + i] != PAGE_RESERVED)
      panic("Page metadata reservation", virt_to_phys(mem_map), i);
  if (page_free(mem_map) || page_free(phys_to_virt(0x200000u)) || free_pages != before)
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
