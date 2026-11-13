#include "sdk.h"
/* GCC's MinGW C entry hook: no host CRT or C++ constructors in native ABI 1. */
extern "C" void __main(void) {
}
/* 【简化实现：用户态堆】每次 malloc 都直接预留一个匿名区域，首次访问才分配
 * 物理页；小对象也按页占空间，并受每进程 32 个区域、合计 16 MiB 的限制。
 * TODO（用户态堆，后置）：以匿名映射作为堆的底层来源，在区域内按对齐的小块
 * 分配、复用和合并，让多个小对象共享 AnonRegion；大块可单独映射。free 先识别
 * 所属块，不能继续直接 AnonUnmap；多线程共享堆需要同步。这不要求先实现内核
 * buddy/slab，本轮进程与调度改造只保留此 TODO，不改变 malloc/free 的行为。 */
void *malloc(u32 bytes) {
  int base = AnonMap(bytes);
  return base > 0 ? (void*)(u32)base : 0;
}
void free(void *pointer) {
  if (pointer)
    AnonUnmap(pointer);
}
void *memcpy(void *d, const void *s, u32 n) {
  u8 *a = (u8*)d;
  const u8 *b = (const u8*)s;
  while (n--)
    *a++ = *b++;
  return d;
}
void *memset(void *d, int c, u32 n) {
  u8 *a = (u8*)d;
  while (n--)
    *a++ = (u8)c;
  return d;
}
u32 strlen(const char *s) {
  u32 n = 0;
  while (s[n])
    n++;
  return n;
}
int streq(const char *a, const char *b) {
  while (*a && *a == *b) {
    a++;
    b++;
  }
  return *a == *b;
}
void copy(char *a, const char *b) {
  while ((*a++ = *b++)) {
  }
}
void number(u32 v, char *s) {
  char b[12];
  int i = 0;
  do {
    b[i++] = (char)('0' + v % 10);
    v /= 10;
  } while (v);
  while (i)
    *s++ = b[--i];
  *s = 0;
}
void Log(const char *s) {
  api(SYS_LOG, (u32)s, 0, strlen(s));
}
