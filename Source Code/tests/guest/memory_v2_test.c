#include "stream.h"

static volatile u8 image[SNX_V2_IMAGE_BYTES - 32768];
static volatile u32 worker_done;

static void require(int condition, const char* why) {
  if (condition) return;
  Log("SNX V2 SELFTEST FAIL: ");
  Log(why);
  Log("\n");
  Exit(1);
}

static void worker(void) {
  StartInfo info;
  worker_done = QueryStartInfo(&info) == S_OK && info.policy == PROCESS_STRICT;
  api(SYS_THREAD_EXIT, 0, 0, 0);
}

int main(void) {
  SystemInfo info;
  require(QuerySystemInfo(&info) == S_OK && (info.features & FEATURE_SNX_V2) &&
          info.image_bytes == SNX_V2_IMAGE_BYTES, "reported v2 image limit");
  require((u32)Arguments() == USER_V2_ARGUMENTS && streq(Arguments(), "wide") &&
          streq(Executable(), "/boot/memv2.snx"), "v2 startup strings");
  require(QueryStartInfo((StartInfo*)USER_V2_ARGUMENTS) == S_INVALID &&
          QueryStartInfo((StartInfo*)(USER_V2_STACK(0) - 4)) == S_INVALID,
          "argument page read-only and high stack guard unmapped");

  u32 end = (u32)(image + sizeof(image));
  require((u32)image < USER_DYNAMIC_BASE && end > USER_DYNAMIC_BASE &&
          !image[SNX_V1_IMAGE_BYTES] && !image[sizeof(image) - 1], "wide BSS is zero-filled");
  image[sizeof(image) - 1] = 0x5a;
  require(QueryStartInfo((StartInfo*)(USER_DYNAMIC_BASE - 8)) == S_OK,
          "copy crosses the dynamic base inside the image");
  require(api(SYS_THREAD, (u32)image, 0, 0) == S_INVALID &&
          api(SYS_THREAD, (u32)worker, 0, 0) == 1, "thread entry remains code-only");
  u32 begin = StreamTicks();
  while (!worker_done && StreamTicks() - begin < 100) Yield(1);
  require(worker_done, "v2 worker uses relocated stack");

  const u32 size = 13 * 1024 * 1024;
  volatile u8* heap = (volatile u8*)malloc(size);
  require(heap && (u32)heap >= end && !heap[0] && !heap[size - 1],
          "heap skips the wide image and starts zero-filled");
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
