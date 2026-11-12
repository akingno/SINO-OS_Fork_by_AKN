/* Fixed wire requests build on Stage D too, so missing Stage E must fail. */
#include "sdk.h"
#ifdef FEATURE_PIPES
#include "stream.h"
#endif
#define PIPE_CREATE_CALL 0x147u
#define STREAM_READY_CALL 0x148u
#define PIPE_LIMITS_CALL 0x149u
#define PEER_CLOSED (-12)
#define WAIT_TIMEOUT (-13)
typedef struct { u32 read, write; } Pair;
typedef struct { RunSpec base; u32 directory, rights, dir_reserved[2], in, out, err, reserved; } Spec3;
static_assert(sizeof(Spec3) == 80, "RunSpec v3 wire size");
static u32 checks;
static u8 data[512], output[512];
static CliInfo cli;
static Pair quota[8];
static int held[24];
static void require(int ok, const char* why) {
  checks++;
  if (!ok) { Log("PIPE SELFTEST FAIL: "); Log(why); Log("\n"); Exit(1); }
}
static int make(u32 capacity, Pair* out) { return api(PIPE_CREATE_CALL, capacity, (u32)out, sizeof(*out)); }
static int ready(u32 h) { return api(STREAM_READY_CALL, h, 0, 0); }
static int transfer(int read, u32 h, const void* p, u32 bytes) {
  return api(read ? SYS_STREAM_READ : SYS_STREAM_WRITE, h, (u32)p, bytes);
}
static int waiting(int read, u32 h, void* p, u32 bytes) {
#ifdef FEATURE_PIPES
  return read ? StreamReadFor((int)h, p, bytes, 1000) : StreamWriteFor((int)h, p, bytes, 1000);
#else
  return transfer(read, h, p, bytes);
#endif
}
static int start(const char* mode, u32 in, u32 out, u32 err, u32 timeout, const char* path) {
  u32 offset = 0;
  Spec3 spec = {{80, 3, 0, (u32)path, 1, strlen(mode) + 1, (u32)mode, (u32)&offset,
                 0, 0, timeout, 0}, 0, 0, {0, 0}, in, out, err, 0};
  return api(SYS_RUN_START, (u32)&spec, sizeof(spec), 0);
}
static RunResult wait_result(int h, u32 reason) {
  require(h > 0, "explicit standard-stream launch");
  RunResult result; int status;
#ifdef FEATURE_PIPES
  status = RunWaitFor(h, &result, 2000);
#else
  u32 begin = (u32)api(SYS_TICKS, 0, 0, 0);
  while ((status = GetRunResult(h, &result)) == S_AGAIN &&
         (u32)api(SYS_TICKS, 0, 0, 0) - begin < 2000) Yield(1);
#endif
  require(status == 0 && result.completion.reason == reason && !result.completion.code,
          "owned completion reaches expected terminal reason");
  return result;
}
static u32 captured_count(int h) {
  u32 count = 0; RunRead request = {1, 0, (u32)&count, sizeof(count)};
  require(ReadRunOutput(h, &request) == 4, "separate final capture");
  return count;
}
#ifdef STREAM_BLOCK_FLAG
static int blocking_transfer(int reading, u32 handle, void* buffer, u32 bytes, u32 timeout) {
  u32 call = reading ? SYS_STREAM_READ : SYS_STREAM_WRITE;
  __asm__ volatile("int $0x80" : "+a"(call)
                   : "b"(handle), "c"(buffer), "d"(STREAM_BLOCK_FLAG | bytes), "S"(timeout)
                   : "memory", "cc");
  return (int)call;
}
typedef struct {
  int reading;
  u32 handle, timeout;
  u8* buffer;
  volatile int started, done, result;
  volatile u32 elapsed;
} WaitProbe;
static WaitProbe probes[2];
static void pipe_wait_worker(WaitProbe* probe) {
  u32 start = StreamTicks();
  probe->started = 1;
  probe->result = blocking_transfer(probe->reading, probe->handle, probe->buffer, 1, probe->timeout);
  probe->elapsed = StreamTicks() - start;
  probe->done = 1;
  api(SYS_THREAD_EXIT, 0, 0, 0);
}
static void first_waiter(void) { pipe_wait_worker(&probes[0]); }
static void second_waiter(void) { pipe_wait_worker(&probes[1]); }
static void begin_wait(int i, int reading, u32 handle, u8* buffer, u32 timeout) {
  probes[i] = (WaitProbe){.reading=reading, .handle=handle, .timeout=timeout, .buffer=buffer};
  require(api(SYS_THREAD, (u32)(i ? second_waiter : first_waiter), 0, 0) > 0, "start pipe waiter");
  u32 start = StreamTicks();
  while (!probes[i].started && StreamTicks() - start < 30) Yield(1);
  Yield(4);
  require(probes[i].started && !probes[i].done, "pipe call stays pending");
}
static int finish_wait(int i) {
  u32 start = StreamTicks();
  while (!probes[i].done && StreamTicks() - start < 100) Yield(1);
  require(probes[i].done, "pipe waiter finishes");
  return probes[i].result;
}
static int blocking_case(const char* mode) {
  Pair pair;
  require(make(1, &pair) == 0, "create blocking pipe");
  if (streq(mode, "read-wake")) {
    begin_wait(0, 1, pair.read, output, 0);
    require(transfer(0, pair.write, (void*)"R", 1) == 1 &&
            finish_wait(0) == 1 && output[0] == 'R', "write wakes reader with its byte");
  } else if (streq(mode, "write-wake") || streq(mode, "write-closed")) {
    require(transfer(0, pair.write, (void*)"A", 1) == 1, "fill pipe");
    data[0] = 'B';
    begin_wait(0, 0, pair.write, data, 0);
    if (streq(mode, "write-wake")) {
      require(transfer(1, pair.read, output, 1) == 1 && output[0] == 'A' &&
              finish_wait(0) == 1 && transfer(1, pair.read, output, 1) == 1 &&
              output[0] == 'B', "read wakes writer and preserves byte order");
    } else {
      require(Close((int)pair.read) == 0 && finish_wait(0) == S_CLOSED, "last reader close wakes writer");
      pair.read = 0;
    }
  } else if (streq(mode, "read-eof")) {
    int duplicate = api(SYS_DUP, pair.write, 2, 0);
    require(duplicate > 0, "duplicate writer");
    begin_wait(0, 1, pair.read, output, 0);
    require(Close((int)pair.write) == 0, "close one writer");
    pair.write = 0;
    Yield(4);
    require(!probes[0].done && Close(duplicate) == 0 && finish_wait(0) == 0, "last writer close wakes reader with EOF");
  } else if (streq(mode, "read-timeout")) {
    output[0] = 0x5a;
    u32 start = StreamTicks();
    require(blocking_transfer(1, pair.read, output, 1, 5) == S_TIMEOUT &&
            StreamTicks() - start >= 5 && output[0] == 0x5a, "timeout preserves read output");
  } else if (streq(mode, "contended-read")) {
    begin_wait(0, 1, pair.read, output, 40);
    begin_wait(1, 1, pair.read, output + 1, 40);
    require(transfer(0, pair.write, (void*)"R", 1) == 1, "wake both contenders");
    int a = finish_wait(0), b = finish_wait(1);
    require((a == 1 && output[0] == 'R' && b == S_TIMEOUT && probes[1].elapsed >= 40) ||
            (b == 1 && output[1] == 'R' && a == S_TIMEOUT && probes[0].elapsed >= 40),
            "only one reader consumes the byte; the other waits again");
  } else if (streq(mode, "local-close")) {
    begin_wait(0, 1, pair.read, output, 0);
    int duplicate = api(SYS_DUP, pair.read, 1, 0);
    require(duplicate > 0 && Close((int)pair.read) == 0, "close exact waited handle with duplicate alive");
    int replacement = api(SYS_DUP, (u32)duplicate, 1, 0);
    require(replacement > 0 && (replacement & 255) == (int)(pair.read & 255) &&
            finish_wait(0) == S_BADHANDLE && Close(replacement) == 0, "slot reuse cannot redirect wait");
    pair.read = (u32)duplicate;
  } else if (streq(mode, "unmap")) {
    int address = AnonMap(4096);
    require((u32)address >= USER_DYNAMIC_BASE &&
            (u32)address < USER_DYNAMIC_END, "map waiter buffer");
    begin_wait(0, 1, pair.read, (u8*)address, 0);
    require(AnonUnmap((void*)address) == 0 && transfer(0, pair.write, (void*)"U", 1) == 1 &&
            finish_wait(0) == S_INVALID && transfer(1, pair.read, output, 1) == 1 &&
            output[0] == 'U', "retry revalidates the buffer before consuming pipe data");
  } else if (streq(mode, "wire")) {
    u32 address = (u32)AnonMap(RUN_IO_MAX);
    require(address >= USER_DYNAMIC_BASE && address < USER_DYNAMIC_END, "map maximum transfer buffer");
    require(blocking_transfer(1, pair.read, (void*)0, 0, 0) == 0 &&
            blocking_transfer(1, pair.read, output, RUN_IO_MAX + 1, 1) == S_INVALID &&
            blocking_transfer(1, pair.read, output, 1, STREAM_WAIT_MAX + 1) == S_INVALID &&
            blocking_transfer(1, pair.write, output, 1, 1) == S_BADHANDLE &&
            blocking_transfer(1, cli.in, output, 1, 1) == S_BADHANDLE &&
            StreamRead((int)cli.in, output, 1) == 0, "blocking wire argument validation");
    require(transfer(0, pair.write, (void*)"M", 1) == 1 &&
            blocking_transfer(1, pair.read, (void*)address, RUN_IO_MAX, STREAM_WAIT_MAX) == 1 &&
            *(u8*)address == 'M' && AnonUnmap((void*)address) == 0, "maximum size and timeout retain short transfers");
  } else {
    require(0, "unknown blocking pipe case");
  }
  require((!pair.read || Close((int)pair.read) == 0) &&
          (!pair.write || Close((int)pair.write) == 0), "release pipe case endpoints");
  return 0;
}
static int blocking_release_case(void) {
  Pair pair;
  require(make(1, &pair) == 0 && transfer(0, pair.write, (void*)"P", 1) == 1, "fill child pipe");
  int child = start("block-child", 0, pair.write, 0, 20, "/boot/pipetest.snx");
  require(child > 0 && Close((int)pair.write) == 0, "child owns last writer");
  wait_result(child, PROCESS_TIMEOUT);
  require(transfer(1, pair.read, output, 1) == 1 && output[0] == 'P' &&
          transfer(1, pair.read, output, 1) == 0 && Close((int)pair.read) == 0 && Close(child) == 0,
          "timeout of an all-blocked process releases its writer");
  Log("PIPE BLOCK RELEASE PASS\n");
  return 0;
}
#endif
static int child(void) {
  require(QueryCliInfo(&cli) == 0 && cli.argc == 1, "child launch context");
  const char* mode = cli.bytes + cli.offsets[0];
  StartInfo info;
  require(QueryStartInfo(&info) == 0 && info.policy == PROCESS_STRICT && !info.caps &&
          Spawn("/boot/pipetest.snx") == S_DENIED, "pipe binding does not downgrade policy");
#ifdef STREAM_BLOCK_FLAG
  if (streq(mode, "block-child")) {
    Log("PIPE BLOCK CHILD ENTER\n");
    blocking_transfer(0, cli.out, (void*)"X", 1, 0);
    require(0, "blocked child must time out at process level");
  }
  if (mode[0] == 'b' && mode[1] == ':') return blocking_case(mode + 2);
#endif
  if (streq(mode, "close-reader")) { require(Close((int)cli.in) == 0, "early reader close"); return 0; }
  if (streq(mode, "shared-writer")) {
    require(StreamWrite((int)cli.out, "X", 1) == 1 && Close((int)cli.out) == 0,
            "close stdout while stderr shares writer");
    Yield(2);
    require(StreamWrite((int)cli.err, "Y", 1) == 1 && Close((int)cli.err) == 0,
            "stderr retains writer until its own close");
    Yield(2); return 0;
  }
  if (streq(mode, "blocked-writer")) {
    waiting(0, cli.out, (void*)"x", 1);
    require(0, "blocked writer must be canceled or timed out");
  }
  if (streq(mode, "foreign") || (mode[0] == 'f' && mode[1] == ':')) {
    if (mode[1] == ':') {
      u32 handle = 0;
      for (u32 i = 2; mode[i]; i++) handle = handle * 10 + (u32)(mode[i] - '0');
      require(ready(handle) == S_BADHANDLE && transfer(1, handle, output, 1) == S_BADHANDLE,
              "parent handle number is not a child grant");
    }
    require(transfer(1, cli.in, output, 0) == 0 && ready(cli.in) == (1 | 4), "finite EOF readiness");
    Pair local;
    require(make(2, &local) == 0 && Close((int)local.read) == 0 && Close((int)local.write) == 0,
            "strict process can create local pipe objects");
    return 0;
  }
  int producer = streq(mode, "produce") || streq(mode, "expect-closed") || streq(mode, "fault-writer");
  u32 total = streq(mode, "fault-writer") ? 31u : 12289u, count = 0;
  if (producer) {
    while (count < total) {
      u32 chunk = total - count; if (chunk > 113) chunk = 113;
      for (u32 i = 0; i < chunk; i++) data[i] = (u8)((count + i) * 37 + 11);
      int n = waiting(0, cli.out, data, chunk);
      if (n == PEER_CLOSED && streq(mode, "expect-closed")) return 0;
      require(n > 0 && (u32)n <= chunk, "producer makes bounded progress"); count += (u32)n;
    }
    if (streq(mode, "fault-writer")) __asm__ volatile("ud2");
    require(!streq(mode, "expect-closed"), "producer must observe closed reader");
  } else {
    for (;;) {
      int n = waiting(1, cli.in, output, 43);
      require(n >= 0, "consumer does not stall or fail");
      if (!n) break;
      for (int i = 0; i < n; i++) require(output[i] == (u8)((count + (u32)i) * 37 + 11), "binary pipe byte ordering");
      count += (u32)n;
    }
    require(streq(mode, "drain") ? count == 31 : count == total, "EOF only after every producer byte");
    require(StreamWrite((int)cli.out, &count, 4) == 4, "consumer finite capture remains available");
  }
  return 0;
}
int main(void) {
#ifdef STREAM_BLOCK_FLAG
  if (streq(Arguments(), "block-release")) return blocking_release_case();
#endif
  if (!streq(Arguments(), "suite")) return child();
  u32 limits[9]; for (u32 i = 0; i < 9; i++) limits[i] = 0xccccccccu;
  require(api(PIPE_LIMITS_CALL, (u32)limits, 36, 0) == 0 && limits[0] == 32 &&
          limits[1] == 1 && limits[2] == 8 && limits[3] == 4096 && limits[4] == 65536 &&
          limits[5] == 60000 && !limits[7] && limits[8] == 0xccccccccu, "pipe limits wire contract");
  SystemInfo system;
  require(QuerySystemInfo(&system) == 0 && (system.features & 64), "pipe feature advertised");
#ifdef FEATURE_THREAD_WAIT
  StartInfo startup;
  require(ThreadWaitAvailable() && QueryStartInfo(&startup) == 0 &&
          (startup.features & FEATURE_THREAD_WAIT), "blocking wait discovery from both queries");
#endif
  Pair pair = {0xccccccccu, 0xccccccccu};
  require(make(0, &pair) == S_INVALID && make(4097, &pair) == S_INVALID && pair.read == 0xccccccccu,
          "capacity errors preserve output");
  require(api(PIPE_CREATE_CALL, 1, USER_BASE, 8) == S_INVALID &&
          api(PIPE_CREATE_CALL, 1, (u32)&pair, 7) == S_INVALID &&
          api(PIPE_LIMITS_CALL, (u32)limits, 31, 0) == S_INVALID &&
          api(PIPE_LIMITS_CALL, (u32)limits, 32, 1) == S_INVALID, "request and output memory validation");
  require(make(3, &pair) == 0 && pair.read > 2 && pair.write > 2 && pair.read != pair.write, "opaque endpoints");
  require(ready(pair.read) == 0 && ready(pair.write) == 2 &&
          api(STREAM_READY_CALL, pair.read, 1, 0) == S_INVALID, "readiness and reserved flags");
  require(transfer(1, pair.read, output, 1) == S_AGAIN && transfer(0, pair.write, (void*)"abcde", 5) == 3 &&
          transfer(0, pair.write, (void*)"x", 1) == S_AGAIN && ready(pair.write) == 0, "short write and full backpressure");
  require(transfer(1, pair.read, (void*)USER_BASE, 1) == S_INVALID &&
          transfer(1, pair.read, output, 2) == 2 && output[0] == 'a' && output[1] == 'b' &&
          transfer(0, pair.write, (void*)"de", 2) == 2 &&
          transfer(1, pair.read, output, 3) == 3 && output[0] == 'c' && output[1] == 'd' && output[2] == 'e',
          "invalid read does not consume and circular buffer wraps");
  require(transfer(0, pair.read, data, 1) == S_BADHANDLE && transfer(1, pair.write, output, 1) == S_BADHANDLE &&
          transfer(0, pair.write, (void*)0x100000, 1) == S_INVALID &&
          transfer(0, pair.write, data, 65537) == S_INVALID, "directions and input bounds");
  int duplicate = api(SYS_DUP, pair.write, 2, 0);
  require(api(SYS_DUP, pair.read, 2, 0) == S_BADHANDLE &&
          api(SYS_DUP, pair.write, 1, 0) == S_BADHANDLE, "duplication cannot change endpoint direction");
  require(duplicate > 0 && Close((int)pair.write) == 0 && transfer(1, pair.read, output, 1) == S_AGAIN &&
          transfer(0, (u32)duplicate, (void*)"\0Z", 2) == 2 && Close(duplicate) == 0,
          "all writer references control EOF");
  require(ready(pair.read) == (1 | 8) && transfer(1, pair.read, output, 4) == 2 && !output[0] && output[1] == 'Z' &&
          ready(pair.read) == (1 | 4 | 8) && transfer(1, pair.read, output, 1) == 0 &&
          transfer(1, pair.read, 0, 0) == 0 && Close((int)pair.read) == 0 && ready(pair.read) == S_BADHANDLE,
          "buffer drains before repeatable EOF and stale handles fail");
  require(make(1, &pair) == 0 && Close((int)pair.read) == 0 && ready(pair.write) == 8 &&
          transfer(0, pair.write, data, 1) == PEER_CLOSED && transfer(0, pair.write, 0, 0) == 0 &&
          Close((int)pair.write) == 0, "last reader close is explicit peer error");
  for (u32 i = 0; i < 8; i++) require(make(1, &quota[i]) == 0, "bounded pipe quota");
  pair.read = pair.write = 0xccccccccu;
  require(make(1, &pair) == S_LIMIT && pair.read == 0xccccccccu, "quota failure is transactional");
  for (u32 i = 0; i < 8; i++) require(Close((int)quota[i].read) == 0 && Close((int)quota[i].write) == 0, "quota release");
  for (u32 i = 0; i < 23; i++) { held[i] = api(SYS_EVENT_CREATE, 0, 0, 0); require(held[i] > 0, "fill local handles"); }
  require(make(1, &pair) == S_LIMIT && pair.read == 0xccccccccu, "second endpoint allocation rolls back first");
  for (u32 i = 0; i < 23; i++) require(Close(held[i]) == 0, "release handle pressure");
  require(make(257, &pair) == 0, "pipeline capacity");
  u32 offset = 0; const char* mode = "consume";
  Spec3 invalid = {{80, 3, 0, (u32)"/boot/pipetest.snx", 1, strlen(mode) + 1, (u32)mode,
                    (u32)&offset, 0, 0, 100, 0}, 0, 0, {0, 0}, pair.read, 0, 0, 1};
  require(api(SYS_RUN_START, (u32)&invalid, 80, 0) == S_INVALID, "v3 reserved word rejected");
  invalid.reserved = 0; invalid.base.version = 2;
  require(api(SYS_RUN_START, (u32)&invalid, 80, 0) == S_INVALID, "v3 version selector required");
  invalid.base.version = 3; invalid.base.input_bytes = 1; invalid.base.input = (u32)data;
  require(api(SYS_RUN_START, (u32)&invalid, 80, 0) == S_INVALID, "pipe stdin cannot silently replace finite input");
  require(start("produce", pair.write, 0, 0, 2000, "/boot/pipetest.snx") == S_BADHANDLE &&
          start("produce", 0, pair.read, 0, 2000, "/boot/pipetest.snx") == S_BADHANDLE &&
          start("consume", pair.read, pair.read, 0, 2000, "/boot/pipetest.snx") == S_BADHANDLE &&
          start("produce", 0, pair.write, 0, 2000, "/boot/missing.snx") == S_NOTFOUND,
          "wrong direction and failed image binding roll back");
  int producer = start("produce", 0, pair.write, 0, 2000, "/boot/pipetest.snx");
  int consumer = start("consume", pair.read, 0, 0, 2000, "/boot/pipetest.snx");
  require(producer > 0 && consumer > 0 && Close((int)pair.read) == 0 && Close((int)pair.write) == 0,
          "start both processes before waiting and close parent copies");
  RunResult r = wait_result(producer, PROCESS_EXITED);
  require(!r.out_bytes && !r.incomplete && Close(producer) == 0, "pipe output bypasses finite capture");
  wait_result(consumer, PROCESS_EXITED);
  require(captured_count(consumer) == 12289 && Close(consumer) == 0, "larger-than-capture binary stream");
  require(make(17, &pair) == 0, "early reader pipeline");
  producer = start("expect-closed", 0, pair.write, 0, 2000, "/boot/pipetest.snx");
  consumer = start("close-reader", pair.read, 0, 0, 2000, "/boot/pipetest.snx");
  require(Close((int)pair.read) == 0 && Close((int)pair.write) == 0, "remove parent reader reference");
  wait_result(producer, PROCESS_EXITED); wait_result(consumer, PROCESS_EXITED);
  require(Close(producer) == 0 && Close(consumer) == 0, "early close does not deadlock");
  require(make(3, &pair) == 0, "shared stdout stderr pipe");
  producer = start("shared-writer", 0, pair.write, pair.write, 100, "/boot/pipetest.snx");
  require(producer > 0 && Close((int)pair.write) == 0, "both role bindings transferred");
  u32 received = 0;
  while (received < 2) {
    int n = waiting(1, pair.read, output + received, 2 - received);
    require(n > 0, "one role close must not truncate the other"); received += (u32)n;
  }
  require(output[0] == 'X' && output[1] == 'Y' && waiting(1, pair.read, output, 1) == 0 &&
          Close((int)pair.read) == 0, "all role references determine EOF");
  r = wait_result(producer, PROCESS_EXITED);
  require(!r.out_bytes && !r.err_bytes && Close(producer) == 0, "pipe roles do not create captures");
  require(make(257, &pair) == 0, "fault pipeline");
  producer = start("fault-writer", 0, pair.write, 0, 2000, "/boot/pipetest.snx");
  consumer = start("drain", pair.read, 0, 0, 2000, "/boot/pipetest.snx");
  require(Close((int)pair.read) == 0 && Close((int)pair.write) == 0, "fault parent copies closed");
  r = wait_result(producer, PROCESS_FAULTED);
  require(r.completion.vector == 6 && Close(producer) == 0, "fault remains typed");
  wait_result(consumer, PROCESS_EXITED);
  require(captured_count(consumer) == 31 && Close(consumer) == 0, "fault closes writer after preserving bytes");
  for (int timed = 0; timed < 2; timed++) {
    require(make(1, &pair) == 0 && transfer(0, pair.write, (void*)"p", 1) == 1, "blocked writer setup");
    producer = start("blocked-writer", 0, pair.write, 0, timed ? 5 : 0, "/boot/pipetest.snx");
    require(producer > 0 && Close((int)pair.write) == 0, "blocked child owns last writer");
#ifdef FEATURE_PIPES
    RunResult pending; ProcessCompletion pending_process;
    memset(&pending, 0xcc, sizeof(pending)); memset(&pending_process, 0xcc, sizeof(pending_process));
    if (!timed) require(RunWaitFor(producer, &pending, 0) == WAIT_TIMEOUT && pending.completion.size == 0xccccccccu &&
            ProcessWaitFor(producer, &pending_process, 0) == WAIT_TIMEOUT && pending_process.size == 0xccccccccu,
            "wait timeout preserves incomplete result buffers");
#endif
    if (!timed) { Yield(2); require(CancelProcess(producer) == 0, "owned cancellation"); }
    wait_result(producer, timed ? PROCESS_TIMEOUT : PROCESS_CANCELED);
    require(transfer(1, pair.read, output, 2) == 1 && output[0] == 'p' &&
            transfer(1, pair.read, output, 1) == 0 && Close((int)pair.read) == 0 && Close(producer) == 0,
            "cancel or timeout releases blocked endpoint");
  }
  producer = start("foreign", 0, 0, 0, 100, "/boot/pipetest.snx");
  wait_result(producer, PROCESS_EXITED); require(Close(producer) == 0, "unbound v3 finite streams compatible");
  require(make(1, &pair) == 0, "foreign handle setup");
  char foreign[32]; foreign[0] = 'f'; foreign[1] = ':'; number(pair.read, foreign + 2);
  producer = start(foreign, 0, 0, 0, 100, "/boot/pipetest.snx");
  wait_result(producer, PROCESS_EXITED);
  require(Close(producer) == 0 && Close((int)pair.read) == 0 && Close((int)pair.write) == 0,
          "foreign handle test releases owner endpoints");
#ifdef FEATURE_PIPES
  require(StreamTimeoutExpired(0x20u, 0xfffffff0u, 0x30u) &&
          !StreamTimeoutExpired(0x20u, 0xfffffff0u, 0x31u), "monotonic deadline handles tick wrap");
  require(make(1, &pair) == 0, "runtime wait setup");
  u32 written = 99;
  require(StreamReadFor((int)pair.read, output, 1, 2) == WAIT_TIMEOUT &&
          StreamWaitFor((int)pair.read, 1, 0) == WAIT_TIMEOUT &&
          StreamWriteAllFor((int)pair.write, "ab", 2, 0, &written) == WAIT_TIMEOUT && written == 1,
          "runtime waits are bounded and retain partial progress");
  require(StreamReadFor((int)pair.read, output, 1, 0) == 1 && output[0] == 'a' &&
          StreamWaitFor((int)pair.write, 2, 0) == 2 &&
          StreamReadFor((int)pair.read, output, 1, 60001) == S_INVALID &&
          Close((int)pair.read) == 0 && StreamWriteAllFor((int)pair.write, "x", 1, 5, &written) == PEER_CLOSED &&
          !written && Close((int)pair.write) == 0, "runtime errors and readiness");
#ifdef FEATURE_THREAD_WAIT
  require(make(1, &pair) == S_OK, "blocked write-all setup");
  u32 begin = StreamTicks();
  require(StreamWriteAllFor((int)pair.write, "ab", 2, 8, &written) == S_TIMEOUT &&
          written == 1 && StreamTicks() - begin >= 8 &&
          StreamReadFor((int)pair.read, output, 2, 0) == 1 && output[0] == 'a' &&
          Close((int)pair.read) == S_OK && Close((int)pair.write) == S_OK,
          "write-all blocks for the remaining deadline and reports partial progress");
#endif
#endif
  Log("PIPE SUITE PASS checks="); char text[16]; number(checks, text); Log(text); Log("\n");
  return 0;
}
