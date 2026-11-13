#include "irq.h"
#include "../klog.h"
#include "../panic.h"

namespace sync {
IrqState readState() {
  IrqState state;
  __asm__ volatile("pushfl; popl %0" : "=r"(state) :: "memory");
  return state;
}
IrqState saveAndDisable() {
  IrqState state;
  __asm__ volatile("pushfl; popl %0; cli" : "=r"(state) :: "memory");
  return state;
}
void restore(IrqState state) {
  if (state & InterruptEnabled) {
    __asm__ volatile("sti" ::: "memory");
  } else {
    __asm__ volatile("cli" ::: "memory");
  }
}

// 在设备初始化完成、IF 已开启的主循环入口调用，不提前改变启动顺序。
void selfTest() {
  if (!(readState() & InterruptEnabled)) {
    panic("IRQ guard test requires enabled IRQs", 0, 0);
  }
  {
    IrqGuard outer;
    if (readState() & InterruptEnabled) {
      panic("IRQ guard did not disable IRQs", 0, 0);
    }
    {
      IrqGuard inner;
      if (readState() & InterruptEnabled) {
        panic("Nested IRQ guard enabled IRQs", 0, 0);
      }
    }
    if (readState() & InterruptEnabled) {
      panic("Nested IRQ guard lost original IF", 0, 0);
    }
  }
  if (!(readState() & InterruptEnabled)) {
    panic("IRQ guard did not restore IF", 0, 0);
  }
  pr_debug("SYNC IRQ guard self-check passed\n");
}
}
