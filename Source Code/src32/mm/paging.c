#include "../kernel.h"
#include "../api.h"
#include "../klog.h"
#include "page.h"
#include "paging.h"
#include "../lib/kstring.h"
#include "../panic.h"

#define PAGE_BYTES 4096u
#define PAGE_MASK (~(PAGE_BYTES - 1))
#define TABLE_ENTRIES 1024u
#define TABLE_BYTES (PAGE_BYTES * TABLE_ENTRIES)
#define USER_FLAGS (PAGE_PRESENT | PAGE_USER)
#define USER_FIRST_PDE (USER_BASE >> 22)
#define USER_END_PDE ((USER_V2_END + TABLE_BYTES - 1) / TABLE_BYTES)
#define TEMP_PD ((u32*)0x90000u)
#define TEMP_PT ((u32*)0x1e0000u)
#define VBE_TABLES 3u
#define VBE_PAGE_FLAGS (PAGE_PRESENT | PAGE_WRITE | 0x18u)

_Static_assert(BOOT_POOL_START % TABLE_BYTES == 0 &&
               BOOT_IDENTITY_BYTES % TABLE_BYTES == 0 &&
               BOOT_POOL_START < BOOT_IDENTITY_BYTES,
               "initial pool is aligned to whole page tables");
_Static_assert(1 + (BOOT_IDENTITY_BYTES - BOOT_POOL_START) / TABLE_BYTES <= 16,
               "temporary page tables fit the low boot reserve");

static u32* kernel_pd;

void paging_activate(const u32* pd) {
  __asm__ volatile("mov %0,%%cr3" :: "r"(pd) : "memory");
}

static void identity_table(u32* pd, u32 index, u32* pt, u32 flags) {
  u32 base = index * TABLE_BYTES;
  for (u32 i = 0; i < TABLE_ENTRIES; i++)
    pt[i] = (base + i * PAGE_BYTES) | flags;
  if (!index)
    pt[0] = 0;
  pd[index] = (u32)pt | PAGE_PRESENT | PAGE_WRITE;
}

void paging_bootstrap(void) {
  memset(TEMP_PD, 0, PAGE_BYTES);
  /* Entry code, kernel BSS, stack and BIOS handoff all fit in the first 4 MiB. */
  identity_table(TEMP_PD, 0, TEMP_PT, PAGE_PRESENT | PAGE_WRITE);
  u32* pt = TEMP_PT + TABLE_ENTRIES;
  for (u32 i = BOOT_POOL_START / TABLE_BYTES; i < BOOT_IDENTITY_BYTES / TABLE_BYTES; i++) {
    identity_table(TEMP_PD, i, pt, PAGE_PRESENT | PAGE_WRITE);
    pt += TABLE_ENTRIES;
  }
  paging_activate(TEMP_PD);
  u32 cr0;
  __asm__ volatile("mov %%cr0,%0" : "=r"(cr0));
  cr0 = (cr0 & ~0x60000000u) | 0x80010000u;
  __asm__ volatile("mov %0,%%cr0" :: "r"(cr0) : "memory");
  pr_debug("PAGING bootstrap CR3=");
  pr_debug_hex((u32)TEMP_PD);
  pr_debug("\n");
}

void paging_kernel_init(u32 identity_bytes, u32 framebuffer) {
  if (identity_bytes < BOOT_IDENTITY_BYTES || identity_bytes > USER_BASE ||
      identity_bytes % TABLE_BYTES)
    panic("Invalid kernel identity span", identity_bytes, 0);
  u32 identity_count = identity_bytes / TABLE_BYTES;
  u32 vbe = framebuffer / TABLE_BYTES;
  if (vbe < identity_count || vbe > TABLE_ENTRIES - VBE_TABLES ||
      (vbe < USER_END_PDE && vbe + VBE_TABLES > USER_FIRST_PDE))
    panic("Invalid VBE framebuffer range", framebuffer, 0);

  u32* pd = page_alloc();
  if (!pd)
    panic("No memory for kernel page directory", identity_bytes, free_pages);
  for (u32 i = 0; i < identity_count; i++) {
    u32* pt = page_alloc();
    if (!pt)
      panic("No memory for kernel page table", i, free_pages);
    identity_table(pd, i, pt, PAGE_PRESENT | PAGE_WRITE);
    if (i >= BOOT_IDENTITY_BYTES / TABLE_BYTES)
      for (u32 slot = 0; slot < TABLE_ENTRIES; slot++)
        if (!page_boot_usable(i * TABLE_BYTES + slot * PAGE_BYTES))
          pt[slot] = 0;
  }
  for (u32 i = 0; i < VBE_TABLES; i++) {
    u32* pt = page_alloc();
    if (!pt)
      panic("No memory for VBE page table", vbe + i, free_pages);
    identity_table(pd, vbe + i, pt, VBE_PAGE_FLAGS);
  }
  /* These pages are permanently owned by the kernel directory. User page
     directories share the supervisor entries and free only private tables. */
  paging_activate(pd);
  kernel_pd = pd;
  u32 cr0;
  __asm__ volatile("mov %%cr0,%0" : "=r"(cr0));
  pr_debug("PAGING enabled: CR0=");
  pr_debug_hex(cr0);
  pr_debug(" CR3=");
  pr_debug_hex((u32)pd);
  pr_debug(", null page unmapped\n");
  pr_debug("VBE framebuffer mapped: ");
  pr_debug_hex(framebuffer);
  pr_debug(" 1024x768x32\n");
}

