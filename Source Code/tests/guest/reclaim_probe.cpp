#include "stream.h"

static_assert(SYS_PROCESS_GROUP == 0x157 && FEATURE_PROCESS_GROUP == 262144u, "process group ABI");
static_assert(SYS_ANON_PURGEABLE == 0x158 && SYS_ANON_LOCK == 0x159 &&
              SYS_ANON_UNLOCK == 0x15a && FEATURE_PURGEABLE == 524288u, "purgeable ABI");

namespace {
constexpr u32 PageBytes = 4096;

void require(bool ok, const char* reason) {
  if (!ok) {
    Log("RECLAIM SELFTEST FAIL: ");
    Log(reason);
    Log("\n");
    Exit(1);
  }
}

[[noreturn]] void hold() {
  for (;;) { Yield(100); }
}

volatile u8* reserve(u32 pages) {
  const int base = AnonMap(pages * PageBytes);
  require(base > 0, "reserve anonymous memory before pressure");
  return (volatile u8*)(u32)base;
}

int queryGroup() {
  SystemInfo info;
  const int group = ProcessGroup();
  require(QuerySystemInfo(&info) == S_OK && (info.features & FEATURE_PROCESS_GROUP) && group > 0,
          "process group feature and positive self query");
  require(api(0x157, 1, 0, 0) == S_INVALID && api(0x157, 0, 1, 0) == S_INVALID &&
          api(0x157, 0, 0, 1) == S_INVALID, "process group rejects arguments");
  return group;
}

int startGroupChild(const char* mode) {
  const u32 offset = 0;
  const RunSpec spec = {sizeof(spec), 1, 0, (u32)"/boot/reclaim.snx", 1,
                       strlen(mode) + 1, (u32)mode, (u32)&offset, 0, 0, 0, 0};
  const int child = StartRun(&spec);
  require(child > 0, "start managed group child");
  return child;
}

void logGroup(const char* label, int group) {
  char text[11];
  number((u32)group, text);
  Log(label);
  Log(text);
  Log("\n");
}

void waitForPressure(bool expected) {
  const u32 start = StreamTicks();
  for (;;) {
    const int mapped = AnonMap(1);
    require(mapped == S_NOMEM || (mapped > 0 && AnonUnmap((void*)(u32)mapped) == S_OK),
            "poll pressure with a temporary reservation");
    if ((mapped == S_NOMEM) == expected) { return; }
    require(StreamTicks() - start < 900, "pressure did not reach the requested state");
    Yield(1);
  }
}

void waitForDiscard(const void* base) {
  const u32 start = StreamTicks();
  for (;;) {
    const int status = AnonQuery(base);
    if (status == S_CHANGED) { return; }
    require(status == S_OK && StreamTicks() - start < 900, "purge status did not change");
    Yield(1);
  }
}

int purgeAbi() {
  SystemInfo info;
  require(QuerySystemInfo(&info) == S_OK && (info.features & FEATURE_PURGEABLE), "purgeable feature bit");
  void* base = (void*)reserve(2);
  const u32 address = (u32)base;
  require(AnonQuery(nullptr) == S_INVALID && AnonQuery((void*)(address + PageBytes)) == S_INVALID &&
          AnonLock((void*)(address + 1)) == S_INVALID && AnonLock(base) == S_INVALID &&
          api(0x158, address, 3, 0) == S_INVALID && api(0x158, address, 1, 1) == S_INVALID &&
          api(0x159, address, 1, 0) == S_INVALID && api(0x15a, address, 0, 1) == S_INVALID,
          "purge calls require an exact base and valid arguments");
  require(AnonQuery(base) == S_OK && AnonSetPurgeable(base, true) == S_OK &&
          AnonSetPurgeable(base, true) == S_OK && AnonUnlock(base) == S_INVALID,
          "a newly marked region starts unlocked");
  require(AnonLock(base) == S_OK && AnonLock(base) == S_OK && AnonUnlock(base) == S_OK &&
          AnonSetPurgeable(base, false) == S_BUSY && AnonUnmap(base) == S_BUSY &&
          AnonUnlock(base) == S_OK && AnonSetPurgeable(base, false) == S_OK &&
          AnonLock(base) == S_INVALID && AnonUnmap(base) == S_OK && AnonQuery(base) == S_INVALID,
          "nested locks pin the region until the last release");

  base = (void*)reserve(1);
  require(AnonSetPurgeable(base, true) == S_OK, "mark lock saturation fixture");
  for (u32 i = 0; i < 0xffffu; ++i) { require(AnonLock(base) == S_OK, "lock count up to maximum"); }
  require(AnonLock(base) == S_LIMIT && AnonUnlock(base) == S_OK && AnonLock(base) == S_OK &&
          AnonLock(base) == S_LIMIT, "overflow leaves lock count intact");
  Log("PURGE ABI PASS\n");
  return 0;
}

int purgeCache() {
  volatile u8* cache = reserve(3);
  volatile u8* pinned = reserve(1);
  volatile u8* ordinary = reserve(1);
  void* empty = (void*)reserve(1);
  void* base = (void*)cache;
  cache[0] = cache[PageBytes] = cache[2 * PageBytes] = 0x6d;
  *pinned = 0xab;
  *ordinary = 0xcd;
  require(AnonSetPurgeable(base, true) == S_OK && AnonSetPurgeable((void*)pinned, true) == S_OK &&
          AnonLock((void*)pinned) == S_OK && AnonSetPurgeable(empty, true) == S_OK,
          "seed one discardable, one pinned and one empty cache");
  Log("PURGE CACHE READY\n");
  waitForDiscard(base);
  require(AnonQuery((void*)pinned) == S_OK && *pinned == 0xab && *ordinary == 0xcd &&
          AnonQuery(empty) == S_OK, "pin, ordinary neighbor and empty reservation survive");
  Log("PURGE CACHE DROPPED\n");
  waitForPressure(false);
  require(AnonSetPurgeable(base, true) == S_BUSY && AnonSetPurgeable(base, false) == S_OK &&
          AnonQuery(base) == S_CHANGED && AnonSetPurgeable(base, true) == S_OK &&
          AnonLock(base) == S_OK && AnonQuery(base) == S_CHANGED, "loss stays visible through mode and lock changes");
  auto* info = (SystemInfo*)(cache + PageBytes);
  require(cache[0] == 0 && cache[2 * PageBytes] == 0 && QuerySystemInfo(info) == S_OK &&
          info->size == sizeof(*info) && cache[2 * PageBytes - 1] == 0 && AnonQuery(base) == S_CHANGED,
          "user touch and syscall preparation repopulate with zeroed pages");
  cache[0] = 0xa4;
  require(AnonSetPurgeable(base, true) == S_OK && AnonQuery(base) == S_OK &&
          AnonUnlock(base) == S_OK && cache[0] == 0xa4 && AnonUnmap(base) == S_OK &&
          AnonUnlock((void*)pinned) == S_OK, "locked rebuild acknowledges the loss");
  Log("PURGE CACHE PASS\n");
  return 0;
}

[[noreturn]] void purgeUnlock() {
  volatile u8* cache = reserve(1);
  void* base = (void*)cache;
  *cache = 0x41;
  require(AnonSetPurgeable(base, true) == S_OK && AnonLock(base) == S_OK && AnonLock(base) == S_OK,
          "pin before pressure");
  Log("PURGE UNLOCK READY\n");
  waitForPressure(true);
  require(AnonUnlock(base) == S_OK, "drop the first pin");
  Yield(5);
  require(AnonQuery(base) == S_OK && *cache == 0x41 && AnonUnlock(base) == S_OK,
          "last pin alone makes resident pages eligible");
  waitForDiscard(base);
  require(AnonLock(base) == S_OK && *cache == 0 && AnonQuery(base) == S_CHANGED,
          "repopulate without acknowledging the loss");
  *cache = 0x29;
  require(AnonUnlock(base) == S_OK, "unacknowledged pages remain eligible");
  Log("PURGE UNLOCK PASS\n");
  hold();
}
}

