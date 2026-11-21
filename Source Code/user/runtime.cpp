#include "sdk.h"
#include "heap.h"

namespace {

sino::heap::MapResult mapHeapRegion(void*, u32 bytes) {
  const int result = AnonMap(bytes);
  if (result <= 0) { return {nullptr, result}; }
  return {(void*)(u32)result, S_OK};
}

int unmapHeapRegion(void*, void* base) { return AnonUnmap(base); }

int setHeapRegionPurgeable(void*, void* base, bool enabled) {
  return AnonSetPurgeable(base, enabled);
}

void fatalHeapUnmap(void*, void*, int status) {
  Log("USER HEAP: failed to unmap owned region\n");
  Exit(status);
}

sino::heap::Heap processHeap({nullptr, mapHeapRegion, unmapHeapRegion,
                             setHeapRegionPurgeable, fatalHeapUnmap});

struct HeapLock {
  u32 held = 0;

  void lock() {
    while (__atomic_exchange_n(&held, 1u, __ATOMIC_ACQUIRE)) { Yield(0); }
  }

  void unlock() { __atomic_store_n(&held, 0u, __ATOMIC_RELEASE); }
};

HeapLock heapLock;

} // namespace

/* GCC's MinGW C entry hook: no host CRT or C++ constructors in native ABI 1. */
extern "C" void __main(void) {
}
extern "C" __attribute__((cdecl,noreturn)) void sino_thread_start(int (*entry)(void*), void* argument) {
  sino::thread::exit(entry(argument));
}
void *malloc(u32 bytes) {
  heapLock.lock();
  void* pointer = processHeap.allocate(bytes);
  heapLock.unlock();
  return pointer;
}
void free(void *pointer) {
  if (!pointer) { return; }
  heapLock.lock();
  processHeap.release(pointer);
  heapLock.unlock();
}
void *calloc(u32 count, u32 bytes) {
  if (!bytes || count > (~0u) / bytes) { return nullptr; }
  const u32 total = count * bytes;
  void* pointer = malloc(total);
  if (pointer) { memset(pointer, 0, total); }
  return pointer;
}
void *realloc(void *pointer, u32 bytes) {
  if (!pointer) { return malloc(bytes); }
  if (!bytes) {
    free(pointer);
    return nullptr;
  }

  heapLock.lock();
  const u32 capacity = processHeap.usableSize(pointer);
  void* replacement = nullptr;
  if (capacity) {
    replacement = bytes <= capacity ? pointer : processHeap.allocate(bytes);
  }
  heapLock.unlock();

  // Both allocations stay live while copying. The caller owns the old block.
  if (replacement && replacement != pointer) {
    memcpy(replacement, pointer, capacity);
    free(pointer);
  }
  return replacement;
}
u32 malloc_size(const void *pointer) {
  heapLock.lock();
  const u32 capacity = processHeap.usableSize(pointer);
  heapLock.unlock();
  return capacity;
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
