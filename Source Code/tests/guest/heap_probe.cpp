#include "stream.h"

namespace {

constexpr u32 PageBytes = 4096;
constexpr u32 LargeChunk = 4080;
constexpr u32 SlabSlots = 16;
using SlabBatch = u8*[SlabSlots];

void require(bool condition, const char* detail) {
  if (condition) { return; }
  Log("HEAP SELFTEST FAIL: ");
  Log(detail);
  Log("\n");
  Exit(1);
}

u8* allocate(u32 size) {
  auto* pointer = static_cast<u8*>(malloc(size));
  require(pointer && !((u32)pointer & 15) && malloc_size(pointer) >= size,
          "aligned allocation with sufficient capacity");
  return pointer;
}

void verifyBytes(const u8* pointer, u32 bytes, u8 value) {
  for (u32 i = 0; i < bytes; ++i) { require(pointer[i] == value, "live heap payload changed"); }
}

void* pageBase(const void* pointer) { return (void*)((u32)pointer & ~(PageBytes - 1)); }

void* fillSlab(SlabBatch& batch) {
  for (auto& pointer : batch) {
    pointer = allocate(LargeChunk);
    pointer[0] = 0x5B;
    pointer[LargeChunk - 1] = 0xC2;
  }
  return pageBase(batch[0]);
}

void freeSlab(const SlabBatch& batch) {
  for (auto* pointer : batch) { free(pointer); }
}

void singleThread() {
  require(!malloc(0) && !calloc(0, 32) && !calloc(0x80000000u, 2), "zero and overflow handling");
  free(nullptr);
  constexpr u32 Count = ANON_MAX_REGIONS + 32;
  u8* objects[Count];
  for (u32 i = 0; i < Count; ++i) {
    objects[i] = allocate(32);
    require(malloc_size(objects[i]) == 32, "32-byte size class");
    if (i) { require(objects[i] == objects[i - 1] + 32, "successive slots share the original slab"); }
    memset(objects[i], i, 32);
  }
  constexpr u32 Sizes[] = {1, 33, 65, 129, 257, 497, 1009, 2033};
  u8* mixed[sizeof(Sizes) / sizeof(Sizes[0])];
  for (u32 i = 0; i < sizeof(Sizes) / sizeof(Sizes[0]); ++i) {
    mixed[i] = allocate(Sizes[i]);
    memset(mixed[i], 0x69 + i, Sizes[i]);
  }
  for (u32 i = 0; i < Count; ++i) {
    verifyBytes(objects[i], 32, (u8)i);
    free(objects[i]);
  }
  auto* zeroed = static_cast<u8*>(calloc(4, 8));
  require(zeroed == objects[Count - 1], "calloc reuses dirty hot slots");
  verifyBytes(zeroed, 32, 0);
  free(zeroed);
  for (u32 i = 0; i < sizeof(Sizes) / sizeof(Sizes[0]); ++i) {
    verifyBytes(mixed[i], Sizes[i], (u8)(0x69 + i));
    free(mixed[i]);
  }
}

constexpr u32 Participants = 4;
u32 arrived[Participants];

struct Worker {
  u32 index;
};

int threadWork(void* argument) {
  const u32 index = static_cast<const Worker*>(argument)->index;
  constexpr u32 Sizes[] = {16, 40, 256, 497, 1009, 4080, 4100, 8192};
  u32 random = index + 1;
  for (u32 round = 1; round <= 32; ++round) {
    u8* pointers[12];
    u32 sizes[12];
    const u8 value = (u8)(round + index * 48);
    for (u32 i = 0; i < 12; ++i) {
      random = random * 1664525u + 1013904223u;
      sizes[i] = Sizes[(random >> 16) % (sizeof(Sizes) / sizeof(Sizes[0]))];
      pointers[i] = allocate(sizes[i]);
      memset(pointers[i], value + i, sizes[i]);
      if ((i & 3) == 0) { Yield(0); }
    }
    __atomic_store_n(&arrived[index], round, __ATOMIC_RELEASE);
    const u32 start = StreamTicks();
    for (const auto& participant : arrived) {
      while (__atomic_load_n(&participant, __ATOMIC_ACQUIRE) < round) {
        require(StreamTicks() - start < 250, "four heap users reach the live-allocation barrier");
        Yield(0);
      }
    }
    for (u32 i = 0; i < 12; ++i) {
      verifyBytes(pointers[i], sizes[i], (u8)(value + i));
      require(malloc_size(pointers[i]) >= sizes[i], "concurrent heap capacity lookup");
      free(pointers[i]);
    }
  }
  return (int)index + 1;
}

void multipleThreads() {
  Worker workers[Participants] = {{0}, {1}, {2}, {3}};
  int threads[Participants - 1];
  for (u32 i = 0; i < Participants - 1; ++i) {
    threads[i] = sino::thread::create(threadWork, &workers[i + 1]);
    require(threads[i] > 0, "start heap worker");
  }
  require(threadWork(&workers[0]) == 1, "main thread participated");
  for (u32 i = 0; i < Participants - 1; ++i) {
    ThreadResult result;
    require(sino::thread::join(threads[i], &result, 900) == S_OK && result.code == (int)i + 2,
            "join all heap workers");
  }
}

void directMapping() {
  auto* small = allocate(32);
  auto* large = allocate(4097);
  void* direct = pageBase(large);
  require(direct != pageBase(small) && malloc_size(large) == 4097 && AnonQuery(direct) == S_OK,
          "large allocation owns an independent anonymous region");
  memset(large, 0x47, 4097);
  memset(small, 0x33, 32);
  verifyBytes(large, 4097, 0x47);
  free(large);
  require(AnonQuery(direct) == S_INVALID && AnonQuery(pageBase(small)) == S_OK,
          "direct free returns its region without releasing the small slab");
  verifyBytes(small, 32, 0x33);
  free(small);
}

void resize() {
  auto* small = static_cast<u8*>(realloc(nullptr, 20));
  require(small && malloc_size(small) == 32, "realloc null allocates a small slot");
  memset(small, 0x7A, 20);
  require(realloc(small, 12) == small && realloc(small, 32) == small, "resize within capacity");
  auto* large = static_cast<u8*>(realloc(small, 8193));
  require(large && large != small && malloc_size(large) == 8193, "moving realloc reports capacity");
  verifyBytes(large, 20, 0x7A);

  const int reservation = AnonMap(ANON_MAX_BYTES - 160 * 1024);
  require(reservation > 0 && !realloc(large, 256 * 1024), "new mapping fails under byte quota");
  verifyBytes(large, 20, 0x7A);
  require(malloc_size(large) == 8193 && AnonUnmap((void*)(u32)reservation) == S_OK,
          "failed realloc leaves the old allocation live");
  void* base = pageBase(large);
  require(!realloc(large, 0) && AnonQuery(base) == S_INVALID, "zero resize frees direct mapping");
}

void exhaustReservations() {
  auto* cached = allocate(32);
  void* hotBase = pageBase(cached);
  free(cached);
  int other[ANON_MAX_REGIONS - 1];
  for (auto& base : other) {
    base = AnonMap(1);
    require(base > 0, "fill all slots outside the heap cache");
  }
  require(AnonMap(1) == S_LIMIT && AnonQuery(hotBase) == S_OK, "anonymous quota includes the hot slab");
  auto* direct = allocate(8192);
  require(pageBase(direct) == hotBase && !malloc_size(cached) && !malloc(8192),
          "direct allocation reuses the space and slot of the evicted hot slab");
  require(AnonUnmap((void*)(u32)other[0]) == S_OK, "return an independent region slot");
  free(allocate(8192));
  free(direct);
  for (u32 i = 1; i < ANON_MAX_REGIONS - 1; ++i) {
    require(AnonUnmap((void*)(u32)other[i]) == S_OK, "release independent fixture");
  }
}

void recycle() {
  constexpr u32 Count = 14;
  SlabBatch batches[Count];
  void* bases[Count];
  for (u32 i = 0; i < Count; ++i) { bases[i] = fillSlab(batches[i]); }
  for (const auto& batch : batches) { freeSlab(batch); }
  require(AnonQuery(bases[0]) == S_INVALID && AnonQuery(bases[1]) == S_OK,
          "cold cache keeps twelve slabs and evicts the oldest mapping");
  require(allocate(LargeChunk) == batches[Count - 1][SlabSlots - 1], "same class reuses latest hot slot");
  auto* rebuilt = allocate(2032);
  require(pageBase(rebuilt) == bases[Count - 2] && malloc_size(rebuilt) == 2032 &&
          AnonLock(bases[Count - 2]) == S_INVALID, "latest cold slab is rebuilt for another size class");
}

void purgeAndRebuild() {
  SlabBatch first, second;
  void* base = fillSlab(first);
  fillSlab(second);
  auto* keeper = allocate(32);
  memset(keeper, 0x94, 32);
  freeSlab(first);
  freeSlab(second);
  require(AnonQuery(base) == S_OK, "cold slab starts with its reservation and contents");
  char address[12];
  number((u32)base, address);
  Log("HEAP PURGE READY ");
  Log(address);
  Log("\n");

  const u32 start = StreamTicks();
  for (;;) {
    const int state = AnonQuery(base);
    if (state == S_CHANGED) { break; }
    require(state == S_OK && StreamTicks() - start < 900, "wait for host to discard the cold slab");
    Yield(1);
  }
  Log("HEAP PURGE DROPPED\n");
  for (;;) {
    const int temporary = AnonMap(1);
    if (temporary > 0) {
      require(AnonUnmap((void*)(u32)temporary) == S_OK, "release watermark probe");
      break;
    }
    require(temporary == S_NOMEM && StreamTicks() - start < 900, "wait for normal watermarks");
    Yield(1);
  }
  auto* rebuilt = allocate(2032);
  require(pageBase(rebuilt) == base && malloc_size(rebuilt) == 2032 && AnonLock(base) == S_INVALID,
          "rebuild purged mapping with the new size and clear discardability before use");
  memset(rebuilt, 0x82, 2032);
  verifyBytes(keeper, 32, 0x94);
  verifyBytes(rebuilt, 2032, 0x82);
  free(rebuilt);
  free(keeper);
}

} // namespace

int main() {
  const char* mode = Arguments();
  CliInfo cli;
  char* argv[2];
  if (!*mode && CliArguments(&cli, argv, 2) == 1) { mode = argv[0]; }
  if (streq(mode, "st")) { singleThread(); }
  else if (streq(mode, "mt")) { multipleThreads(); }
  else if (streq(mode, "large")) { directMapping(); }
  else if (streq(mode, "realloc")) { resize(); }
  else if (streq(mode, "exhaust")) { exhaustReservations(); }
  else if (streq(mode, "recycle")) { recycle(); }
  else if (streq(mode, "purge")) { purgeAndRebuild(); }
  else { require(false, "unknown heap probe mode"); }
  Log("HEAP PASS\n");
  return 0;
}
