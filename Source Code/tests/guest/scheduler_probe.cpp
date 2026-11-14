#include "sdk.h"
#include "scheduler_probe.h"

namespace schedulerprobe {
namespace {
PipePair pipe;
volatile u32 started[2], finished[2];
u8 received[2];

u32 now() { return (u32)api(SYS_TICKS, 0, 0, 0); }

void require(bool ok, const char* label) {
  if (!ok) {
    Log("USER PROBE FAIL: scheduler ");
    Log(label);
    Log("\n");
    Exit(1);
  }
}

void reader(u32 index) {
  started[index] = 1;
  int result = StreamReadBlocking((int)pipe.read, &received[index], 1, 100);
  require(result == 1, "blocking reader result");
  finished[index] = 1;
  api(SYS_THREAD_EXIT, 0, 0, 0);
  for (;;) {}
}
void firstReader() { reader(0); }
void secondReader() { reader(1); }

void waitFor(bool starting, u32 count) {
  const u32 begin = now();
  while ((starting ? started[0] + started[1] : finished[0] + finished[1]) < count) {
    require(now() - begin < 100, "worker progress");
    Yield(0);
  }
}
}

int run() {
  require(PipeCreate(2, &pipe) == S_OK, "pipe create");
  require(api(SYS_THREAD, (u32)firstReader, 0, 0) > 0 &&
          api(SYS_THREAD, (u32)secondReader, 0, 0) > 0, "reader threads");
  waitFor(true, 2);
  // 故意让当前线程抢走通知后的数据，覆盖重试 S_AGAIN 和原期限保留。
  require(StreamWrite((int)pipe.write, "x", 1) == 1, "stale notification byte");
  u8 stolen;
  require(StreamRead((int)pipe.read, &stolen, 1) == 1 && stolen == 'x', "consume before retry");
  Yield(0);
  // 一次提供两个字节，只通知一个读者；成功后交接剩余字节，不依赖新写入。
  require(StreamWrite((int)pipe.write, "ab", 2) == 2, "handoff bytes");
  waitFor(false, 2);
  require((received[0] == 'a' && received[1] == 'b') ||
          (received[0] == 'b' && received[1] == 'a'), "exact once bytes");
  require(Close((int)pipe.read) == S_OK && Close((int)pipe.write) == S_OK,
          "pipe close");

  bool sameTick = false;
  for (u32 i = 0; i < 96; i++) {
    const u32 tick = now();
    Yield(0);
    sameTick = sameTick || now() == tick;
  }
  require(sameTick, "yield zero should not require a timer IRQ");
  const u32 begin = now();
  Yield(5);
  require(now() - begin >= 5, "finite yield not early");
  Log("SCHEDULER PROBE PASS: yield blocking retry deadlines\n");
  return 0;
}
}
