#include "kernel_stack.h"
#include "../mm/page.h"
#include "../x86.h"
#include "../api.h"
#include "../panic.h"
#include "../klog.h"

namespace task {
int KernelStack::allocate() {
  if (allocation || allocationPages || guard.id) {
    return S_INVALID;
  }
  allocation = kpage_alloc_run(TotalPages);
  if (!allocation) {
    return S_NOMEM;
  }
  allocationPages = TotalPages;
  int status = paging_kernel_guard(allocation, &guard);
  if (status != S_OK) {
    release();
    return status;
  }
  return S_OK;
}

void KernelStack::release() {
  if (!allocation) {
    return;
  }
  if (guard.id && paging_kernel_unguard(&guard) != S_OK) {
    panic("Kernel stack guard restore failed", (u32)allocation, guard.id);
  }
  if (!kpage_free_run(allocation, allocationPages)) {
    panic("Kernel stack release failed", (u32)allocation, allocationPages);
  }
  allocation = 0;
  allocationPages = 0;
}

u32 KernelStack::bottom() const {
  return allocation ? (u32)allocation + GuardPages * PAGE_SIZE : 0;
}

u32 KernelStack::top() const {
  return allocation ? bottom() + UsableBytes : 0;
}

namespace {
u32 kernelFlags(const u32* pd, u32 address) {
  u32 pde = pd[pde_index(address)];
  if (!(pde & PAGE_PRESENT) || (pde & PAGE_USER)) {
    panic("Kernel stack PDE invalid", address, pde);
  }
  const u32* table = (const u32*)phys_to_virt(entry_address(pde));
  return table[pte_index(address)];
}
}

// 启动时尚无进程，检验真实分配/保护/释放，并用保护记录耗尽覆盖失败回滚。
// 不访问未映射的保护页：溢出时尚无独立异常栈，不能承诺恢复。
void KernelStack::selfTest() {
  const u32 before = free_pages;
  KernelStack stack{};
  stack.release();
  if (stack.bottom() || stack.top() || stack.allocate() != S_OK ||
      free_pages + TotalPages != before || stack.allocationPages != TotalPages ||
      stack.bottom() != (u32)stack.allocation + GuardPages * PAGE_SIZE ||
      stack.top() != (u32)stack.allocation + TotalPages * PAGE_SIZE ||
      stack.top() - stack.bottom() != UsableBytes) {
    panic("Kernel stack layout", free_pages, before);
  }
  if (stack.allocate() != S_INVALID || free_pages + TotalPages != before) {
    panic("Kernel stack duplicate allocation", free_pages, before);
  }
  u32* pd = paging_user_create();
  const u32* kernel = (const u32*)phys_to_virt(page_floor(read_cr3()));
  if (!pd || kernelFlags(kernel, (u32)stack.allocation) & PAGE_PRESENT ||
      kernelFlags(pd, (u32)stack.allocation) & PAGE_PRESENT) {
    panic("Kernel stack guard mapping", (u32)stack.allocation, 0);
  }
  for (u32 page = 0; page < UsablePages; page++) {
    u32 address = stack.bottom() + page * PAGE_SIZE;
    constexpr u32 flags = PAGE_PRESENT | PAGE_WRITE | PAGE_USER;
    if ((kernelFlags(kernel, address) & flags) != (PAGE_PRESENT | PAGE_WRITE) ||
        (kernelFlags(pd, address) & flags) != (PAGE_PRESENT | PAGE_WRITE)) {
      panic("Kernel stack usable mapping", address, page);
    }
  }
  volatile u32* low = (volatile u32*)stack.bottom();
  volatile u32* high = (volatile u32*)(stack.top() - sizeof(u32));
  *low = 0x13579bdfu;
  *high = 0x2468ace0u;
  if (*low != 0x13579bdfu || *high != 0x2468ace0u) {
    panic("Kernel stack boundary access", *low, *high);
  }
  paging_user_destroy(pd);
  stack.release();
  stack.release();
  if (stack.bottom() || stack.top() || free_pages != before) {
    panic("Kernel stack reclaim", free_pages, before);
  }

  // 32 是此自检的探测上界，不依赖页表模块私有的记录数量；当前上限为 16。
  constexpr u32 ProbePages = 32;
  KernelGuard tokens[ProbePages]{};
  u8* pages = (u8*)kpage_alloc_run(ProbePages);
  if (!pages) {
    panic("Kernel stack guard probe allocation", free_pages, before);
  }
  u32 count = 0;
  for (; count < ProbePages; count++) {
    int status = paging_kernel_guard(pages + count * PAGE_SIZE, &tokens[count]);
    if (status == S_LIMIT) {
      break;
    }
    if (status != S_OK) {
      panic("Kernel stack guard probe", count, (u32)status);
    }
  }
  if (!count || count == ProbePages) {
    panic("Kernel stack guard probe bound", count, ProbePages);
  }
  const u32 guarded = free_pages;
  if (stack.allocate() != S_LIMIT || free_pages != guarded ||
      stack.allocation || stack.allocationPages || stack.guard.id) {
    panic("Kernel stack guard rollback", free_pages, guarded);
  }
  for (u32 i = 0; i < count; i++) {
    if (paging_kernel_unguard(&tokens[i]) != S_OK) {
      panic("Kernel stack guard probe restore", i, count);
    }
  }
  if (!kpage_free_run(pages, ProbePages) || free_pages != before ||
      stack.allocate() != S_OK) {
    panic("Kernel stack retry allocation", free_pages, before);
  }
  stack.release();
  if (free_pages != before) {
    panic("Kernel stack retry reclaim", free_pages, before);
  }
  pr_debug("TASK kernel stack self-check passed (32 KiB usable + 4 KiB guard)\n");
}
}
