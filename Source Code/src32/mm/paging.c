#include "../kernel.h"
#include "../api.h"
#include "paging.h"

#define PAGE_BYTES 4096u
#define PAGE_MASK (~(PAGE_BYTES - 1))
#define TABLE_ENTRIES 1024u
#define USER_FLAGS (PAGE_PRESENT | PAGE_USER)
#define USER_FIRST_PDE (USER_BASE >> 22)
#define USER_END_PDE (USER_DYNAMIC_END >> 22)

void paging_activate(const u32* pd) {
  __asm__ volatile("mov %0,%%cr3" :: "r"(pd) : "memory");
}

static int active(const u32* pd) {
  u32 cr3;
  __asm__ volatile("mov %%cr3,%0" : "=r"(cr3) :: "memory");
  return (cr3 & PAGE_MASK) == (u32)pd;
}

static u32* user_table(const u32* pd, u32 va) {
  if (va < USER_BASE || va >= USER_DYNAMIC_END)
    return 0;
  u32 pde = pd[va >> 22];
  if ((pde & USER_FLAGS) != USER_FLAGS)
    return 0;
  return (u32*)(pde & PAGE_MASK);
}

static u32 slot(u32 va) { return (va >> 12) & (TABLE_ENTRIES - 1); }

u32* paging_user_create(const u32* kernel_pd) {
  for (u32 i = USER_FIRST_PDE; i < USER_END_PDE; i++)
    if (kernel_pd[i] & PAGE_PRESENT)
      return 0;
  u32* pd = page_alloc();
  if (pd)
    memcpy(pd, kernel_pd, PAGE_BYTES);
  return pd;
}

int paging_user_map(u32* pd, u32 va, void* page, int write) {
  if (!page || va < USER_BASE || va >= USER_DYNAMIC_END ||
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
