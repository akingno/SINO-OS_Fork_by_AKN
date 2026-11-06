#include <stdio.h>
#include <string.h>
#include "../../src32/types.h"
#define SINO_KERNEL_H
/* Use the host libc declarations from <string.h>. */
#define SINO_KSTRING_H
static unsigned char output[128];
static u32 used;
static void outb(u16 port, u8 byte) {
  if (port == 0xe9 && used < sizeof(output))
    output[used++] = byte;
}
#include "../../src32/klog.c"
int main(void) {
  const unsigned char bytes[] = {'a', 0, 'b'};
  klog_write_bytes(bytes, 3);
  printf("CHECK\tklog.explicit-length\t%d\n",
         used == 3 && !memcmp(output, bytes, 3));
  used = 0;
  pr_debug_hex(0x12abcdef);
  printf("CHECK\tklog.hex-compatibility\t%d\n",
         used == 10 && !memcmp(output, "0x12ABCDEF", 10));
  used = 0;
  pr_debug("ready");
  printf("CHECK\tklog.no-prefix-or-newline\t%d\n",
         used == 5 && !memcmp(output, "ready", 5));
  return 0;
}
