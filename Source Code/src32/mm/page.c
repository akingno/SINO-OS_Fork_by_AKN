#include "../api.h"
#include "../klog.h"
#include "addr.h"
#include "page.h"
#include "e820.h"
#include "paging.h"
#include "kmap.h"
#include "../lib/kstring.h"
#include "../panic.h"

// 一次性最大连续分配 64 页，最大区域条目 32
enum { MAX_RUN = 64, E820_MAX = 32 };
// 每个物理页的状态
enum { PAGE_FREE, PAGE_USED, PAGE_RESERVED, PAGE_KERNEL, PAGE_GUARDED };

// 最大 32 个 物理区域描述（一段，起始到结束，以及type，如上描述）
static BootRange ranges[E820_MAX];
static u32 range_count;  // ranges 的有效条目

// u8数组, 每个元素是一个物理页的状态（其实只要两bit，这里剩下6bit的全冗余了）
// 需要 mem_map[1048576] 页来存 4GB
static u8* mem_map;
// 需要 256 metadata_pages 来存 mem_map[1048576]
static u32 metadata_pages;

// 分配一个物理页则--，释放++
u32 free_pages;
// 可用 E820 区域中的最大的结束 PFN，但中间可能有被保留的 PFN
u32 nr_pages;

// 通用物理页分配池允许使用的最低 PFN
#define LOW_START_PFN (BOOT_POOL_START / PAGE_SIZE)

// 类似 next fit的搜索游标，下次分配从这开始找
static u32 low_hint = LOW_START_PFN;
static u32 high_hint;

// 调试用: 是否已经打印过首次从启动恒等映射范围之外分配物理页的消息
// 调试用: 是否已经打印过首次从直接映射范围之外分配物理页的消息
// 本质是两个 bool
static int logged_extended_allocation, logged_high_allocation;

// 遍历 ranges 看start 所在 FPN 其所在范围内是否可用
// 因为 ranges 内条目并不是顺序放的，所以要遍历所有条目才行
int page_boot_usable(phys_t start) {
  return e820_page_usable(ranges, range_count, start / PAGE_SIZE);
}

// 把一段物理页正式标为分配器的可分配资源
// 可用性规则已统一到 e820_page_usable。
// TODO(MM-07): 测量启动开销后，考虑规范化区间以减少逐页重复遍历。
static void publish_pages(u32 start_pfn, u32 end_pfn) {
  u32 metadata_pfn = virt_to_phys(mem_map) / PAGE_SIZE;
  // 把这个 pfn 范围内的、没有存放mem map metadata 且可用的页面标为FREE
  for (u32 pfn = start_pfn; pfn < end_pfn; pfn++) {
    if ((pfn < metadata_pfn || pfn >= metadata_pfn + metadata_pages) &&
        page_boot_usable(pfn * PAGE_SIZE)) {
      mem_map[pfn] = PAGE_FREE;
      free_pages++;
    }
  }
}

/* Only the loader's E820 handoff is read here. No page allocation is allowed
 * until the metadata has been placed and the initial pool published. */
static void read_boot_memory_map(void) {
  u32 count = *(volatile u16*)phys_to_virt(E820_COUNT_ADDRESS);
  const E820Entry* map = (const E820Entry*)phys_to_virt(E820_TABLE_ADDRESS);
  if (!count || count > E820_MAX) {
    panic("Invalid E820 memory map", count, 0);
  }
  for (u32 i = 0; i < count; i++) {
    BootRange range;
    if (!e820_range(&map[i], &range)) continue;
    ranges[range_count++] = range;
    if (range.type == 1 && range.end_pfn > nr_pages)
      nr_pages = range.end_pfn;
  }
}

/* Metadata must fit in complete usable pages inside the bootstrap mapping.
 * Return a physical address; no access to unpublished/high pages is needed. */
static phys_t find_boot_metadata(u32 boot_end_pfn) {
  u32 run = 0;
  for (u32 pfn = LOW_START_PFN; pfn < boot_end_pfn; pfn++) {
    if (!page_boot_usable(pfn * PAGE_SIZE)) {
      run = 0;
      continue;
    }
    if (++run == metadata_pages)
      return (pfn + 1 - run) * PAGE_SIZE;
  }
  panic("page_init: No boot space for page metadata", nr_pages, metadata_pages);
  return PHYS_NULL;
}

static void init_page_metadata(u32 boot_end_pfn) {
  phys_t base = find_boot_metadata(boot_end_pfn);
  mem_map = (u8*)phys_to_virt(base);
  /* All pages start reserved, including the metadata itself. */
  memset(mem_map, PAGE_RESERVED, metadata_pages * PAGE_SIZE);
}

static void publish_boot_pages(u32 boot_end_pfn) {
  publish_pages(LOW_START_PFN, boot_end_pfn);
  if (!free_pages)
    panic("page_init: No allocatable physical pages", nr_pages, metadata_pages);
}