int main() {
  const char* mode = Arguments();
  CliInfo cli;
  char* argv[2];
  if (!*mode && CliArguments(&cli, argv, 2) == 1) { mode = argv[0]; }

  if (streq(mode, "purge-abi")) { return purgeAbi(); }
  if (streq(mode, "purge-cache")) { return purgeCache(); }
  if (streq(mode, "purge-unlock")) { purgeUnlock(); }
  if (streq(mode, "purge-hold") || streq(mode, "purge-hold-large")) {
    const u32 pages = streq(mode, "purge-hold") ? 2 : 4;
    volatile u8* memory = reserve(pages);
    for (u32 i = 0; i < pages; ++i) { memory[i * PageBytes] = 1; }
    require(AnonSetPurgeable((void*)memory, true) == S_OK, "mark resident cache");
    Log("PURGE HOLD READY\n");
    hold();
  }

  if (streq(mode, "group-id")) { return queryGroup(); }
  if (streq(mode, "group-hold")) {
    volatile u8* memory = reserve(2);
    memory[0] = memory[PageBytes] = 1;
    logGroup("RECLAIM GROUP MEMBER ", queryGroup());
    hold();
  }
  if (streq(mode, "group-parent") || streq(mode, "group-parent-max")) {
    if (streq(mode, "group-parent-max")) {
      require(SetReclaimPriority(350) == S_OK, "raise parent score before managed run");
    }
    const int group = queryGroup();
    const int shortChild = startGroupChild("group-id");
    ProcessCompletion result;
    require(ProcessWaitFor(shortChild, &result, 500) == S_OK && result.pid != (u32)group &&
            result.reason == PROCESS_EXITED && result.code == group,
            "managed child inherits group and publishes its own completion");
    require(Close(shortChild) == S_OK, "close completed child handle");
    volatile u8* memory = reserve(1);
    *memory = 1;
    startGroupChild("group-hold");
    logGroup("RECLAIM GROUP PARENT ", group);
    hold();
  }

  if (streq(mode, "low")) {
    volatile u8* reserved = reserve(2);
    Log("RECLAIM LOW READY\n");
    const u32 start = (u32)api(SYS_TICKS, 0, 0, 0);
    for (;;) {
      const int base = AnonMap(1);
      if (base == S_NOMEM) { break; }
      require(base > 0 && AnonUnmap((void*)(u32)base) == S_OK, "poll reservation");
      require((u32)api(SYS_TICKS, 0, 0, 0) - start < 900, "LOW did not arrive");
      Yield(1);
    }
    require(AnonMap(0) == S_INVALID && AnonMap(ANON_MAX_BYTES) == S_LIMIT,
            "invalid request and quota retain precedence");
    require(reserved[0] == 0 && reserved[PageBytes] == 0, "existing region can still populate");
    reserved[PageBytes] = 0x6d;
    require(reserved[PageBytes] == 0x6d && AnonUnmap((void*)reserved) == S_OK,
            "existing region remains writable and can be unmapped");
    Log("RECLAIM LOW PASS\n");
    return 0;
  }

  if (streq(mode, "retry") || streq(mode, "fault") || streq(mode, "table-fault")) {
    volatile u8* memory = reserve(1);
    Log("RECLAIM TOUCH READY\n");
    require(*memory == 0, "first touch receives a zero page");
    *memory = 0x6d;
    require(*memory == 0x6d && streq(mode, "retry"), "only retry mode may resume");
    Log("RECLAIM TOUCH PASS\n");
    hold();
  }

  if (streq(mode, "copy") || streq(mode, "table-copy")) {
    volatile u8* memory = reserve(1);
    require(QuerySystemInfo((SystemInfo*)memory) == S_NOMEM, "system call preserves S_NOMEM");
    Log(streq(mode, "copy") ? "RECLAIM COPY PASS\n" : "RECLAIM TABLE COPY PASS\n");
    hold();
  }

  u32 pages = 0;
  if (streq(mode, "hold-small")) { pages = 2; }
  if (streq(mode, "hold-large")) { pages = 16; }
  if (streq(mode, "protect")) { pages = 24; }
  require(pages > 0, "unknown reclaim probe mode");
  volatile u8* memory = reserve(pages);
  for (u32 i = 0; i < pages; ++i) { memory[i * PageBytes] = 1; }
  Log("RECLAIM HOLD READY\n");
  hold();
}
