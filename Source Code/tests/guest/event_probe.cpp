#include "sdk.h"
#include "event_probe.h"

static_assert(SYS_EVENT_WAIT == 0x115, "legacy event query number");
static_assert(SYS_EVENT_WAIT_BLOCK == 0x151, "blocking event number");
static_assert(SYS_CONSOLE == 0x150, "console ABI unchanged");
static_assert(FEATURE_EVENT_WAIT == 32768u, "event discovery bit");

namespace eventprobe {
namespace {
namespace event = sino::event;
u32 ticks() { return (u32)api(SYS_TICKS, 0, 0, 0); }
void require(bool ok, const char* label) {
  if (!ok) {
    Log("USER PROBE FAIL: event "); Log(label); Log("\n"); Exit(1);
  }
}
struct Waiter {
  int handle;
  u32 timeout;
  volatile u32 entered, done;
  int result;
  u32 elapsed;
  void run() {
    entered = 1;
    const u32 begin = ticks();
    result = event::wait(handle, timeout);
    elapsed = ticks() - begin;
    done = 1;
    api(SYS_THREAD_EXIT, 0, 0, 0);
    for (;;) {}
  }
};
Waiter waiters[2];
void first() { waiters[0].run(); }
void second() { waiters[1].run(); }
void start(u32 index, int handle, u32 timeout) {
  waiters[index].handle = handle;
  waiters[index].timeout = timeout;
  require(api(SYS_THREAD, (u32)(index ? second : first), 0, 0) > 0, "create waiter");
}
void until(u32 count, bool entered) {
  const u32 begin = ticks();
  while ((entered ? waiters[0].entered + waiters[1].entered
                 : waiters[0].done + waiters[1].done) < count) {
    require(ticks() - begin < 200, "waiter progress");
    Yield(0);
  }
}
void legacy() {
  SystemInfo system;
  StartInfo launch;
  require(event::available() && QuerySystemInfo(&system) == S_OK &&
          QueryStartInfo(&launch) == S_OK && (system.features & FEATURE_EVENT_WAIT) &&
          (launch.features & FEATURE_EVENT_WAIT) && launch.policy == PROCESS_STRICT,
          "strict feature discovery");
  int h = event::create();
  require(h > 0 && event::query(h) == S_AGAIN, "legacy query empty");
  require(api(0x115, (u32)h, 123, 456) == S_AGAIN, "legacy unused arguments unchanged");
  require(event::set(h, true) == S_OK && api(0x115, (u32)h, 0, 0) == S_OK,
          "legacy set/query");
  require(Close(h) == S_OK, "legacy close");
}
void preset() {
  int h = event::create();
  require(h > 0 && event::set(h, true) == S_OK, "pre-set");
  require(event::wait(h) == S_OK && event::wait(h, 1) == S_OK &&
          event::query(h) == S_OK, "success does not reset");
  require(event::set(h, false) == S_OK && event::query(h) == S_AGAIN,
          "explicit reset");
  require(Close(h) == S_OK, "preset close");
}
void broadcast() {
  int h = event::create();
  require(h > 0, "broadcast event");
  start(0, h, 0);
  start(1, h, STREAM_WAIT_MAX);
  until(2, true);
  require(event::set(h, false) == S_OK, "reset while parked");
  Yield(20);
  require(!waiters[0].done && !waiters[1].done, "unrelated ticks/reset do not complete");
  require(event::set(h, true) == S_OK && event::set(h, true) == S_OK, "broadcast repeated set");
  until(2, false);
  require(waiters[0].result == S_OK && waiters[1].result == S_OK &&
          event::query(h) == S_OK, "all waiters observe manual-reset state");
  require(Close(h) == S_OK, "broadcast close");
}
void resetDeadline() {
  int h = event::create();
  require(h > 0, "reset event");
  start(0, h, 12);
  until(1, true);
  for (u32 i = 0; i < 4; i++) {
    // 新时间片开始后连续 Set/Reset，在等待者重试前撤销条件。
    Yield(0);
    require(event::set(h, true) == S_OK && event::set(h, false) == S_OK, "set then reset");
    Yield(2);
  }
  until(1, false);
  require(waiters[0].result == S_TIMEOUT && waiters[0].elapsed >= 12 &&
          waiters[0].elapsed < 18, "retry preserves original deadline");
  require(event::query(h) == S_AGAIN && Close(h) == S_OK, "reset stays clear");
}
void timeout() {
  int h = event::create();
  require(h > 0, "timeout event");
  require(event::wait(h, STREAM_WAIT_MAX + 1) == S_INVALID &&
          api(SYS_EVENT_WAIT_BLOCK, (u32)h, 1, 1) == S_INVALID &&
          event::wait(-1, 1) == S_BADHANDLE &&
          event::wait(-1, STREAM_WAIT_MAX + 1) == S_INVALID, "wire validation");
  u32 begin = ticks();
  require(event::wait(h, 5) == S_TIMEOUT && ticks() - begin >= 5, "finite wait deadline");
  require(event::set(h, true) == S_OK && event::wait(h, 1) == S_OK, "timeout leaves usable event");
  require(Close(h) == S_OK, "timeout close");
}
void localClose() {
  int h = event::create();
  require(h > 0, "close event");
  start(0, h, 100);
  until(1, true);
  require(event::set(h, true) == S_OK && Close(h) == S_OK, "close notified handle");
  int reused = event::create();
  require(reused > 0 && reused != h && event::query(h) == S_BADHANDLE,
          "stale handle after slot reuse");
  until(1, false);
  require(waiters[0].result == S_BADHANDLE && event::query(reused) == S_AGAIN &&
          event::wait(reused, 2) == S_TIMEOUT, "old wait cannot complete against new event");
  require(Close(reused) == S_OK, "reuse close");
}
void duplicates() {
  int original = event::create();
  require(original > 0, "duplicate event");
  int read = api(SYS_DUP, (u32)original, 1, 0);
  int write = api(SYS_DUP, (u32)original, 2, 0);
  require(read > 0 && write > 0 && event::set(read, true) == S_BADHANDLE &&
          event::query(write) == S_BADHANDLE && event::wait(write, 1) == S_BADHANDLE,
          "event read/write rights");
  PipePair pipe;
  require(PipeCreate(1, &pipe) == S_OK && event::wait((int)pipe.read, 1) == S_BADHANDLE,
          "wrong object type");
  require(Close((int)pipe.read) == S_OK && Close((int)pipe.write) == S_OK, "type probe close");
  start(0, read, 0);
  until(1, true);
  require(Close(original) == S_OK, "close other duplicate");
  Yield(3);
  require(!waiters[0].done && event::set(write, true) == S_OK, "duplicate wait survives");
  until(1, false);
  require(waiters[0].result == S_OK && Close(read) == S_OK && Close(write) == S_OK,
          "duplicate result and cleanup");
}
void park(bool fault) {
  int h = event::create();
  require(h > 0, "cancel/fault event");
  start(0, h, 500);
  until(1, true);
  if (fault) {
    Log("EVENT EXPECT FAULT\n");
    *(volatile u32*)0x100000 = 1;
  } else {
    Log("EVENT CANCEL PARKED\n");
    event::wait(h);
  }
  require(false, "parked path returned");
}
}
bool matches(const char* mode) {
  return streq(mode, "/event-legacy") || streq(mode, "/event-preset") ||
         streq(mode, "/event-broadcast") || streq(mode, "/event-reset") ||
         streq(mode, "/event-timeout") || streq(mode, "/event-close") ||
         streq(mode, "/event-duplicates") || streq(mode, "/event-cancel") ||
         streq(mode, "/event-fault");
}
int run(const char* mode) {
  if (streq(mode, "/event-legacy")) { legacy(); }
  else if (streq(mode, "/event-preset")) { preset(); }
  else if (streq(mode, "/event-broadcast")) { broadcast(); }
  else if (streq(mode, "/event-reset")) { resetDeadline(); }
  else if (streq(mode, "/event-timeout")) { timeout(); }
  else if (streq(mode, "/event-close")) { localClose(); }
  else if (streq(mode, "/event-duplicates")) { duplicates(); }
  else if (streq(mode, "/event-cancel")) { park(false); }
  else if (streq(mode, "/event-fault")) { park(true); }
  else { require(false, "unknown mode"); }
  Log("EVENT PROBE PASS "); Log(mode); Log("\n");
  return 0;
}
}
