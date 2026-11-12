#include "kernel.h"
#include "klog.h"
#include "lib/kstring.h"
void klog_write_bytes(const void* data, u32 length) {
  const u8* p = (const u8*)data;
  while (length--)
    outb(0xe9, *p++);
}
void pr_debug(const char* s) {
  klog_write_bytes(s, strlen(s));
}
void pr_debug_hex(u32 v) {
  const char* h = "0123456789ABCDEF";
  pr_debug("0x");
  for (int n = 28; n >= 0; n -= 4)
    outb(0xe9, h[(v >> n) & 15]);
}
