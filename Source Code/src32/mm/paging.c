#include "../kernel.h"
#include "../api.h"
#include "../klog.h"
#include "page.h"
#include "paging.h"
#include "kmap.h"
#include "layout.h"
#include "../lib/kstring.h"
#include "../panic.h"

#define USER_FLAGS (PAGE_PRESENT | PAGE_USER)
#define USER_FIRST_PDE (USER_BASE / TABLE_BYTES)
#define USER_END_PDE ((USER_V2_END + TABLE_BYTES - 1) / TABLE_BYTES)

_Static_assert(BOOT_POOL_START % TABLE_BYTES == 0 &&
               BOOT_IDENTITY_BYTES % TABLE_BYTES == 0 &&
               BOOT_POOL_START < BOOT_IDENTITY_BYTES,
               "initial pool is aligned to whole page tables");
_Static_assert(1 + (BOOT_IDENTITY_BYTES - BOOT_POOL_START) / TABLE_BYTES <= 16,
               "temporary page tables fit the low boot reserve");
_Static_assert(KMAP_PDE_INDEX * TABLE_BYTES == KMAP_BASE,
               "KMAP_BASE matches its PDE index");
_Static_assert(KMAP_PDE_INDEX >= USER_END_PDE,
               "KMAP PDE does not overlap user area");

u32 direct_map_end;
static u32* kernel_pd;
static u32 kernel_pde_snapshot[TABLE_ENTRIES];
static int kernel_pdes_frozen;

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
  u32* pd = phys_to_virt(BOOT_PD);
  u32* pt = phys_to_virt(BOOT_PT);
  memset(pd, 0, PAGE_SIZE);
  identity_table(pd, 0, pt, PAGE_PRESENT | PAGE_WRITE);
  for (u32 i = BOOT_POOL_START / TABLE_BYTES; i < BOOT_IDENTITY_BYTES / TABLE_BYTES; i++) {
    pt += TABLE_ENTRIES;
    identity_table(pd, i, pt, PAGE_PRESENT | PAGE_WRITE);
  }
  write_cr3(BOOT_PD);
  write_cr0((read_cr0() & ~(CR0_CD | CR0_NW)) | CR0_PG | CR0_WP);
  pr_debug("PAGING bootstrap CR3=");
  pr_debug_hex(BOOT_PD);
  pr_debug("\n");
}

void paging_kernel_init(void) {
  u32 dm_pages = (nr_pages + TABLE_ENTRIES - 1) & ~(TABLE_ENTRIES - 1);
  if (dm_pages < BOOT_IDENTITY_BYTES / PAGE_SIZE)
    dm_pages = BOOT_IDENTITY_BYTES / PAGE_SIZE;
  if (dm_pages > USER_BASE / PAGE_SIZE)
    dm_pages = USER_BASE / PAGE_SIZE;
  u32 dm_end = dm_pages * PAGE_SIZE;
  u32 identity_count = dm_end / TABLE_BYTES;

  u32* pd = kpage_alloc();
  if (!pd)
    panic("No memory for kernel page directory", dm_end, free_pages);
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
  u32* pt0 = phys_to_virt(entry_address(pd[0]));
  for (u32 pte = 0xA0000 / PAGE_SIZE; pte <= 0xFF000 / PAGE_SIZE; pte++)
    if (pte != VGA_TEXT_BASE / PAGE_SIZE)
      pt0[pte] = make_entry(pte * PAGE_SIZE, PAGE_PRESENT);

  u32* kmap_pt = kpage_alloc();
  if (!kmap_pt)
    panic("No memory for KMAP page table", KMAP_PDE_INDEX, free_pages);
  pd[KMAP_PDE_INDEX] = make_entry(virt_to_phys(kmap_pt), PAGE_PRESENT | PAGE_WRITE);

  paging_activate(pd);
  kmap_init(kmap_pt);
  kernel_pd = pd;
  direct_map_end = dm_end;
  memcpy(kernel_pde_snapshot, pd, PAGE_SIZE);
  kernel_pdes_frozen = 1;

  pr_debug("PAGING enabled: CR0=");
  pr_debug_hex(read_cr0());
  pr_debug(" CR3=");
  pr_debug_hex(virt_to_phys(pd));
  pr_debug(", null page unmapped\n");
  pr_debug("Direct mapping: [0, ");
  pr_debug_hex(dm_end);
  pr_debug("), KMAP at PDE ");
  pr_debug_hex(KMAP_PDE_INDEX);
  pr_debug("\n");
  pr_debug("VGA text console: 80x25\n");
}

void paging_activate_kernel(void) { paging_activate(kernel_pd); }

void paging_kernel_protect(u32 va) {
  if (!page_aligned(va) || va >= direct_map_end)
    panic("paging_kernel_protect: bad va", va, direct_map_end);
  u32 pde = kernel_pd[pde_index(va)];
  if (!(pde & PAGE_PRESENT))
    panic("paging_kernel_protect: no PDE", va, pde);
  u32* pt = phys_to_virt(entry_address(pde));
  pt[pte_index(va)] = 0;
  invlpg(va);
}

