#include "kmalloc.h"
#include "addr.h"
#include "page.h"
#include "paging.h"
#include "slab.h"
#include "../api.h"
#include "../klog.h"
#include "../lib/kstring.h"

namespace {
constexpr u32 MaxBytes = mm::MaxPageRun * PAGE_SIZE;

bool release_allocation(void* pointer) {
  if (mm::SlabCache* cache = mm::slab_cache_for(pointer)) {
    return mm::slab_free(*cache, pointer);
  }
  return mm::heap_page_free(pointer);
}

void check_size(u32 size, u32 alignment, u32 pages) {
  const u32 before = free_pages;
  auto* memory = static_cast<u8*>(kmalloc(size));
  mm::SlabCache* expected = alignment < PAGE_SIZE ? mm::slab_cache_create(alignment) : nullptr;
  if (!memory || (virt_to_phys(memory) & (alignment - 1)) ||
      mm::slab_cache_for(memory) != expected || free_pages + pages != before) {
    panic("KMALLOC size or alignment", size, free_pages);
  }
  memory[0] = 0x25;
  memory[size - 1] = 0x52;
  kfree(memory);
  if (free_pages != before) {
    panic("KMALLOC size release", size, free_pages);
  }
}

void check_release() {
  const u32 before = free_pages;
  auto* block = static_cast<u8*>(kmalloc(2 * PAGE_SIZE + 1));
  auto* ordinary = static_cast<u8*>(kpage_alloc());
  auto* small = static_cast<u8*>(kmalloc(16));
  auto* live = static_cast<u8*>(kmalloc(16));
  if (!block || !ordinary || !small || !live || free_pages + 6 != before) {
    panic("KMALLOC ownership setup", free_pages, before);
  }

  KernelGuard guard{};
  if (kpage_free_run(block, 4) || kpage_free(block + PAGE_SIZE) ||
      page_free(virt_to_phys(block)) || paging_kernel_guard(block, &guard) != S_INVALID ||
      paging_kernel_guard(block + PAGE_SIZE, &guard) != S_INVALID || guard.id ||
      release_allocation(block + 1) || release_allocation(block + PAGE_SIZE) ||
      release_allocation(ordinary) || release_allocation(small + 1) ||
      release_allocation(reinterpret_cast<void*>(0xfffff000u)) ||
      free_pages + 6 != before) {
    panic("KMALLOC ownership rejection", free_pages, before);
  }

  ordinary[0] = 0x4b;
  live[0] = 0x3d;
  kfree(small);
  if (release_allocation(small) || live[0] != 0x3d) {
    panic("KMALLOC duplicate slab release", virt_to_phys(small), free_pages);
  }
  kfree(block);
  if (release_allocation(block) || ordinary[0] != 0x4b || free_pages + 2 != before) {
    panic("KMALLOC duplicate heap release", virt_to_phys(block), free_pages);
  }
  kfree(live);
  if (!kpage_free(ordinary) || free_pages != before) {
    panic("KMALLOC ownership teardown", free_pages, before);
  }
}

void check_zeroing(u32 size) {
  const u32 before = free_pages;
  void* live = kmalloc(size);
  void* dirty = kmalloc(size);
  if (!live || !dirty) {
    panic("KZALLOC test setup", size, free_pages);
  }
  memset(dirty, 0xa5, size);
  kfree(dirty);

  auto* zeroed = static_cast<u8*>(kzalloc(size));
  if (!zeroed || (size == 65 && zeroed != dirty)) {
    panic("KZALLOC allocation or reuse", size, virt_to_phys(zeroed));
  }
  for (u32 i = 0; i < size; ++i) {
    if (zeroed[i]) {
      panic("KZALLOC did not zero the request", size, i);
    }
  }
  kfree(zeroed);
  kfree(live);
  if (free_pages != before) {
    panic("KZALLOC teardown", free_pages, before);
  }
}
}

void* kmalloc(u32 size) {
  if (!size || size > MaxBytes) {
    return nullptr;
  }
  if (mm::SlabCache* cache = mm::slab_cache_for_size(size)) {
    return mm::slab_alloc(*cache);
  }
  return mm::heap_page_alloc((size - 1) / PAGE_SIZE + 1);
}

void* kzalloc(u32 size) {
  void* pointer = kmalloc(size);
  if (pointer) {
    memset(pointer, 0, size);
  }
  return pointer;
}

void kfree(void* pointer) {
  if (pointer && !release_allocation(pointer)) {
    panic("KFREE invalid allocation", virt_to_phys(pointer), 0);
  }
}

void mm::kmalloc_selftest() {
  const u32 before = free_pages;
  kfree(nullptr);
  if (kmalloc(0) || kzalloc(0) || kmalloc(MaxBytes + 1) || kzalloc(0xffffffffu) ||
      free_pages != before) {
    panic("KMALLOC size limits", free_pages, before);
  }

  for (u32 size = 8; size <= 1024; size <<= 1) {
    check_size(size == 8 ? 1 : size / 2 + 1, size, 1);
    check_size(size, size, 1);
  }
  check_size(1025, PAGE_SIZE, 1);
  check_size(PAGE_SIZE, PAGE_SIZE, 1);
  check_size(PAGE_SIZE + 1, 2 * PAGE_SIZE, 2);
  check_size(2 * PAGE_SIZE + 1, 4 * PAGE_SIZE, 4);
  // The largest request can legitimately fail on a fragmented boot pool.
  if (void* max = kmalloc(MaxBytes)) {
    if (free_pages + MaxPageRun != before || virt_to_phys(max) % MaxBytes) {
      panic("KMALLOC maximum block", free_pages, before);
    }
    kfree(max);
  }

  check_release();
  check_zeroing(65);
  check_zeroing(2 * PAGE_SIZE + 1);
  if (free_pages != before) {
    panic("KMALLOC allocator totals", free_pages, before);
  }
  pr_debug("KMALLOC allocator self-check passed\n");
}
