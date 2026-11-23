#include "stream.h"
#include "thread_probe.h"

namespace thread = sino::thread;

static constexpr u32 WideImageSampleOffset = 192 * 1024;
static volatile u8 image[SNX_V2_IMAGE_BYTES - 32768];
static volatile u32 worker_done;
static threadprobe::StackSample worker_stack;

static void require(int condition, const char* why) {
  if (condition) return;
  Log("SNX V2 SELFTEST FAIL: ");
  Log(why);
  Log("\n");
  Exit(1);
}

static int worker(void* arg) {
  const u32 top = static_cast<threadprobe::StackSample*>(arg)->top();
  StartInfo info;
  const u32 stackAddress = (u32)&info;
  require(top > USER_MAIN_STACK_TOP && top <= threadprobe::WorkerWindowEnd &&
          stackAddress >= top - threadprobe::StackBytes && stackAddress < top,
          "worker local uses its sampled stack");
  worker_done = QueryStartInfo(&info) == S_OK && info.policy == PROCESS_STRICT;
  return S_OK;
}

static void waitForPressure(bool low) {
  const u32 begin = StreamTicks();
  for (;;) {
    const u32 mapped = AnonMap(1);
    const int status = AnonMapStatus(mapped);
    require(status == S_NOMEM || (status == S_OK && AnonUnmap((void*)mapped) == S_OK),
            "poll pressure with a temporary reservation");
    if ((status == S_NOMEM) == low) { return; }
    require(StreamTicks() - begin < 900, "host resource sample timed out");
    Yield(1);
  }
}

static int sampleWorker() {
  worker_done = 0;
  const int tid = threadprobe::createCaptured(worker, worker_stack);
  ThreadResult result;
  require(tid > 0 && thread::join(tid, &result, 100) == S_OK && result.code == S_OK && worker_done,
          "sample worker ran and was joined");
  return tid;
}

static int stackRollback(bool objectFailure) {
  const int original = sampleWorker();
  const u32 top = worker_stack.top();
  Log("VMA STACK BASELINE\n");
  waitForPressure(true);
  require(threadprobe::createCaptured(worker, worker_stack) == (objectFailure ? S_LIMIT : S_NOMEM),
          "worker create returns the injected error");
  Log("VMA STACK FAILED\n");
  waitForPressure(false);
  const int retry = sampleWorker();
  require(worker_stack.top() == top && (u32)retry == (u32)original + 8,
          "retry reuses the same stack and thread slot without consuming a TID");
  Log("VMA STACK RETRY PASS\n");
  return 0;
}

int main(void) {
  const bool objectFailure = streq(Arguments(), "oom-stack-object");
  if (objectFailure || streq(Arguments(), "oom-stack-data") || streq(Arguments(), "oom-vma-stack") ||
      streq(Arguments(), "oom-vma-guard")) { return stackRollback(objectFailure); }
  SystemInfo info;
  require(QuerySystemInfo(&info) == S_OK && (info.features & FEATURE_SNX_V2) &&
          info.image_bytes == SNX_V2_IMAGE_BYTES, "reported v2 image limit");
  require((u32)Arguments() == USER_ARGUMENTS && streq(Arguments(), "wide") &&
          streq(Executable(), "/boot/memv2.snx"), "v2 startup strings");
  require(QueryStartInfo((StartInfo*)USER_ARGUMENTS) == S_INVALID &&
          QueryStartInfo((StartInfo*)(USER_MAIN_STACK_TOP - 4)) == S_INVALID &&
          QueryStartInfo((StartInfo*)(USER_MAIN_STACK_TOP - threadprobe::StackBytes - sizeof(StartInfo))) == S_INVALID,
          "argument page read-only and high stack guard unmapped");

  constexpr u32 imagePdeBoundary = USER_BASE + 4 * 1024 * 1024;
  u32 end = (u32)(image + sizeof(image));
  require((u32)image < imagePdeBoundary && end > imagePdeBoundary &&
          !image[WideImageSampleOffset] && !image[sizeof(image) - 1], "wide BSS is zero-filled");
  image[sizeof(image) - 1] = 0x5a;
  require(QueryStartInfo((StartInfo*)(imagePdeBoundary - 8)) == S_OK,
          "copy crosses a PDE inside the wide image");
  require(thread::create(reinterpret_cast<thread::Entry>(image), nullptr,
                         ThreadCreateFlags::Detached) == S_INVALID &&
          threadprobe::createCaptured(worker, worker_stack, ThreadCreateFlags::Detached) > 0,
          "thread entry remains code-only");
  u32 begin = StreamTicks();
  while (!worker_done && StreamTicks() - begin < 100) Yield(1);
  require(worker_done, "v2 worker uses relocated stack");

  const u32 size = 13 * 1024 * 1024;
  volatile u8* heap = (volatile u8*)malloc(size);
  require(heap && (u32)heap >= USER_DYNAMIC_BASE && (u32)heap < USER_DYNAMIC_END &&
          malloc_size((const void*)heap) >= size, "heap uses high addresses and reports its capacity");
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