void paging_kernel_unprotect(u32 va) {
  if (!page_aligned(va) || va >= direct_map_end)
    panic("paging_kernel_unprotect: bad va", va, direct_map_end);
  u32 pde = kernel_pd[pde_index(va)];
  if (!(pde & PAGE_PRESENT))
    panic("paging_kernel_unprotect: no PDE", va, pde);
  u32* pt = phys_to_virt(entry_address(pde));
  pt[pte_index(va)] = make_entry(virt_to_phys((void*)va), PAGE_PRESENT | PAGE_WRITE);
  invlpg(va);
}

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

/* CPU sets Accessed (0x20) and Dirty (0x40) on live page directory entries;
 * mask them out so the freeze check compares only software-managed bits. */
#define PDE_HW_BITS 0x60u
u32* paging_user_create(void) {
  if (kernel_pdes_frozen)
    for (u32 i = 0; i < TABLE_ENTRIES; i++) {
      if (i >= USER_FIRST_PDE && i < USER_END_PDE) continue;
      if ((kernel_pd[i] & ~PDE_HW_BITS) != (kernel_pde_snapshot[i] & ~PDE_HW_BITS))
        panic("Kernel PDE changed after init", i, kernel_pd[i]);
    }
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
  /* No TLB invalidation: both the new PTE and any new PDE are 0→1
   * transitions, and x86 never caches non-present entries (SDM 4.10.4.3). */
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
  if (i == TABLE_ENTRIES) {
    pd[pde_index(va)] = 0;
    /* Flush the entire TLB: the paging-structure cache may still reference
     * this page table, so a single invlpg is insufficient.  Must complete
     * before the table page is freed.  If CR4.PGE is ever enabled, the
     * no-global-pages invariant that exempts inactive PDs must be revisited. */
    if (active(pd))
      paging_activate(pd);
    kpage_free(pt);
  } else {
    /* Only the data PTE changed (1→0); invlpg suffices.  Inactive PDs need
     * no invalidation: every CR3 switch flushes the TLB (no global pages). */
    if (active(pd))
      invlpg(va);
  }
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

void paging_protect_selftest(void) {
  u32 saved_free = free_pages;
  void* run = kpage_alloc_run(2);
  if (!run)
    panic("paging_protect_selftest: alloc", 0, free_pages);
  paging_kernel_protect((u32)run);

  u32 fc = fault_count;
  __asm__ volatile(
    "movl $1f, %[res]\n\t"
    "movl $14, %[exp]\n\t"
    "movl $0xDEAD, (%[a])\n\t"
    "1:"
    : [res] "=m"(fault_resume), [exp] "=m"(fault_expected)
    : [a] "r"(run) : "memory"
  );
  fault_expected = 0;
  fault_resume = 0;
  if (fault_count != fc + 1)
    panic("paging_protect_selftest: guard", fault_count, fc + 1);

  paging_kernel_unprotect((u32)run);
  fc = fault_count;
  __asm__ volatile(
    "movl $1f, %[res]\n\t"
    "movl $14, %[exp]\n\t"
    "movl $0xBEEF, (%[a])\n\t"
    "1:"
    : [res] "=m"(fault_resume), [exp] "=m"(fault_expected)
    : [a] "r"(run) : "memory"
  );
  fault_expected = 0;
  fault_resume = 0;
  if (fault_count != fc)
    panic("paging_protect_selftest: unprotect", fault_count, fc);
  kpage_free_run(run, 2);
  if (free_pages != saved_free)
    panic("paging_protect_selftest: leak", free_pages, saved_free);

  fc = fault_count;
  __asm__ volatile(
    "movl $1f, %[res]\n\t"
    "movl $14, %[exp]\n\t"
    "movl $0xDEAD, (%[a])\n\t"
    "1:"
    : [res] "=m"(fault_resume), [exp] "=m"(fault_expected)
    : [a] "r"((void*)0xF0000) : "memory"
  );
  fault_expected = 0;
  fault_resume = 0;
  if (fault_count != fc + 1)
    panic("paging_protect_selftest: ROM", fault_count, fc + 1);

  volatile u32* vga = (volatile u32*)VGA_TEXT_BASE;
  u32 val = *vga;
  fc = fault_count;
  __asm__ volatile(
    "movl $1f, %[res]\n\t"
    "movl $14, %[exp]\n\t"
    "movl %[v], (%[a])\n\t"
    "1:"
    : [res] "=m"(fault_resume), [exp] "=m"(fault_expected)
    : [a] "r"(vga), [v] "r"(val) : "memory"
  );
  fault_expected = 0;
  fault_resume = 0;
  if (fault_count != fc)
    panic("paging_protect_selftest: VGA", fault_count, fc);

  pr_debug("PAGING protection self-check passed\n");
}
