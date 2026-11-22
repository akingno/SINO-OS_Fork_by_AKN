#ifndef SINO_THREAD_PROBE_H
#define SINO_THREAD_PROBE_H
#include "sdk.h"

namespace threadprobe {
inline constexpr u32 PageBytes = 4096;
inline constexpr u32 StackBytes = 16 * 1024;
inline constexpr u32 GuardBytes = 16 * 1024;
inline constexpr u32 WorkerWindowEnd = USER_MAIN_STACK_TOP + 0x20000u;

struct StackSample {
  volatile u32 esp;
  u32 returnAddress, entry, argument;
  u32 top() const { return esp + 3 * sizeof(u32); }
};
static_assert(sizeof(StackSample) == 16, "thread probe sample layout");

// The worker argument points to the sample, possibly as the first member of its config.
__attribute__((naked, noinline)) inline void thread_probe_capture() {
  __asm__ volatile("movl 8(%esp),%edx; movl (%esp),%eax; movl %eax,4(%edx);"
                   "movl 4(%esp),%eax; movl %eax,8(%edx); movl %edx,12(%edx);"
                   "movl %esp,(%edx); jmp _sino_thread_start");
}

inline int createCaptured(sino::thread::Entry entry, StackSample& sample,
                          ThreadCreateFlags flags = ThreadCreateFlags::Joinable) {
  sample.esp = 0;
  const ThreadCreateSpec spec{sizeof(spec), 1, (u32)entry, (u32)&sample,
                              (u32)thread_probe_capture, (u32)flags};
  return api(SYS_THREAD_CREATE, (u32)&spec, sizeof(spec), 0);
}

bool matches(const char* mode);
int run(const char* mode);
}
#endif
