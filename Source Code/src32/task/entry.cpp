#include "internal.h"

// 只在 ABI 边界保留旧链接符号，算法和私有状态均在 namespace task。
extern "C" {
  volatile u32 user_switch_out;
  u32 process_faults, process_switches, process_exits;
  void process_init() { task::init(); }
  int process_launch_console() { return task::launchConsole(); }
  int process_launch(const char * a0, u32 a1) { return task::launch(a0, a1); }
  int process_launch_args(const char * a0, u32 a1, const char * a2) { return task::launchArgs(a0, a1, a2); }
  int process_launch_strict(const char * a0, const char * a1) { return task::launchStrict(a0, a1); }
  int process_workspace_frontend(u32 a0) { return task::workspaceFrontend(a0); }
  int process_desktop_run(u32 a0, u32 a1, const DesktopRun* a2) { return task::desktopRun(a0, a1, a2); }
  void process_completion_release(int a0) { task::completionRelease(a0); }
  void process_stream_release(int a0) { task::streamRelease(a0); }
  void process_pipe_wake(const Object* a0) { task::pipeWake(a0); }
  void process_terminal_wake(u32 a0) { task::terminalWake(a0); }
  void process_interactive_wake(u32 a0) { task::interactiveWake(a0); }
  int process_run_start(Handle * a0, const RunSpec * a1) { return task::runStart(a0, a1); }
  int process_run_start_directory(Handle * a0, const RunSpec * a1, u32 a2, u32 a3) { return task::runStartDirectory(a0, a1, a2, a3); }
  int process_run_start_bound(Handle * a0, const RunSpec * a1, u32 a2, u32 a3, const u32 * a4) { return task::runStartBound(a0, a1, a2, a3, a4); }
  int process_run_result(Handle * a0, u32 a1, RunResult * a2) { return task::runResult(a0, a1, a2); }
  int process_run_read(Handle * a0, u32 a1, u32 a2, u32 a3, void * a4, u32 a5) { return task::runRead(a0, a1, a2, a3, a4, a5); }
  int process_cancel_owned(Handle * a0, u32 a1) { return task::cancelOwned(a0, a1); }
  Handle* process_terminal_target(Handle* a0, u32 a1, u32* a2) { return task::terminalTarget(a0, a1, a2); }
  Handle* process_interactive_handles(u32 a0) { return task::interactiveHandles(a0); }
  int process_terminal_cancel(u32 a0) { return task::terminalCancel(a0); }
  int process_interactive_result(Handle* a0, u32 a1, ProcessCompletion* a2) { return task::interactiveResult(a0, a1, a2); }
  int process_interactive_start(Handle* a0, const InteractiveSpec* a1, int a2, void* a3, u32* a4) { return task::interactiveStart(a0, a1, a2, a3, a4); }
  void process_run_limits(RunLimits * a0) { task::runLimits(a0); }
  int process_list(ProcessInfo * a0, u32 a1) { return task::list(a0, a1); }
  int process_alive(u32 a0) { return task::alive(a0); }
  int process_kill(u32 a0, i32 a1) { return task::kill(a0, a1); }
  void process_step() { task::step(); }
  int process_interrupt(Frame * a0) { return task::interrupt(a0); }
  int process_anon_page(u32* a0, u32 a1, int a2) { return task::anonPage(a0, a1, a2); }
  u32 process_user_limit(const u32* a0, u32 a1) { return task::userLimit(a0, a1); }
  void process_tests() { task::tests(); }
}
