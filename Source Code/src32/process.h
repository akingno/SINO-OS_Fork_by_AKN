#ifndef SINO_PROCESS_H
#define SINO_PROCESS_H
#include "api.h"
#include "interactive.h"
#include "kernel.h"
#include "object.h"
extern volatile u32 user_switch_out;
extern u32 process_faults, process_switches, process_exits;
void process_init(void);
int process_launch_console(void);
int process_launch(const char *, u32);
int process_launch_args(const char *, u32, const char *);
/* Trusted kernel launch only. No user syscall can select compatibility mode. */
int process_launch_strict(const char *, const char *);
int process_workspace_frontend(u32);
int process_desktop_run(u32, u32, const DesktopRun*);
void process_completion_release(int);
void process_stream_release(int);
void process_pipe_wake(const Object*);
void process_terminal_wake(u32 object);
void process_interactive_wake(u32 object);
/* Trusted kernel entry points; the user syscall validates all pointers first. */
int process_run_start(Handle *, const RunSpec *);
int process_run_start_directory(Handle *, const RunSpec *, u32, u32);
int process_run_start_bound(Handle *, const RunSpec *, u32, u32, const u32 *);
int process_run_result(Handle *, u32, RunResult *);
int process_run_read(Handle *, u32, u32, u32, void *, u32);
int process_cancel_owned(Handle *, u32);
Handle* process_terminal_target(Handle*,u32,u32*);
Handle* process_interactive_handles(u32);
int process_terminal_cancel(u32);
int process_interactive_result(Handle*,u32,ProcessCompletion*);
int process_interactive_start(Handle*,const InteractiveSpec*,int,void*,u32*);
void process_run_limits(RunLimits *);
int process_list(ProcessInfo *, u32);
int process_alive(u32);
int process_kill(u32, i32);
void process_step(void);
int process_interrupt(Frame *);
/* Validate or populate a missing page in the current process's anonymous range. */
int process_anon_page(u32* pd, u32 page, int populate);
u32 process_user_limit(const u32* pd, u32 address);
void process_message(u32, UiEvent);
void process_tests(void);
#endif
