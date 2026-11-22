#include "sdk.h"
#include "thread_probe.h"

static_assert(SYS_THREAD_CREATE == 0x152 && SYS_THREAD_EXIT_CODE == 0x155, "new thread ABI");
namespace threadprobe {
namespace {
namespace thread = sino::thread;
namespace event = sino::event;
void require(bool ok, const char* label) {
  if (!ok) { Log("USER PROBE FAIL: thread "); Log(label); Log("\n"); Exit(1); }
}
struct Config {
  StackSample stack;
  int gate, code;
  u32 delay;
  int tid;
  bool self;
  volatile u32 entered;
};
Config configs[3];
int worker(void* raw) {
  Config* c = (Config*)raw;
  require(c >= configs && c < configs + 3, "argument pointer");
  c->entered = 1;
  if (c->gate > 0) { require(event::wait(c->gate) == S_OK, "worker gate"); }
  if (c->self) {
    ThreadResult out;
    require(thread::join(c->tid, &out, 1) == S_INVALID, "self join denied");
  }
  if (c->delay) { Yield(c->delay); }
  return c->code;
}
ThreadResult joined(int tid, int code, u32 timeout = 100) {
  struct { u32 before; ThreadResult result; u32 after; } out;
  memset(&out, 0xcc, sizeof(out));
  require(thread::join(tid, &out.result, timeout) == S_OK && out.result.size == 16 &&
          out.result.version == 1 && out.result.tid == (u32)tid && out.result.code == code &&
          out.before == 0xccccccccu && out.after == 0xccccccccu, "join result/canaries");
  return out.result;
}
void started(Config& c) {
  u32 begin = (u32)api(SYS_TICKS, 0, 0, 0);
  while (!c.entered) {
    require((u32)api(SYS_TICKS, 0, 0, 0) - begin < 100, "worker start");
    Yield(0);
  }
}
void returning() {
  require(thread::available(), "feature discovery");
  configs[0].code = -47;
  StackSample& sample = configs[0].stack;
  int tid = createCaptured(worker, sample);
  require(tid > 4, "safe TID");
  joined(tid, -47);
  require(sample.top() > USER_MAIN_STACK_TOP && sample.top() <= WorkerWindowEnd &&
          !(sample.top() % PageBytes) && !sample.returnAddress && sample.entry == (u32)worker &&
          sample.argument == (u32)&sample, "initial cdecl stack frame");
  configs[1].code = 123;
  tid = thread::create(worker, &configs[1]);
  require(tid > 0, "SDK trampoline create");
  joined(tid, 123);
}
void exitedRecord() {
  configs[0].code = -91;
  StackSample& sample = configs[0].stack;
  int tid = createCaptured(worker, sample);
  require(tid > 0, "record create");
  Yield(2);
  const u32 top = sample.top();
  require(sample.esp != 0, "stack captured before worker exit");
  for (u32 page = PageBytes; page <= StackBytes; page += PageBytes) {
    require(api(SYS_LOG, top - page, 0, 1) == S_INVALID,
            "exited thread stack unmapped before join");
  }
  require(thread::query(tid, (ThreadResult*)USER_BASE) == S_INVALID &&
          thread::query(tid, (ThreadResult*)0xfffffff8u) == S_INVALID,
          "invalid result does not consume record");
  const int other = createCaptured(worker, configs[1].stack);
  require(other > 0 && ((u32)other & 7u) != ((u32)tid & 7u), "unclaimed record holds its TID slot");
  joined(other, 0);
  require(configs[1].stack.top() == top, "unclaimed record does not hold the stack address");
  ThreadResult out;
  require(thread::query(tid, &out) == S_OK && out.code == -91 && out.tid == (u32)tid,
          "exit before query");
  require(thread::query(tid, &out) == S_BADHANDLE && thread::detach(tid) == S_BADHANDLE,
          "result consumed once");
  int next = createCaptured(worker, sample);
  require(next > 0 && next != tid && (((u32)next & 7) == ((u32)tid & 7)) &&
          thread::join(tid, &out, 1) == S_BADHANDLE, "stale TID after reuse");
  joined(next, -91);
  require(sample.top() == top, "released stack address reused");
}
struct UnmapConfig { void* address; int gate; } unmapping;
int unmapper(void* raw) {
  auto* c = (UnmapConfig*)raw;
  Yield(2);
  require(AnonUnmap(c->address) == S_OK && event::set(c->gate, true) == S_OK,
          "unmap waiting output");
  return 0;
}
void timeoutAndUnmap() {
  configs[0].delay = 12; configs[0].code = 31;
  int tid = thread::create(worker, &configs[0]);
  require(tid > 0, "timeout target");
  ThreadResult out;
  memset(&out, 0xcc, sizeof(out));
  require(thread::query(tid, &out) == S_AGAIN && out.size == 0xccccccccu,
          "live query leaves output");
  require(thread::join(tid, &out, 2) == S_TIMEOUT && out.size == 0xccccccccu,
          "join timeout leaves output and claim");
  joined(tid, 31);
  configs[1].gate = event::create(); configs[1].code = 77;
  require(configs[1].gate > 0, "unmap gate");
  tid = thread::create(worker, &configs[1]);
  int base = AnonMap(4096);
  require(tid > 0 && base > 0, "unmap setup");
  unmapping = {(void*)(u32)base, configs[1].gate};
  int helper = thread::create(unmapper, &unmapping);
  require(helper > 0 && thread::join(tid, (ThreadResult*)(u32)base, 100) == S_INVALID,
          "retry validates unmapped output");
  joined(tid, 77); // 失败不消费退出结果，也不残留 join 占用。
  joined(helper, 0);
  require(Close(configs[1].gate) == S_OK, "unmap gate close");
}
void detached() {
  configs[0].gate = event::create();
  require(configs[0].gate > 0, "detach gate");
  int tid = thread::create(worker, &configs[0]);
  ThreadResult out;
  require(tid > 0 && thread::detach(tid) == S_OK && thread::detach(tid) == S_INVALID &&
          thread::query(tid, &out) == S_INVALID, "detach live target");
  require(event::set(configs[0].gate, true) == S_OK, "detach release");
  Yield(2);
  require(thread::query(tid, &out) == S_BADHANDLE && Close(configs[0].gate) == S_OK,
          "detached target reclaimed");
  int next = thread::create(worker, &configs[1], ThreadCreateFlags::Detached);
  require(next > 0 && next != tid && thread::query(next, &out) == S_INVALID,
          "create detached");
  Yield(2);
  next = thread::create(worker, &configs[1]);
  require(next > 0, "zombie detach create");
  Yield(2);
  require(thread::detach(next) == S_OK && thread::query(next, &out) == S_BADHANDLE,
          "detach exited record");
}
struct JoinConfig { int target; volatile u32 entered; } joining;
int joiner(void* raw) {
  auto* c = (JoinConfig*)raw;
  c->entered = 1;
  joined(c->target, 42, 500);
  return 0;
}
bool prefix(const char* value, const char* text) {
  while (*text) { if (*value++ != *text++) { return false; } }
  return true;
}
void competition() {
  configs[0].gate = event::create(); configs[0].code = 42; configs[0].self = true;
  int target = thread::create(worker, &configs[0]);
  require(target > 0 && configs[0].gate > 0, "competing target");
  configs[0].tid = target;
  joining.target = target;
  int consumer = thread::create(joiner, &joining);
  require(consumer > 0, "join consumer");
  while (!joining.entered) { Yield(0); }
  ThreadResult out;
  require(thread::join(target, &out, 1) == S_BUSY && thread::query(target, &out) == S_BUSY &&
          thread::detach(target) == S_BUSY, "single pending consumer");
  char args[48]; copy(args, "/thread-foreign "); number((u32)target, args + strlen(args));
  int child = SpawnTracked("/boot/probe.snx", args);
  ProcessCompletion result;
  require(child > 0 && WaitProcess(child, &result, 100) == S_OK &&
          result.reason == PROCESS_EXITED && result.code == 0 && Close(child) == S_OK,
          "foreign TID rejected in child");
  require(event::set(configs[0].gate, true) == S_OK, "release competing target");
  joined(consumer, 0);
  require(thread::query(target, &out) == S_BADHANDLE && Close(configs[0].gate) == S_OK,
          "consumer wins exactly once");
}
void retiredCalls() {
  StartInfo launch;
  require(QueryStartInfo(&launch) == S_OK, "retired syscall policy");
  const int retired = launch.policy == PROCESS_STRICT ? S_DENIED : S_INVALID;
  require(api(0x111, (u32)worker, 0, 0) == retired && api(0x112, 0, 0, 0) == retired,
          "retired calls do not create or exit a thread");
}
void wireAndQuota() {
  require(api(SYS_THREAD_EXIT_CODE, 99, 1, 0) == S_INVALID, "exit reserved argument");
  ThreadCreateSpec spec{sizeof(spec), 1, (u32)worker, (u32)&configs[0], (u32)sino_thread_start, 0};
  require(api(SYS_THREAD_CREATE, 0xfffffff8u, sizeof(spec), 0) == S_INVALID &&
          api(SYS_THREAD_CREATE, (u32)&spec, sizeof(spec) - 1, 0) == S_INVALID &&
          api(SYS_THREAD_CREATE, (u32)&spec, sizeof(spec), 1) == S_INVALID, "create wire bounds");
  spec.version = 2;
  require(api(SYS_THREAD_CREATE, (u32)&spec, sizeof(spec), 0) == S_INVALID, "create version");
  spec.version = 1; spec.flags = 2;
  require(api(SYS_THREAD_CREATE, (u32)&spec, sizeof(spec), 0) == S_INVALID, "create flags");
  spec.flags = 0; spec.entry = (u32)&configs[0];
  require(api(SYS_THREAD_CREATE, (u32)&spec, sizeof(spec), 0) == S_INVALID, "writable worker rejected");
  spec.entry = (u32)worker; spec.trampoline = 0x100000;
  require(api(SYS_THREAD_CREATE, (u32)&spec, sizeof(spec), 0) == S_INVALID, "kernel trampoline rejected");
  spec.trampoline = (u32)sino_thread_start; spec.entry = USER_BASE;
  require(api(SYS_THREAD_CREATE, (u32)&spec, sizeof(spec), 0) == S_INVALID, "SNX header is not an entry point");
  spec.entry = (u32)worker; spec.trampoline = USER_ARGUMENTS;
  require(api(SYS_THREAD_CREATE, (u32)&spec, sizeof(spec), 0) == S_INVALID, "read-only parameters are not code");
  retiredCalls();
  int ids[3];
  const int gate = event::create();
  require(gate > 0, "stack inspection gate");
  for (u32 i = 0; i < 3; i++) {
    configs[i].code = (int)i;
    configs[i].gate = gate;
    ids[i] = createCaptured(worker, configs[i].stack);
    require(ids[i] > 0, "fill thread quota");
    started(configs[i]);
    const u32 top = configs[i].stack.top();
    const u32 previous = i ? configs[i - 1].stack.top() : USER_MAIN_STACK_TOP;
    require(top == previous + StackBytes + GuardBytes && top <= WorkerWindowEnd,
            "first fit reserves separate stacks and guards");
    for (u32 offset = StackBytes; offset < StackBytes + GuardBytes; offset += PageBytes) {
      require(api(SYS_LOG, top - offset - 1, 0, 1) == S_INVALID, "guard page inaccessible");
    }
  }
  require(thread::create(worker, &configs[0]) == S_LIMIT, "running quota");
  require(event::set(gate, true) == S_OK, "release inspected workers");
  Yield(2);
  require(thread::create(worker, &configs[0]) == S_LIMIT, "exit records retain bounded slots");
  ThreadResult out;
  require(api(SYS_THREAD_JOIN, (u32)ids[0], (u32)&out, 0) == S_INVALID &&
          thread::join(ids[0], &out, STREAM_WAIT_MAX + 1) == S_INVALID &&
          api(SYS_THREAD_DETACH, (u32)ids[0], 1, 0) == S_INVALID &&
          thread::query(1, &out) == S_BADHANDLE, "join/detach wire and legacy slot rejection");
  for (u32 i = 0; i < 3; i++) { joined(ids[i], (int)i); }
  require(Close(gate) == S_OK, "close stack inspection gate");
  configs[0].gate = 0;
  int next = thread::create(worker, &configs[0]);
  require(next > 0 && ((u32)next >> 3) == ((u32)ids[2] >> 3) + 1,
          "failed creates did not publish or consume TID");
  joined(next, 0);
}
void parked(bool fault) {
  int old = thread::create(worker, &configs[0]);
  require(old > 0, "unclaimed record setup");
  Yield(2);
  configs[1].gate = event::create(); configs[1].code = 42;
  int target = createCaptured(worker, configs[1].stack);
  require(target > 0 && configs[1].gate > 0, "park target");
  joining.target = target;
  require(thread::create(joiner, &joining) > 0, "park joiner");
  while (!joining.entered) { Yield(0); }
  if (fault) {
    started(configs[1]);
    const u32 guard = configs[1].stack.top() - StackBytes - sizeof(u32);
    Log("THREAD EXPECT FAULT\n");
    *(volatile u32*)guard = 1;
  }
  else { Log("THREAD CANCEL PARKED\n"); event::wait(configs[1].gate); }
  require(false, "park path returned");
}
void lastThread() {
  retiredCalls();
  int child = SpawnTracked("/boot/probe.snx", "/thread-last-child");
  ProcessCompletion out;
  require(child > 0 && WaitProcess(child, &out, 100) == S_OK &&
          out.reason == PROCESS_EXITED && out.code == 37 && Close(child) == S_OK,
          "unclaimed record does not keep process alive");
}
}
bool matches(const char* mode) { return prefix(mode, "/thread-"); }
int run(const char* mode) {
  if (streq(mode, "/thread-return")) { returning(); }
  else if (streq(mode, "/thread-record")) { exitedRecord(); }
  else if (streq(mode, "/thread-timeout")) { timeoutAndUnmap(); }
  else if (streq(mode, "/thread-detach")) { detached(); }
  else if (streq(mode, "/thread-compete")) { competition(); }
  else if (streq(mode, "/thread-wire")) { wireAndQuota(); }
  else if (streq(mode, "/thread-cancel")) { parked(false); }
  else if (streq(mode, "/thread-fault")) { parked(true); }
  else if (streq(mode, "/thread-last")) { lastThread(); }
  else if (streq(mode, "/thread-last-child")) {
    configs[0].delay = 2; configs[0].code = 37;
    require(thread::create(worker, &configs[0]) > 0, "last child worker");
    thread::exit(0);
  } else if (prefix(mode, "/thread-foreign ")) {
    u32 tid = 0;
    for (const char* p = mode + 16; *p; p++) {
      require(*p >= '0' && *p <= '9', "foreign TID argument");
      tid = tid * 10 + (u32)(*p - '0');
    }
    ThreadResult out;
    require(thread::join((int)tid, &out, 1) == S_BADHANDLE && thread::detach((int)tid) == S_BADHANDLE,
            "foreign TID must not resolve in current process");
  } else { require(false, "unknown mode"); }
  Log("THREAD PROBE PASS "); Log(mode); Log("\n");
  return 0;
}
}
