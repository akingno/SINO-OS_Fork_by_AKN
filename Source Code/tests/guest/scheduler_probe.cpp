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
  require(PipeCreate(1, &pipe) == S_OK, "pipe create");
  require(api(SYS_THREAD, (u32)firstReader, 0, 0) > 0 &&
          api(SYS_THREAD, (u32)secondReader, 0, 0) > 0, "reader threads");
  waitFor(true, 2);
  // 两个读者均已挂起；一个字节通知二者，第二个重试 S_AGAIN 必须再次阻塞。
  require(StreamWrite((int)pipe.write, "a", 1) == 1, "first byte");
  waitFor(false, 1);
  require(StreamWrite((int)pipe.write, "b", 1) == 1, "second byte");
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
