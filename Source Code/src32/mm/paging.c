#include "../kernel.h"
#include "../api.h"
#include "../klog.h"
#include "page.h"
#include "paging.h"
#include "../lib/kstring.h"
#include "../panic.h"

#define USER_FLAGS (PAGE_PRESENT | PAGE_USER)
#define USER_FIRST_PDE (USER_BASE / TABLE_BYTES)
#define USER_END_PDE ((USER_V2_END + TABLE_BYTES - 1) / TABLE_BYTES)
/* Fixed low-memory pages holding the boot page directory and its tables. */
#define TEMP_PD ((phys_t)0x90000u)
#define TEMP_PT ((phys_t)0x1e0000u)
#define VBE_TABLES 3u
#define VBE_PAGE_FLAGS (PAGE_PRESENT | PAGE_WRITE | PAGE_WRITE_THROUGH | PAGE_CACHE_DISABLE)

_Static_assert(BOOT_POOL_START % TABLE_BYTES == 0 &&
               BOOT_IDENTITY_BYTES % TABLE_BYTES == 0 &&
               BOOT_POOL_START < BOOT_IDENTITY_BYTES,
               "initial pool is aligned to whole page tables");
_Static_assert(1 + (BOOT_IDENTITY_BYTES - BOOT_POOL_START) / TABLE_BYTES <= 16,
               "temporary page tables fit the low boot reserve");

static u32* kernel_pd;

void paging_activate(const u32* pd) { write_cr3(virt_to_phys(pd)); }

static void identity_table(u32* pd, u32 index, u32* pt, u32 flags) {
  u32 base = index * TABLE_BYTES;
  for (u32 i = 0; i < TABLE_ENTRIES; i++)
    pt[i] = make_entry(base + i * PAGE_SIZE, flags);
  if (!index)
    pt[0] = 0;
  pd[index] = make_entry(virt_to_phys(pt), PAGE_PRESENT | PAGE_WRITE);
}

/* 【简化实现：启动映射】固定页表暂时恒等映射低端执行区和初始物理页池，
 * 供开启分页及建立正式页表使用。这种启动过渡有必要，但不应决定长期布局。
 * 替换方向：完成正式内核映射后撤掉不再需要的启动映射；先迁移启动数据、
 * 页表及裸物理指针的访问，不能直接删除这里的映射。 */
void paging_bootstrap(void) {
  u32* pd = phys_to_virt(TEMP_PD);
  u32* pt = phys_to_virt(TEMP_PT);
  memset(pd, 0, PAGE_SIZE);
  /* Entry code, kernel BSS, stack and BIOS handoff all fit in the first 4 MiB. */
  identity_table(pd, 0, pt, PAGE_PRESENT | PAGE_WRITE);
  for (u32 i = BOOT_POOL_START / TABLE_BYTES; i < BOOT_IDENTITY_BYTES / TABLE_BYTES; i++) {
    pt += TABLE_ENTRIES;
    identity_table(pd, i, pt, PAGE_PRESENT | PAGE_WRITE);
  }
  write_cr3(TEMP_PD);
  write_cr0((read_cr0() & ~(CR0_CD | CR0_NW)) | CR0_PG | CR0_WP);
  pr_debug("PAGING bootstrap CR3=");
  pr_debug_hex(TEMP_PD);
  pr_debug("\n");
}

/* 【简化实现：长期恒等映射】将 identity_bytes 范围映射为 supervisor 可写，
 * 64 MiB 以上过滤非可用页，低端只排除零页；另按 4 MiB 粒度覆盖帧缓冲。
 * 因此空闲页、用户页的物理后备也有内核别名，并非只映射内核实际占用区；
 * supervisor 权限仍阻止 Ring 3 访问这些别名，但映射范围和权限过于宽泛。
 * 替换方向：按内核段设置权限，显式映射栈、堆、页表、启动资源和设备区域；
 * 用户后备页通过受控临时映射访问，或另行选择有边界的内核直接映射窗口。
 * 必须先解耦 page_alloc、页表遍历和映像装载中的物理地址/指针，才能收紧。 */
void paging_kernel_init(u32 identity_bytes, u32 framebuffer) {
  if (identity_bytes < BOOT_IDENTITY_BYTES || identity_bytes > USER_BASE ||
      identity_bytes % TABLE_BYTES)
    panic("Invalid kernel identity span", identity_bytes, 0);
  u32 identity_count = identity_bytes / TABLE_BYTES;
  u32 vbe = framebuffer / TABLE_BYTES;
  if (framebuffer && (vbe < identity_count || vbe > TABLE_ENTRIES - VBE_TABLES ||
      (vbe < USER_END_PDE && vbe + VBE_TABLES > USER_FIRST_PDE)))
    panic("Invalid VBE framebuffer range", framebuffer, 0);

  u32* pd = kpage_alloc();
  if (!pd)
    panic("No memory for kernel page directory", identity_bytes, free_pages);
  for (u32 i = 0; i < identity_count; i++) {
    u32* pt = kpage_alloc();
    if (!pt)
      panic("No memory for kernel page table", i, free_pages);
    identity_table(pd, i, pt, PAGE_PRESENT | PAGE_WRITE);
    if (i >= BOOT_IDENTITY_BYTES / TABLE_BYTES)
      for (u32 slot = 0; slot < TABLE_ENTRIES; slot++)
        if (!page_boot_usable(i * TABLE_BYTES + slot * PAGE_SIZE))
          pt[slot] = 0;
  }
  for (u32 i = 0; framebuffer && i < VBE_TABLES; i++) {
    u32* pt = kpage_alloc();
    if (!pt)
      panic("No memory for VBE page table", vbe + i, free_pages);
    identity_table(pd, vbe + i, pt, VBE_PAGE_FLAGS);
  }
  /* These pages are permanently owned by the kernel directory. User page
     directories share the supervisor entries and free only private tables. */
  paging_activate(pd);
  kernel_pd = pd;
  pr_debug("PAGING enabled: CR0=");
  pr_debug_hex(read_cr0());
  pr_debug(" CR3=");
  pr_debug_hex(virt_to_phys(pd));
  pr_debug(", null page unmapped\n");
  if (framebuffer) {
    pr_debug("VBE framebuffer mapped: ");
    pr_debug_hex(framebuffer);
    pr_debug(" 1024x768x32\n");
  } else pr_debug("VGA text console: 80x25\n");
}

