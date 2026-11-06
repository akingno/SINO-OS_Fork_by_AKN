#include "../kernel.h"
#include "paging.h"

#define PAGE_BYTES 4096u
#define PAGE_MASK (~(PAGE_BYTES - 1))
#define TABLE_ENTRIES 1024u
#define USER_FLAGS (PAGE_PRESENT | PAGE_USER)

void paging_activate(const u32* pd) {
  __asm__ volatile("mov %0,%%cr3" :: "r"(pd) : "memory");
}

static int active(const u32* pd) {
  u32 cr3;
  __asm__ volatile("mov %%cr3,%0" : "=r"(cr3) :: "memory");
  return (cr3 & PAGE_MASK) == (u32)pd;
}

static u32* user_table(const u32* pd, u32 va) {
  u32 pde = pd[va >> 22];
  if ((pde & USER_FLAGS) != USER_FLAGS)
    return 0;
  return (u32*)(pde & PAGE_MASK);
}

static u32 slot(u32 va) { return (va >> 12) & (TABLE_ENTRIES - 1); }

u32* paging_user_create(const u32* kernel_pd, u32 user_base) {
  u32 index = user_base >> 22;
  if (kernel_pd[index] & PAGE_PRESENT)
    return 0;
  u32* pd = page_alloc();
  if (!pd)
    return 0;
  u32* pt = page_alloc();
  if (!pt) {
    page_free(pd);
    return 0;
  }
  memcpy(pd, kernel_pd, PAGE_BYTES);
  pd[index] = (u32)pt | USER_FLAGS | PAGE_WRITE;
  return pd;
}

int paging_user_map(u32* pd, u32 va, void* page, int write) {
  if (!page || ((va | (u32)page) & ~PAGE_MASK))
    return 0;
  u32* pt = user_table(pd, va);
  if (!pt || (pt[slot(va)] & PAGE_PRESENT) ||
      (write && !(pd[va >> 22] & PAGE_WRITE)))
    return 0;
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
  /* Invalidate before the caller can return the old page to the allocator. */
  if (active(pd))
    paging_activate(pd);
  return (void*)(pte & PAGE_MASK);
}

u32 paging_user_flags(const u32* pd, u32 va) {
  const u32* pt = user_table(pd, va);
  return pt ? pt[slot(va)] & pd[va >> 22] & (USER_FLAGS | PAGE_WRITE) : 0;
}

u32 paging_user_count(const u32* pd, u32 user_base) {
  const u32* pt = user_table(pd, user_base);
  u32 count = 0;
  if (pt)
    for (u32 i = 0; i < TABLE_ENTRIES; i++)
      if (pt[i] & PAGE_PRESENT)
        count++;
  return count;
}

void paging_user_destroy(u32* pd, u32 user_base) {
  if (active(pd))
    panic("Destroying active user page directory", (u32)pd, user_base);
  u32* pt = user_table(pd, user_base);
  if (pt) {
    pd[user_base >> 22] = 0;
    for (u32 i = 0; i < TABLE_ENTRIES; i++)
      if (pt[i] & PAGE_PRESENT)
        page_free((void*)(pt[i] & PAGE_MASK));
    page_free(pt);
  }
  page_free(pd);
}
