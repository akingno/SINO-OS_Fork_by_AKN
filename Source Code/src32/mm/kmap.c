#include "../x86.h"
#include "../klog.h"
#include "../panic.h"
#include "paging.h"
#include "page.h"
#include "layout.h"

static u32* kmap_table;
static phys_t kmap_slot[KMAP_SLOTS];

void kmap_init(u32* table) {
  kmap_table = table;
}

void* kmap(phys_t page) {
  if (!page || !page_aligned(page))
    panic("kmap: invalid page", page, 0);
#ifndef SINO_KMAP_FORCE_SLOT
  if (page < direct_map_end)
    return phys_to_virt(page);
#endif
  for (u32 i = 0; i < KMAP_SLOTS; i++) {
    if (!kmap_slot[i]) {
      kmap_slot[i] = page;
      u32 va = KMAP_BASE + i * PAGE_SIZE;
      kmap_table[i] = make_entry(page, PAGE_PRESENT | PAGE_WRITE);
      invlpg(va);
      return (void*)va;
    }
  }
  panic("kmap: all slots full", page, KMAP_SLOTS);
  return 0;
}

void kunmap(void* pointer) {
  u32 addr = (u32)pointer;
  if (addr < KMAP_BASE || addr >= KMAP_BASE + KMAP_SLOTS * PAGE_SIZE)
    return;
  u32 i = (addr - KMAP_BASE) / PAGE_SIZE;
  if (!kmap_slot[i])
    panic("kunmap: double free", addr, i);
  kmap_slot[i] = 0;
  kmap_table[i] = 0;
  invlpg(addr);
}

u32 kmap_free_slots(void) {
  u32 count = 0;
  for (u32 i = 0; i < KMAP_SLOTS; i++)
    if (!kmap_slot[i])
      count++;
  return count;
}

void kmap_selftest(void) {
  u32 before = free_pages;
  phys_t pages[KMAP_SLOTS];
  for (u32 i = 0; i < KMAP_SLOTS; i++) {
    pages[i] = page_alloc();
    if (!pages[i])
      panic("kmap_selftest: alloc", i, free_pages);
    void* mapped = kmap(pages[i]);
    *(volatile u32*)mapped = 0xCAFE0000u | i;
    kunmap(mapped);
  }
  for (u32 i = 0; i < KMAP_SLOTS; i++) {
    void* remapped = kmap(pages[i]);
    u32 val = *(volatile u32*)remapped;
    if (val != (0xCAFE0000u | i))
      panic("kmap_selftest: verify", val, 0xCAFE0000u | i);
    kunmap(remapped);
    page_free(pages[i]);
  }
  if (free_pages != before)
    panic("kmap_selftest: leak", free_pages, before);
  pr_debug("KMAP self-check passed\n");
}
