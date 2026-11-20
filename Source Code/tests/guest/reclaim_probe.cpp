#include "sdk.h"

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
}

int main() {
  const char* mode = Arguments();
  CliInfo cli;
  char* argv[2];
  if (!*mode && CliArguments(&cli, argv, 2) == 1) { mode = argv[0]; }

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
