#include "stream.h"

static constexpr u32 WideImageSampleOffset = 192 * 1024;
static volatile u8 image[SNX_V2_IMAGE_BYTES - 32768];
static volatile u32 worker_done;

static void require(int condition, const char* why) {
  if (condition) return;
  Log("SNX V2 SELFTEST FAIL: ");
  Log(why);
  Log("\n");
  Exit(1);
}

static int worker(void*) {
  StartInfo info;
  const u32 stackAddress = (u32)&info;
  require(stackAddress >= USER_STACK(1) - 16384 && stackAddress < USER_STACK(1),
          "worker local uses the first v2 stack");
  worker_done = QueryStartInfo(&info) == S_OK && info.policy == PROCESS_STRICT;
  return S_OK;
}

int main(void) {
  if (streq(Arguments(), "oom-vma-stack")) {
    require(sino::thread::create(worker, nullptr, ThreadCreateFlags::Detached) == S_NOMEM,
            "stack VMA failure reported");
    require(QueryStartInfo((StartInfo*)(USER_STACK(1) - sizeof(StartInfo))) == S_INVALID,
            "failed stack unmapped");
    require(sino::thread::create(worker, nullptr, ThreadCreateFlags::Detached) > 0,
            "failed slot reused");
    const u32 begin = StreamTicks();
    while (!worker_done && StreamTicks() - begin < 100) { Yield(1); }
    require(worker_done, "replacement worker ran");
    Log("VMA STACK RETRY PASS\n");
    return 0;
  }
  SystemInfo info;
  require(QuerySystemInfo(&info) == S_OK && (info.features & FEATURE_SNX_V2) &&
          info.image_bytes == SNX_V2_IMAGE_BYTES, "reported v2 image limit");
  require((u32)Arguments() == USER_ARGUMENTS && streq(Arguments(), "wide") &&
          streq(Executable(), "/boot/memv2.snx"), "v2 startup strings");
  require(QueryStartInfo((StartInfo*)USER_ARGUMENTS) == S_INVALID &&
          QueryStartInfo((StartInfo*)(USER_STACK(0) - 4)) == S_INVALID,
          "argument page read-only and high stack guard unmapped");

  u32 end = (u32)(image + sizeof(image));
  require((u32)image < USER_DYNAMIC_BASE && end > USER_DYNAMIC_BASE &&
          !image[WideImageSampleOffset] && !image[sizeof(image) - 1], "wide BSS is zero-filled");
  image[sizeof(image) - 1] = 0x5a;
  require(QueryStartInfo((StartInfo*)(USER_DYNAMIC_BASE - 8)) == S_OK,
          "copy crosses the dynamic base inside the image");
  require(sino::thread::create(reinterpret_cast<sino::thread::Entry>(image), nullptr,
                               ThreadCreateFlags::Detached) == S_INVALID &&
          sino::thread::create(worker, nullptr, ThreadCreateFlags::Detached) > 0,
          "thread entry remains code-only");
  u32 begin = StreamTicks();
  while (!worker_done && StreamTicks() - begin < 100) Yield(1);
  require(worker_done, "v2 worker uses relocated stack");

  const u32 size = 13 * 1024 * 1024;
  volatile u8* heap = (volatile u8*)malloc(size);
  require(heap && (u32)heap >= end && malloc_size((const void*)heap) >= size,
          "heap skips the wide image and reports its capacity");
  for (u32 i = 0; i < size; i++) heap[i] = (u8)(i ^ (i >> 12) ^ 0x3c);
  for (u32 i = 0; i < size; i++)
    if (heap[i] != (u8)(i ^ (i >> 12) ^ 0x3c)) require(0, "large heap byte mismatch");
  require(image[sizeof(image) - 1] == 0x5a, "heap kept the image intact");
  Log("SNX V2 LARGE HEAP READY\n");
  Yield(80);
  free((void*)heap);
  Log("SNX V2 PASS\n");
  return 0;
}