static void log_page_metadata(void) {
  pr_debug("PAGE metadata base=");
  pr_debug_hex(virt_to_phys(mem_map));
  pr_debug(" pages=");
  pr_debug_hex(metadata_pages);
  pr_debug(" nr_pages=");
  pr_debug_hex(nr_pages);
  pr_debug("\n");
}

/* Called once: parse -> size -> place/reserve metadata -> publish boot pool.
 * Extended pages are still published separately after paging_kernel_init. */
void page_init(void) {
  read_boot_memory_map();
  metadata_pages = (nr_pages + PAGE_SIZE - 1) / PAGE_SIZE;
  u32 boot_end_pfn = nr_pages < BOOT_IDENTITY_BYTES / PAGE_SIZE
                         ? nr_pages : BOOT_IDENTITY_BYTES / PAGE_SIZE;
  init_page_metadata(boot_end_pfn);
  publish_boot_pages(boot_end_pfn);
  log_page_metadata();
}

// 发布启动映射范围以外的物理页
void page_publish_extended(void) {
  u32 before = free_pages;
  publish_pages(BOOT_IDENTITY_BYTES / PAGE_SIZE, nr_pages);
  pr_debug("PAGE extended pages=");
  pr_debug_hex(free_pages - before);
  pr_debug("\n");
}

// 清零物理页
// page: 要清零的起始物理地址
// count: 清零连续多少个物理页
static void zero_run(phys_t page, u32 count) {
  u32 dm_end_pfn = direct_map_end / PAGE_SIZE;

  if (!dm_end_pfn || page / PAGE_SIZE + count <= dm_end_pfn) {  // 在直接映射区
    memset(phys_to_virt(page), 0, count * PAGE_SIZE);
  } else {  // 高内存，地址不能直接当指针用，需要kmap
    for (u32 k = 0; k < count; k++) {
      void* mapped = kmap(page + k * PAGE_SIZE);
      memset(mapped, 0, PAGE_SIZE);
      kunmap(mapped);
    }
  }
}

