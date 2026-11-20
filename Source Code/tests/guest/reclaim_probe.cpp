#include "stream.h"

static_assert(SYS_PROCESS_GROUP == 0x157 && FEATURE_PROCESS_GROUP == 262144u, "process group ABI");

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
}

int main() {
  const char* mode = Arguments();
  CliInfo cli;
  char* argv[2];
  if (!*mode && CliArguments(&cli, argv, 2) == 1) { mode = argv[0]; }

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