void paging_activate_kernel(void) { paging_activate(kernel_pd); }

static int active(const u32* pd) {
  u32 cr3;
  __asm__ volatile("mov %%cr3,%0" : "=r"(cr3) :: "memory");
  return (cr3 & PAGE_MASK) == (u32)pd;
}

static u32* user_table(const u32* pd, u32 va) {
  if (va < USER_BASE || va >= USER_V2_END)
    return 0;
  u32 pde = pd[va >> 22];
  if ((pde & USER_FLAGS) != USER_FLAGS)
    return 0;
  return (u32*)(pde & PAGE_MASK);
}

static u32 slot(u32 va) { return (va >> 12) & (TABLE_ENTRIES - 1); }

u32* paging_user_create(void) {
  for (u32 i = USER_FIRST_PDE; i < USER_END_PDE; i++)
    if (kernel_pd[i] & PAGE_PRESENT)
      return 0;
  u32* pd = page_alloc();
  if (pd)
    memcpy(pd, kernel_pd, PAGE_BYTES);
  return pd;
}

int paging_user_map(u32* pd, u32 va, void* page, int write) {
  if (!page || va < USER_BASE || va >= USER_V2_END ||
      ((va | (u32)page) & ~PAGE_MASK))
    return 0;
  u32* pt = user_table(pd, va);
  if (!pt) {
    if (pd[va >> 22] & PAGE_PRESENT)
      return 0;
    pt = page_alloc();
    if (!pt)
      return 0;
    pd[va >> 22] = (u32)pt | USER_FLAGS | PAGE_WRITE;
  } else if ((pt[slot(va)] & PAGE_PRESENT) ||
             (write && !(pd[va >> 22] & PAGE_WRITE))) {
    return 0;
  }
  pt[slot(va)] = (u32)page | USER_FLAGS | (write ? PAGE_WRITE : 0);
  if (active(pd))
    paging_activate(pd);
  return 1;
}

void* paging_user_unmap(u32* pd, u32 va) {
  if (va & ~PAGE_MASK)
    return 0;
  u32* pt = user_table(pd, va);
  if (!pt || !(pt[slot(va)] & PAGE_PRESENT))
    return 0;
  u32 pte = pt[slot(va)];
  pt[slot(va)] = 0;
  u32 i = 0;
  while (i < TABLE_ENTRIES && !(pt[i] & PAGE_PRESENT))
    i++;
  if (i == TABLE_ENTRIES)
    pd[va >> 22] = 0;
  /* Invalidate before either the table or data page can be reused. */
  if (active(pd))
    paging_activate(pd);
  if (i == TABLE_ENTRIES)
    page_free(pt);
  return (void*)(pte & PAGE_MASK);
}

u32 paging_user_flags(const u32* pd, u32 va) {
  const u32* pt = user_table(pd, va);
  return pt ? pt[slot(va)] & pd[va >> 22] & (USER_FLAGS | PAGE_WRITE) : 0;
}

u32 paging_user_count(const u32* pd) {
  u32 count = 0;
  for (u32 pde = USER_FIRST_PDE; pde < USER_END_PDE; pde++) {
    const u32* pt = user_table(pd, pde << 22);
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
      !bytes || bytes > end - start || ((start | end | bytes) & ~PAGE_MASK))
    return 0;
  u32 gap = start;
  for (u32 va = start; va < end;) {
    const u32* pt = user_table(pd, va);
    if (pt) {
      if (pt[slot(va)] & PAGE_PRESENT)
        gap = va + PAGE_BYTES;
      va += PAGE_BYTES;
    } else {
      u32 next = ((va >> 22) + 1) << 22;
      va = next < end ? next : end;
    }
    if (va - gap >= bytes)
      return gap;
  }
  return 0;
}

void paging_user_destroy(u32* pd) {
  if (active(pd))
    panic("Destroying active user page directory", (u32)pd, 0);
  for (u32 pde = USER_FIRST_PDE; pde < USER_END_PDE; pde++) {
    u32* pt = user_table(pd, pde << 22);
    if (!pt)
      continue;
    pd[pde] = 0;
    for (u32 i = 0; i < TABLE_ENTRIES; i++)
      if (pt[i] & PAGE_PRESENT)
        page_free((void*)(pt[i] & PAGE_MASK));
    page_free(pt);
  }
  page_free(pd);
}