void paging_activate_kernel(void) { paging_activate(kernel_pd); }

static int active(const u32* pd) {
  return page_floor(read_cr3()) == virt_to_phys(pd);
}

/* Page tables are kernel pages, so the direct mapping always reaches them. */
static u32* user_table(const u32* pd, u32 va) {
  if (va < USER_BASE || va >= USER_V2_END)
    return 0;
  u32 pde = pd[pde_index(va)];
  if ((pde & USER_FLAGS) != USER_FLAGS)
    return 0;
  return phys_to_virt(entry_address(pde));
}

u32* paging_user_create(void) {
  for (u32 i = USER_FIRST_PDE; i < USER_END_PDE; i++)
    if (kernel_pd[i] & PAGE_PRESENT)
      return 0;
  u32* pd = kpage_alloc();
  if (pd)
    memcpy(pd, kernel_pd, PAGE_SIZE);
  return pd;
}

int paging_user_map(u32* pd, u32 va, phys_t page, int write) {
  if (!page || va < USER_BASE || va >= USER_V2_END || !page_aligned(va | page))
    return 0;
  u32* pt = user_table(pd, va);
  if (!pt) {
    if (pd[pde_index(va)] & PAGE_PRESENT)
      return 0;
    pt = kpage_alloc();
    if (!pt)
      return 0;
    pd[pde_index(va)] = make_entry(virt_to_phys(pt), USER_FLAGS | PAGE_WRITE);
  } else if ((pt[pte_index(va)] & PAGE_PRESENT) ||
             (write && !(pd[pde_index(va)] & PAGE_WRITE))) {
    return 0;
  }
  pt[pte_index(va)] = make_entry(page, USER_FLAGS | (write ? PAGE_WRITE : 0));
  if (active(pd))
    paging_activate(pd);
  return 1;
}

phys_t paging_user_unmap(u32* pd, u32 va) {
  if (!page_aligned(va))
    return PHYS_NULL;
  u32* pt = user_table(pd, va);
  if (!pt || !(pt[pte_index(va)] & PAGE_PRESENT))
    return PHYS_NULL;
  u32 pte = pt[pte_index(va)];
  pt[pte_index(va)] = 0;
  u32 i = 0;
  while (i < TABLE_ENTRIES && !(pt[i] & PAGE_PRESENT))
    i++;
  if (i == TABLE_ENTRIES)
    pd[pde_index(va)] = 0;
  /* Invalidate before either the table or data page can be reused. */
  if (active(pd))
    paging_activate(pd);
  if (i == TABLE_ENTRIES)
    kpage_free(pt);
  return entry_address(pte);
}

u32 paging_user_flags(const u32* pd, u32 va) {
  const u32* pt = user_table(pd, va);
  return pt ? pt[pte_index(va)] & pd[pde_index(va)] & (USER_FLAGS | PAGE_WRITE) : 0;
}

u32 paging_user_count(const u32* pd) {
  u32 count = 0;
  for (u32 pde = USER_FIRST_PDE; pde < USER_END_PDE; pde++) {
    const u32* pt = user_table(pd, pde * TABLE_BYTES);
    if (!pt)
      continue;
    for (u32 i = 0; i < TABLE_ENTRIES; i++)
      if (pt[i] & PAGE_PRESENT)
        count++;
  }
  return count;
}

u32 paging_user_find_gap(const u32* pd, u32 start, u32 end, u32 bytes) {
  if (start < USER_DYNAMIC_BASE || end > USER_DYNAMIC_END || start >= end ||
      !bytes || bytes > end - start || !page_aligned(start | end | bytes))
    return 0;
  u32 gap = start;
  for (u32 va = start; va < end;) {
    const u32* pt = user_table(pd, va);
    if (pt) {
      if (pt[pte_index(va)] & PAGE_PRESENT)
        gap = va + PAGE_SIZE;
      va += PAGE_SIZE;
    } else {
      u32 next = (pde_index(va) + 1) * TABLE_BYTES;
      va = next < end ? next : end;
    }
    if (va - gap >= bytes)
      return gap;
  }
  return 0;
}

void paging_user_destroy(u32* pd) {
  if (active(pd))
    panic("Destroying active user page directory", virt_to_phys(pd), 0);
  for (u32 pde = USER_FIRST_PDE; pde < USER_END_PDE; pde++) {
    u32* pt = user_table(pd, pde * TABLE_BYTES);
    if (!pt)
      continue;
    pd[pde] = 0;
    for (u32 i = 0; i < TABLE_ENTRIES; i++)
      if (pt[i] & PAGE_PRESENT)
        page_free(entry_address(pt[i]));
    kpage_free(pt);
  }
  kpage_free(pd);
}
