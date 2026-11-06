#include "kstring.h"

void* memcpy(void* d, const void* s, u32 n) {
  void* result = d;
  u32 words = n / 4, tail = n & 3;
  __asm__ volatile("rep movsl" : "+D"(d), "+S"(s), "+c"(words)::"memory");
  u8* p = d;
  const u8* q = s;
  while (tail--)
    *p++ = *q++;
  return result;
}
void* memset(void* d, int v, u32 n) {
  void* result = d;
  u32 words = n / 4, tail = n & 3, pattern = (u8)v * 0x01010101u;
  __asm__ volatile("rep stosl"
                   : "+D"(d), "+c"(words)
                   : "a"(pattern)
                   : "memory");
  u8* p = d;
  while (tail--)
    *p++ = (u8)v;
  return result;
}
u32 strlen(const char* s) {
  u32 n = 0;
  while (s[n])
    n++;
  return n;
}
int streq(const char* a, const char* b) {
  while (*a && *a == *b) {
    a++;
    b++;
  }
  return *a == *b;
}
void number(u32 v, char* s) {
  char t[11];
  u32 n = 0;
  do {
    t[n++] = (char)('0' + v % 10);
    v /= 10;
  } while (v);
  u32 j = 0;
  while (n)
    s[j++] = t[--n];
  s[j] = 0;
}