// 在指定范围内寻找 count
// 个物理地址连续且全部空闲的页面，标记为已使用并返回起始地址
static phys_t alloc_from(u32 start_pfn, u32 end_pfn, u32 count, u32* hint) {
  if (start_pfn >= end_pfn || !count || count > MAX_RUN || count > free_pages) {
    return PHYS_NULL;
  }
  u32 h = *hint;  // 上次从哪开始，并更新位下次从哪开始
  if (h < start_pfn || h >= end_pfn) {
    h = start_pfn;
  }
  // 两轮搜索：1. 从 hint 搜索到范围末尾 2. 如果没找到，就回头搜索前半段
  // next-fit 使用游标减少重复搜索，最坏仍为 O(N) 页状态扫描。
  // TODO(MM-08): 检查 hint 分界的连续块完整性，再根据测量决定索引优化。
  for (int pass = 0; pass < 2; pass++) {
    u32 from = pass ? start_pfn : h;
    u32 to = pass ? h : end_pfn;
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

// 物理地址和页数，尝试释放对应的连续页面。
static int free_run(phys_t page, u32 count, u8 state) {
  u32 pfn = page / PAGE_SIZE;
  // 此处检查分配器释放是否是完整物理页，因为不能从页中间释放
  if (!count || count > MAX_RUN || !page_aligned(page) ||
      page < BOOT_POOL_START || pfn >= nr_pages || count > nr_pages - pfn) {
    return 0;
  }
  for (u32 i = 0; i < count; i++) {  // 防重复释放
    if (mem_map[pfn + i] != state) {
      return 0;
    }
  }
  // 只标为不占用，但没清零，再次分配时才用 zero_run 清零
  memset(mem_map + pfn, PAGE_FREE, count);
  free_pages += count;
  return 1;
}

/**
 *内核页面分配路径：
 *kpage_alloc()(申请一个内核页面) -> kpage_alloc_run(1) (确定low mem 范围)
 *  -> zero_run (清零) -> 返回内核指针
 *
 *内核一页释放路径：
 *kpage_free(ptr) -> kpage_free_run(ptr, 1)（转回物理地址） -> free_run()
 *(验证并标为 FREE)
 */
// 封装，分配内核可直接访问的连续物理页
void* kpage_alloc_run(u32 count) {
  u32 low_end = direct_map_end ? direct_map_end / PAGE_SIZE : nr_pages;
  phys_t page = alloc_from(LOW_START_PFN, low_end, count, &low_hint);
  if (!page) {
    return 0;
  }
  memset(mem_map + page / PAGE_SIZE, PAGE_KERNEL, count);
  zero_run(page, count);
  if (page >= BOOT_IDENTITY_BYTES && !logged_extended_allocation) {
    logged_extended_allocation = 1;
    pr_debug("PAGE first extended allocation=");
    pr_debug_hex(page);
    pr_debug("\n");
  }
  return phys_to_virt(page);
}

// 分配一页的接口
// 分配一页也许可以单独优化，避免冗余检查
void* kpage_alloc(void) {
  return kpage_alloc_run(1);
}

// 释放连续的内核页面
int kpage_free_run(void* page, u32 count) {
  return free_run(virt_to_phys(page), count, PAGE_KERNEL);
}

// 释放某一页
// TODO: 释放一页可以单独优化，避免冗余检查
int kpage_free(void* page) {
  return kpage_free_run(page, 1);
}

/* Paging alone changes a live kernel allocation into a guard. Guarded pages
 * cannot be freed through either allocator until their PTE has been restored. */
int kpage_guard_claim(void* page) {
  u32 address = (u32)page, pfn = address / PAGE_SIZE;
  if (!page_aligned(address) || address < BOOT_POOL_START ||
      address >= direct_map_end || pfn >= nr_pages || mem_map[pfn] != PAGE_KERNEL)
    return 0;
  mem_map[pfn] = PAGE_GUARDED;
  return 1;
}
void kpage_guard_release(void* page) {
  u32 pfn = virt_to_phys(page) / PAGE_SIZE;
  if (pfn >= nr_pages || mem_map[pfn] != PAGE_GUARDED)
    panic("Invalid guarded page release", (u32)page, pfn);
  mem_map[pfn] = PAGE_KERNEL;
}

// 分配用户进程的物理页
// page_alloc() 的物理内存选择顺序
// 0–52 MiB: 保留区，不参与分配
// 52 MiB–1 GiB: Low Memory，第二选择：High Memory 分配失败才尝试
// 1–4 GiB: High Memory，第一选择：优先从这里分配
// （假设 direct_map_end 为 1 GiB）
phys_t page_alloc(void) {
  u32 low_end = direct_map_end ? direct_map_end / PAGE_SIZE : nr_pages;
  phys_t page = PHYS_NULL;
  if (low_end < nr_pages) {  // 尝试分配 High Memory
    page = alloc_from(low_end, nr_pages, 1, &high_hint);
  }
  if (!page) {
    page = alloc_from(LOW_START_PFN, low_end, 1, &low_hint);
  }
  if (!page) {
    return PHYS_NULL;
  }
  zero_run(page, 1);
  if (direct_map_end && page >= direct_map_end && !logged_high_allocation) {
    logged_high_allocation = 1;
    pr_debug("PAGE first high allocation=");
    pr_debug_hex(page);
    pr_debug("\n");
  }
  return page;
}

int page_free(phys_t page) {
  return free_run(page, 1, PAGE_USED);
}

// 物理页分配器自检，每次启动运行
void page_allocator_test(void) {
  u32 before = free_pages;
  for (u32 i = 0; i < metadata_pages; i++)
    if (mem_map[virt_to_phys(mem_map) / PAGE_SIZE + i] != PAGE_RESERVED) {
      panic("page_allocator_test: Page metadata reservation", virt_to_phys(mem_map), i);
    }
  if (kpage_free(mem_map) || page_free(0x200000u) || free_pages != before) {
    panic("page_allocator_test: Reserved page free validation", free_pages, before);
  }

  u32* a = (u32*)kpage_alloc();
  u32* b = (u32*)kpage_alloc();
  if (!a || !b || a == b || ((u32)a | (u32)b) % PAGE_SIZE) {
    panic("page_allocator_test: Page allocation", (u32)a, (u32)b);
  }
  *a = 0x46454c49;
  *b = 0x58333221;
  if (*a != 0x46454c49 || *b != 0x58333221) {
    panic("page_allocator_test: Memory read/write", 0, 0);
  }
  if (!kpage_free(a) || !kpage_free(b) || kpage_free(a) ||
      free_pages != before) {
    panic("page_allocator_test: Page free validation", free_pages, before);
  }

  phys_t user = page_alloc();
  if (!user || !page_aligned(user) || user < BOOT_POOL_START ||
      free_pages + 1 != before) {
    panic("page_allocator_test: User page allocation", user, free_pages);
  }
  if (!page_free(user) || page_free(user) || free_pages != before) {
    panic("page_allocator_test: User page release", user, free_pages);
  }

  u8* block = (u8*)kpage_alloc_run(2);
  if (!block || (u32)block % PAGE_SIZE || free_pages + 2 != before) {
    panic("page_allocator_test: Page run allocation", (u32)block, free_pages);
  }
  for (u32 i = 0; i < 2 * PAGE_SIZE; i++) {
    if (block[i]) {
      panic("page_allocator_test: Page run zeroing", (u32)block, i);
    }
  }
  block[PAGE_SIZE] = 1;
  if (!kpage_free_run(block, 2) || kpage_free_run(block, 2) ||
      free_pages != before) {
    panic("page_allocator_test: Page run release", (u32)block, free_pages);
  }
  pr_debug("PAGE allocator self-check passed\n");
}
