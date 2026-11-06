#include "panic.h"
#include "console.h"
#include "kernel.h"
#include "klog.h"

void panic(const char* s, u32 v, u32 e) {
  cli();
  pr_debug("KERNEL PANIC: ");
  pr_debug(s);
  pr_debug(" ");
  pr_debug_hex(v);
  pr_debug(" ");
  pr_debug_hex(e);
  pr_debug("\n");
  console_panic(s, v, e);
  for (;;)
    halt();
}
