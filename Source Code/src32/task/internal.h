#ifndef SINO_TASK_INTERNAL_H
#define SINO_TASK_INTERNAL_H
#include "task.h"
#include "../mm/paging.h"
#include "kernel_stack.h"

// 仅 task 实现使用；其他子系统通过 task.h 的窄接口访问。
namespace task {
inline constexpr int MaxProcesses = 8;
inline constexpr int MaxThreads = 4;
inline constexpr u32 UserStackBytes = 16384u;

struct UserFrame {
  Frame frame;
  u32 useresp, ss;
};
static_assert(sizeof(UserFrame) == 60, "user_enter frame size");
static_assert(__builtin_offsetof(UserFrame, useresp) == 52, "iret user ESP offset");
static_assert(__builtin_offsetof(Frame, cs) == 44, "interrupt_common CS offset");

enum class ThreadState : int { UNUSED, ACTIVE, BLOCKED };
enum class WaitReason : u32 { NONE = 0, PIPE_READ = 1, PIPE_WRITE, COMPLETION, TERM_READ, IX_RESULT };
enum class QueueOwner : u32 { None = 0, Ready, ObjectWait };
struct Thread;
struct ScheduleLink {
  Thread* previous;
  Thread* next;
  QueueOwner owner;
  bool linked() const { return owner != QueueOwner::None; }
};

struct Thread {
  ThreadState state;
  u32 wake;
  u32 waitObject;
  WaitReason waitOp;
  u32 waitDeadline;
  int waitTimed;
  TerminalRequest waitTerminal;
  UserFrame saved;
  // PR-03 接入：就绪/对象等待互斥使用此节点，不动态分配。
  ScheduleLink schedLink;
  // 0 表示不在定时堆中；接入时保存 heap index + 1，与链节点独立。
  u32 timerSlot;

  void initialize(u32 entry, u32 stack);
  void block(u32 object, WaitReason op, u32 timeout);
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
};
static_assert(__is_trivially_copyable(Process), "released process slots can be zeroed");

extern Process processes[MaxProcesses];
extern Process* current;
extern int currentThread;
void reapAndExpireRuns();
void setEntryStack(const Process& process);
void leave(Frame* frame);
int retryWait(Thread* thread);
int dispatchSyscall(Frame* frame);
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
void step();
int interrupt(Frame * a0);
int anonPage(u32* a0, u32 a1, int a2);
u32 userLimit(const u32* a0, u32 a1);
void tests();
}
#endif
