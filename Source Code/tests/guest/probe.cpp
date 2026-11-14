#include "sdk.h"
#include "scheduler_probe.h"
#include "event_probe.h"
static volatile int worker_done;
static int event_handle;
static u32 time_buffer[4];
static u32 check_count;
static void worker(void) {
  worker_done = 123;
  api(SYS_EVENT_SET, (u32)event_handle, 1, 0);
  api(SYS_THREAD_EXIT, 0, 0, 0);
  for (;;) {
  }
}
static void check(int ok, const char *s) {
  check_count++;
  if (!ok) {
    Log("USER PROBE FAIL: ");
    Log(s);
    Log("\n");
    Exit(1);
  }
}
#if MODE == 0
/* Text-only core contracts. */
static u8 contracts_data[MAX_FILE];
static void contracts_files(int verify_only) {
  const char *saved = "/home/user/contracts-persist.bin";
  if (!verify_only) {
    check(Mkdir("/home/user/contracts-api") == 0, "contracts mkdir");
    for (u32 i = 0; i < MAX_FILE; i++)
      contracts_data[i] = (u8)(i * 37u + 11u);
    check(Write("/home/user/contracts-api/a.bin", contracts_data, MAX_FILE) == 0,
          "64 KiB binary write");
    contracts_data[0] = 0xcc;
    check(Read("/home/user/contracts-api/a.bin", contracts_data, MAX_FILE - 1) ==
                  S_LIMIT && contracts_data[0] == 0xcc,
          "whole-file short buffer untouched");
    check(Read("/home/user/contracts-api/a.bin", contracts_data, MAX_FILE) == MAX_FILE,
          "64 KiB binary read");
    int equal = 1;
    for (u32 i = 0; i < MAX_FILE; i++)
      if (contracts_data[i] != (u8)(i * 37u + 11u))
        equal = 0;
    check(equal, "binary bytes including NUL preserved");
    check(Write("/home/user/contracts-api/a.bin", contracts_data, MAX_FILE + 1) ==
              S_INVALID, "file size bound");
    check(Write("/home/user/contracts-api/a.bin", "short", 5) == 0 &&
              Read("/home/user/contracts-api/a.bin", contracts_data, MAX_FILE) == 5,
          "overwrite shrinks file");
    check(api(SYS_MOVE, (u32)"/home/user/contracts-api/a.bin",
              (u32)"/home/user/contracts-api/b.bin", 0) == 0,
          "move file");
    FileInfo info[2];
    check(List("/home/user/contracts-api", info, 2) == 1 &&
              streq(info[0].name, "b.bin") && info[0].size == 5 && !info[0].dir,
          "list moved file");
    check(Remove("/home/user/contracts-api/b.bin", 0) == 0 &&
              Remove("/home/user/contracts-api", 1) == 0,
          "file and directory cleanup");
    for (u32 i = 0; i < 257; i++)
      contracts_data[i] = (u8)(i * 37u + 11u);
    check(Write(saved, contracts_data, 257) == 0 && Sync() == 0,
          "persistent binary commit");
  }
  check(Read(saved, contracts_data, MAX_FILE) == 257, "persistent binary length");
  int equal = 1;
  for (u32 i = 0; i < 257; i++)
    if (contracts_data[i] != (u8)(i * 37u + 11u))
      equal = 0;
  check(equal, "persistent binary content");
}
static void contracts_headless(void) {
  check(streq(Executable(), "/boot/probe.snx"), "immutable executable path");
  check(api(0xffffffffu, 0, 0, 0) == S_INVALID, "unknown syscall rejected");
  check(api(SYS_KILL, 0xdeadbeefu, 0, 0) == S_DENIED,
        "foreign process management denied");
  check(Read("/boot/readme.txt", (void *)USER_ARGUMENTS, 4) == S_INVALID,
        "readonly launch page output denied");
  check(Write("/home/user/contracts-invalid", (const void *)0x100000, 4) ==
              S_INVALID, "kernel source pointer denied");
  check(Read("/home/user/contracts-invalid", contracts_data, MAX_FILE) == S_NOTFOUND,
        "invalid write has no file side effect");
  Log("CONTRACT CHECK permissions-pointers PASS\n");
  int original = api(SYS_EVENT_CREATE, 0, 0, 0);
  check(original > 0, "event allocation");
  int read_only = api(SYS_DUP, (u32)original, 1, 0);
  check(read_only > 0 && api(SYS_EVENT_SET, (u32)read_only, 1, 0) == S_BADHANDLE,
        "duplicate rights reduced");
  check(api(SYS_EVENT_WAIT, (u32)read_only, 0, 0) == S_AGAIN &&
              api(SYS_EVENT_SET, (u32)original, 1, 0) == 0 &&
              Close(original) == 0 &&
              api(SYS_EVENT_WAIT, (u32)read_only, 0, 0) == 0,
        "duplicate retains event after original close");
  check(Close(read_only) == 0 && Close(read_only) == S_BADHANDLE,
        "duplicate stale close denied");
  int handles[24];
  for (int i = 0; i < 24; i++) {
    handles[i] = api(SYS_EVENT_CREATE, 0, 0, 0);
    check(handles[i] > 0, "fill local handle table");
  }
  check(api(SYS_EVENT_CREATE, 0, 0, 0) == S_LIMIT, "local handle limit");
  for (int i = 0; i < 24; i++)
    check(Close(handles[i]) == 0, "handle limit cleanup");
  Log("CONTRACT CHECK handles-rights-limit PASS\n");
  contracts_files(0);
  check(Remove("/home/user/probe.txt", 0) == 0, "shared probe file cleanup");
  Log("CONTRACT CHECK files-binary-limit-sync PASS\n");
  char count[12];
  number(check_count, count);
  Log("CONTRACT PROBE PASS checks=");
  Log(count);
  Log("\n");
}
#endif
int main(void) {
#if MODE == 1
  *(volatile u32 *)0x100000 = 1;
#elif MODE == 2
  __asm__ volatile("outb %%al,%%dx" ::"a"(255), "d"(0x21));
#elif MODE == 3
  for (;;)
    worker_done++;
#elif MODE == 4
  __asm__ volatile("ud2");
#else
  const char *arg = Arguments();
  if (eventprobe::matches(arg)) {
    return eventprobe::run(arg);
  }
  if (streq(arg, "/schedule")) {
    return schedulerprobe::run();
  }
  if (streq(arg, "/boot-extent")) {
    int directory = GrantDirectory("/boot", FS_READ | FS_LOOKUP);
    check(directory > 0, "boot extent directory");
    int file = OpenAt(directory, "extent.txt", FS_READ, 0);
    check(file > 0, "boot extent file");
    u8 range[24];
    check(FileReadAt(file, 1020, range, 23) == 23, "cross cluster range");
    const char *expected = "aaa\nEXPANDED TAIL PASS\n";
    for (u32 i = 0; i < 23; i++) check(range[i] == (u8)expected[i], "extent exact range byte");
    Close(file); Close(directory);
    Log("BOOT EXTENT RANGE PASS\n");
    return 0;
  }
  if (streq(arg, "/contracts-fail"))
    check(0, "intentional core runner failure");
  if (streq(arg, "/contracts-read")) {
    contracts_files(1);
    Log("CONTRACT PERSISTENCE PASS\n");
    return 0;
  }
  check(api(SYS_SOCKET, 1, 0, 0) == S_DENIED, "network default deny");
  for (int i = 0; i < 4; i++)
    time_buffer[i] = 0xccccccccu;
  check(api(SYS_TIME, (u32)time_buffer, 11, 0) == S_INVALID &&
            time_buffer[0] == 0xccccccccu,
        "time short buffer untouched");
  check(api(SYS_TIME, 0, 12, 0) == S_INVALID &&
            api(SYS_TIME, 0xffffffffu, 12, 0) == S_INVALID,
        "time invalid pointers");
  check(api(SYS_TIME, 0x100000, 12, 0) == S_INVALID, "time kernel output");
  check(api(SYS_TIME, USER_BASE, 12, 0) == S_INVALID,
        "time readonly code output");
  check(api(SYS_TIME, USER_STACK(0) - 8, 12, 0) == S_INVALID,
        "time stack guard crossing");
  check(api(SYS_TIME, (u32)time_buffer, sizeof(time_buffer), 0) == 0 &&
            time_buffer[0] < 24 && time_buffer[1] < 60 && time_buffer[2] < 60 &&
            time_buffer[3] == 0xccccccccu,
        "time bounded output");
  Log("TIME ABI PASS: pointer rights capacity and output bounds\n");
  check(Read("/boot/readme.txt", (void *)0x100000, 20) == S_INVALID,
        "kernel pointer");
  check(Write("/etc/hacked", "bad", 3) == S_DENIED, "protected directory");
  check(Write("/home/user/../../etc/hacked", "bad", 3) == S_DENIED,
        "canonical traversal");
  check(api(SYS_WINDOW, 0xffffffff, 300, 200) == S_INVALID, "string overflow");
  event_handle = api(SYS_EVENT_CREATE, 0, 0, 0);
  check(event_handle > 0, "event create");
  check(api(SYS_THREAD, (u32)worker, 0, 0) > 0, "thread creation");
  while (!worker_done)
    Yield(1);
  check(worker_done == 123 && api(SYS_EVENT_WAIT, (u32)event_handle, 0, 0) == 0,
        "thread shared address space/event");
  check(Close(event_handle) == 0 && Close(event_handle) == S_BADHANDLE,
        "closed handle");
  int next = api(SYS_EVENT_CREATE, 0, 0, 0);
  check(next > 0 && next != event_handle &&
            api(SYS_EVENT_SET, (u32)event_handle, 1, 0) == S_BADHANDLE,
        "stale generation");
  Close(next);
  check(Write("/home/user/probe.txt", "ring3 file\n", 11) == 0,
        "write user file");
  Log("RING3 PROBE PASS: threads events pointers permissions handles VFS\n");
  if (streq(arg, "/contracts")) {
    contracts_headless();
    return 0;
  }
#endif
  return 0;
}
