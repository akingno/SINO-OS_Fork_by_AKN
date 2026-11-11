#include "../api.h"
#include "../klog.h"
#include "addr.h"
#include "page.h"
#include "paging.h"
#include "kmap.h"
#include "../lib/kstring.h"
#include "../panic.h"

// 一次性最大连续分配 64 页，最大区域条目 32
enum { MAX_RUN = 64, E820_MAX = 32 };
// 每个物理页的状态
enum { PAGE_FREE, PAGE_USED, PAGE_RESERVED };

/*
BIOS 返回的一条 E820 内存记录
base: 物理区域起始地址的低32位
base_hi: 高32位（需要是因为一个区域的结束位置可能超过32位地址范围）
length:  区域长度的低32位
length_hi: 区域长度的高32位
type: 内存区域的类型（1 usable 2 reserved...）type == 1为可分配 RAM
attributes: 扩展属性，我们检查最低位来看此条目是否有效
*/
typedef struct PACKED {
  u32 base, base_hi, length, length_hi, type, attributes;
} E820Entry;

/*
类似 E820Entry，但是不少BIOS提供的格式，而是简化的，
所以此处使用 Page Frame Number = Phisical Addr / 4096
start_pfn: 物理区域起始页框编号
end_pfn: 结束编号
type: 同E820Entry的type
*/
typedef struct {
  u32 start_pfn, end_pfn, type;
} Range;

// 最大 32 个 物理区域描述（一段，起始到结束，以及type，如上描述）
static Range ranges[E820_MAX];
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
  u32 pfn = start / PAGE_SIZE;
  int usable = 0;
  for (u32 i = 0; i < range_count; i++) {
    const Range* r = &ranges[i];
    if (r->start_pfn > pfn || r->end_pfn <= pfn)  // 这一段在检查范围外，跳过
      continue;
    if (r->type != 1)  // 这一段在检查范围内且不可用，返回0
      return 0;
    usable = 1;
  }
  return usable;
}

// 把一段物理页正式标为分配器的可分配资源
// TODO: 这个函数和 page_boot_usable 能不能结合起来来优化遍历？
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

// 从E820开始的 内存分配器展开
// TODO: 这个函数需要拆分
void page_init(void) {
  // 从内存里读取条目数量和数组
  u32 count = *(volatile u16*)phys_to_virt(E820_COUNT_ADDRESS);
  const E820Entry* map = phys_to_virt(E820_TABLE_ADDRESS);
  if (!count || count > E820_MAX) {
    panic("Invalid E820 memory map", count, 0);
  }

  for (u32 i = 0; i < count; i++) {
    const E820Entry* e = &map[i];
    // 排除 无效条目、超32为范围以及区域长度为0
    if (!(e->attributes & 1) || e->base_hi || (!e->length_hi && !e->length)) {
      continue;
    }
    // TODO: base_pfn 当前向下取整，
    // TODO: 如果 E820 的可用区域起点不是 4 KiB
    // 对齐的，就有可能把只部分可用的页面当作完整可用页。
    // TODO: 更加严格的方式是对可用区域起点向上对齐，结束位置向下对齐。
    u32 base_pfn = e->base / PAGE_SIZE;
    // 结束地址（可能超32位）并限制在 4GiB内，然后得到结束FPN
    unsigned long long end64 =
        (unsigned long long)e->base +
        ((unsigned long long)e->length_hi << 32 | e->length);
    if (end64 > (unsigned long long)PHYS_LIMIT_PAGES << PAGE_SHIFT) {
      end64 = (unsigned long long)PHYS_LIMIT_PAGES << PAGE_SHIFT;
    }
    u32 end_pfn = (u32)(end64 >> PAGE_SHIFT);
    if (base_pfn >= end_pfn) {
      continue;
    }
    // 保存range
    ranges[range_count++] = (Range){base_pfn, end_pfn, e->type};
    // 更新得到最大有效结束FPN
    if (e->type == 1 && end_pfn > nr_pages) {
      nr_pages = end_pfn;
    }
  }

  // 向上取整再除，得到需要的 mem_map 大小
  metadata_pages = (nr_pages + PAGE_SIZE - 1) / PAGE_SIZE;

  // 初始 64 MiB 范围与实际可用物理页上界中较小的
  u32 boot_end_pfn = nr_pages < BOOT_IDENTITY_BYTES / PAGE_SIZE
                         ? nr_pages
                         : BOOT_IDENTITY_BYTES / PAGE_SIZE;

  u32 run = 0, metadata_base = 0;
  // 找 metadata_pages 个连续页面来放 mem_map
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
  // 初始化 mem_map
  if (!metadata_base) {
    panic("page.c: No boot space for page metadata", nr_pages, metadata_pages);
  }
  // 先将全部页面视为不可分配 -> 再用 publish_pages
  // 把已经确认可以使用的页面标记为 FREE
  mem_map = phys_to_virt(metadata_base);
  memset(mem_map, PAGE_RESERVED, metadata_pages * PAGE_SIZE);
  publish_pages(LOW_START_PFN, boot_end_pfn);
  if (!free_pages) {
    panic("page.c: No allocatable physical pages", nr_pages, metadata_pages);
  }
  pr_debug("PAGE metadata base=");
  pr_debug_hex(metadata_base);
  pr_debug(" pages=");
  pr_debug_hex(metadata_pages);
  pr_debug(" nr_pages=");
  pr_debug_hex(nr_pages);
  pr_debug("\n");
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
  // TODO: 这个算法可以改进，目前是类似 nextfit 的线性扫描，O(1)
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
static int free_run(phys_t page, u32 count) {
  u32 pfn = page / PAGE_SIZE;
  // 此处检查分配器释放是否是完整物理页，因为不能从页中间释放
  if (!count || count > MAX_RUN || !page_aligned(page) ||
      page < BOOT_POOL_START || pfn >= nr_pages || count > nr_pages - pfn) {
    return 0;
  }
  for (u32 i = 0; i < count; i++) {  // 防重复释放
    if (mem_map[pfn + i] != PAGE_USED) {
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
  return free_run(virt_to_phys(page), count);
}

// 释放某一页
// TODO: 释放一页可以单独优化，避免冗余检查
int kpage_free(void* page) {
  return kpage_free_run(page, 1);
}

// 分配用户进程的物理页
//page_alloc() 的物理内存选择顺序
//0–52 MiB: 保留区，不参与分配
//52 MiB–1 GiB: Low Memory，第二选择：High Memory 分配失败才尝试
//1–4 GiB: High Memory，第一选择：优先从这里分配
//（假设 direct_map_end 为 1 GiB）
phys_t page_alloc(void) {
  u32 low_end = direct_map_end ? direct_map_end / PAGE_SIZE : nr_pages;
  phys_t page = PHYS_NULL;
  if (low_end < nr_pages) { // 尝试分配 High Memory
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
  return free_run(page, 1);
}

// 物理页分配器自检，每次启动运行
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
  if (!user || !page_aligned(user) || user < BOOT_POOL_START ||
      free_pages + 1 != before)
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
  if (!kpage_free_run(block, 2) || kpage_free_run(block, 2) ||
      free_pages != before)
    panic("Page run release", (u32)block, free_pages);
  pr_debug("PAGE allocator self-check passed\n");
}
