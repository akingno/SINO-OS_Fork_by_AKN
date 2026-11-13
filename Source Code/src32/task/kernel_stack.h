#ifndef SINO_TASK_KERNEL_STACK_H
#define SINO_TASK_KERNEL_STACK_H
#include "../mm/paging.h"

namespace task {
// 显式管理生命周期：不能在正在使用的入口栈上析构/释放自身。
// 零初始化后才能 allocate；活动对象地址必须稳定，KernelGuard 绑定此对象。
class KernelStack {
public:
  static constexpr u32 UsableBytes = 32u * 1024u;
  static constexpr u32 GuardPages = 1u;
  static constexpr u32 UsablePages = UsableBytes / PAGE_SIZE;
  static constexpr u32 TotalPages = GuardPages + UsablePages;

  KernelStack() = default;
  KernelStack(const KernelStack&) = delete;
  KernelStack& operator=(const KernelStack&) = delete;

  int allocate();
  void release();
  u32 bottom() const;
  u32 top() const;
  static void selfTest();

private:
  void* allocation;
  // 实际分配描述独立于可用大小；以后 buddy 返回更大有序块时记录实际页数。
  u32 allocationPages;
  KernelGuard guard;
};
static_assert(KernelStack::UsableBytes % PAGE_SIZE == 0, "whole usable stack pages");
static_assert(KernelStack::TotalPages == 9, "32 KiB stack plus one guard page");
static_assert(__is_trivially_copyable(KernelStack), "zero state needs no runtime construction");
}
#endif
