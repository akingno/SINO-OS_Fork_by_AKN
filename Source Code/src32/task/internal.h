#ifndef SINO_TASK_INTERNAL_H
#define SINO_TASK_INTERNAL_H
#include "task.h"
#include "../mm/paging.h"
#include "kernel_stack.h"
#include "schedule.h"
#include "collections.h"
#include "wait.h"

// 仅 task 实现使用；其他子系统通过 task.h 的窄接口访问。
namespace task {
inline constexpr int MaxProcesses = 8;
inline constexpr int MaxThreads = 4;
static_assert(MaxThreads < 8, "TID ABI reserves three low bits for slot + 1");
inline constexpr u32 UserStackBytes = 16384u;

struct UserFrame {
  Frame frame;
  u32 useresp, ss;
};
static_assert(sizeof(UserFrame) == 60, "user_enter frame size");
static_assert(__builtin_offsetof(UserFrame, useresp) == 52, "iret user ESP offset");
static_assert(__builtin_offsetof(Frame, cs) == 44, "interrupt_common CS offset");

enum class ThreadState : int { UNUSED, READY, RUNNING, SLEEPING, BLOCKED, EXITED };
struct Process;

struct Thread {
  ThreadState state;
  u32 wake;
  u32 waitObject;
  u32 waitGeneration;
  bool waitNotified;
  WaitReason waitOp;
  u32 waitDeadline;
  int waitTimed;
  TerminalRequest waitTerminal;
  UserFrame saved;
  QueueLink<Thread> schedLink;
  TimerNode timer;
  Process* process;
  u32 index;
  u32 tid, joinObject;
  int exitCode;
  Thread* joiner;
  Thread* joinTarget;

  QueueLink<Thread>& queueLink() { return schedLink; }
  WaitKey waitKey() const { return {waitObject, waitGeneration, waitOp}; }
  void initialize(Process* owner, u32 slot, u32 entry, u32 stack, u32 token = 0, u32 record = 0);
  void releaseJoinClaim();
  void dropJoinRecord();
  void block(u32 object, WaitReason op, u32 timeout);
  void reblock();
  void sleep(u32 delay);
  void cancel(bool pass = true);
  void finishWait(int result, u32 now);
  int waitExpired(u32 now) const;
};
static_assert(__is_trivially_copyable(Thread), "thread state needs no runtime construction");

struct AnonRegion { u32 base, bytes; };
struct Process {
  u32 pid, state, caps;
  int code;
  u32* pd;
  u32 abi, code_end;
  KernelStack kernelStack;
  Thread thread[MaxThreads];
  Handle handles[HANDLES];
  char name[32];
  u32 policy, workspace_frontend, input_frontend;
  u32 completion_object;
  u32 reason, fault_vector;
  CliInfo* interactive;
  u32 directory;
  u32 boot_directory;
  u32 desktop_owner;
  AnonRegion anon[ANON_MAX_REGIONS];
  QueueLink<Process> reapLink;
  TimerNode timer;
  QueueLink<Process>& queueLink() { return reapLink; }
  void retire(u32 why, int result, u32 vector = 0);
};
static_assert(__is_trivially_copyable(Process), "released process slots can be zeroed");

extern Process processes[MaxProcesses];
extern Process* current;
extern int currentThread;
void reapAndExpireRuns();
void initScheduler();
void waitSelfTest();
void enqueueReady(Thread& thread);
Thread* takeReady();
u32 readyCount();
void armTimer(TimerNode& timer, u32 deadline, TimerKind kind, void* target);
void disarmTimer(TimerNode& timer);
void expireTimers(u32 now);
bool queuedWork(u32 now);
Process* takeReap();
void detachProcess(Process& process);
void setEntryStack(const Process& process);
void leave(Frame* frame);
int retryWait(Thread* thread);
int dispatchSyscall(Frame* frame);
int joinResult(Thread& caller, Frame& frame);
extern "C" void user_enter(UserFrame*);

// 仅供薄 C ABI 桥接与 task 内部调用。
void init();
int launchConsole();
int launch(const char * a0, u32 a1);
int launchArgs(const char * a0, u32 a1, const char * a2);
int launchStrict(const char * a0, const char * a1);
int workspaceFrontend(u32 a0);
int desktopRun(u32 a0, u32 a1, const DesktopRun* a2);
void completionRelease(int a0);
void streamRelease(int a0);
void pipeWake(const Object* a0);
void terminalWake(u32 a0);
void interactiveWake(u32 a0);
int runStart(Handle * a0, const RunSpec * a1);
int runStartDirectory(Handle * a0, const RunSpec * a1, u32 a2, u32 a3);
int runStartBound(Handle * a0, const RunSpec * a1, u32 a2, u32 a3, const u32 * a4);
int runResult(Handle * a0, u32 a1, RunResult * a2);
int runRead(Handle * a0, u32 a1, u32 a2, u32 a3, void * a4, u32 a5);
int cancelOwned(Handle * a0, u32 a1);
Handle* terminalTarget(Handle* a0, u32 a1, u32* a2);
Handle* interactiveHandles(u32 a0);
int terminalCancel(u32 a0);
int interactiveResult(Handle* a0, u32 a1, ProcessCompletion* a2);
int interactiveStart(Handle* a0, const InteractiveSpec* a1, int a2, void* a3, u32* a4);
void runLimits(RunLimits * a0);
int list(ProcessInfo * a0, u32 a1);
int alive(u32 a0);
int kill(u32 a0, i32 a1);
int interrupt(Frame * a0);
int anonPage(u32* a0, u32 a1, int a2);
u32 userLimit(const u32* a0, u32 a1);
void tests();
}
#endif
