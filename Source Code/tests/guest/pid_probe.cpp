#include "sdk.h"
#include "pid_probe.h"
namespace pidprobe {
namespace {
void require(bool ok, const char* why) {
  if (!ok) { Log("USER PROBE FAIL: identifiers "); Log(why); Log("\n"); Exit(1); }
}
int worker(void*) { return 29; }
void history() {
  int old = SpawnTracked("/boot/probe.snx", "/pid-child-exit");
  ProcessCompletion before;
  require(old > 0 && WaitProcess(old, &before, 100) == S_OK && before.code == 73,
          "historical child result");
  int next = SpawnTracked("/boot/probe.snx", "/pid-child-spin");
  require(next > 0 && CancelProcess(old) == S_OK, "cancel completed record is harmless");
  ProcessCompletion after;
  require(GetProcessResult(next, &after) == S_AGAIN, "old completion cannot cancel new life");
  require(CancelProcess(next) == S_OK && WaitProcess(next, &after, 100) == S_OK &&
          after.reason == PROCESS_CANCELED && after.pid != before.pid &&
          (after.pid & 15) == (before.pid & 15), "same slot, distinct life");
  ProcessCompletion again;
  require(GetProcessResult(old, &again) == S_OK && again.pid == before.pid &&
          again.code == 73 && again.reason == PROCESS_EXITED, "history stays bound");
  require(Close(old) == S_OK && CancelProcess(old) == S_BADHANDLE && Close(next) == S_OK,
          "stale completion handle denied");
}
void shellFile() {
  int directory = GrantDirectory("/boot", FS_READ | FS_LOOKUP);
  int file = OpenAt(directory, "readme.txt", FS_READ, 0);
  char byte;
  require(directory > 0 && file > 0 && FileReadAt(file, 0, &byte, 1) == 1 &&
          Close(file) == S_OK && Close(directory) == S_OK, "directory/file workload");
}
void pipes() {
  PipePair pair;
  require(PipeCreate(16, &pair) == S_OK, "pipe create");
  const char data[] = "x";
  char out;
  require(api(SYS_STREAM_WRITE, pair.write, (u32)data, 1) == 1 &&
          api(SYS_STREAM_READ, pair.read, (u32)&out, 1) == 1 && out == 'x' &&
          Close((int)pair.read) == S_OK && Close((int)pair.write) == S_OK, "pipe transfer/close");
}
void events() {
  int handle = sino::event::create();
  int duplicate = api(SYS_DUP, (u32)handle, 1, 0);
  require(handle > 0 && duplicate > 0 && sino::event::set(handle, true) == S_OK &&
          sino::event::wait(duplicate) == S_OK && Close(handle) == S_OK &&
          Close(duplicate) == S_OK, "Event dup/set/wait/close");
  // 失败的第 25 次 Event 创建先创建 Object，再因句柄表满回滚，代数照样消耗。
  int held[24];
  for (int& h : held) { h = sino::event::create(); require(h > 0, "fill handles"); }
  require(sino::event::create() == S_LIMIT, "object allocation followed by handle rollback");
  for (int h : held) { require(Close(h) == S_OK, "close full table"); }
}
void threads() {
  ThreadResult result;
  int tid = sino::thread::create(worker, nullptr);
  require(tid > 0 && sino::thread::join(tid, &result, 100) == S_OK && result.code == 29,
          "joinable thread workload");
  tid = sino::thread::create(worker, nullptr, ThreadCreateFlags::Detached);
  require(tid > 0, "detached thread workload");
  Yield(3);
}
}
bool matches(const char* mode) {
  return streq(mode, "/pid-history") || streq(mode, "/pid-child-exit") ||
         streq(mode, "/pid-child-spin") || streq(mode, "/pid-shell-file") ||
         streq(mode, "/pid-pipe") || streq(mode, "/pid-event") || streq(mode, "/pid-thread");
}
int run(const char* mode) {
  if (streq(mode, "/pid-child-exit")) { return 73; }
  if (streq(mode, "/pid-child-spin")) { for (;;) { Yield(1); } }
  if (streq(mode, "/pid-history")) { history(); }
  else if (streq(mode, "/pid-shell-file")) { shellFile(); }
  else if (streq(mode, "/pid-pipe")) { pipes(); }
  else if (streq(mode, "/pid-event")) { events(); }
  else if (streq(mode, "/pid-thread")) { threads(); }
  else { require(false, "unknown mode"); }
  Log("IDENTIFIER PROBE PASS "); Log(mode); Log("\n");
  return 0;
}
}
