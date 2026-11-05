#ifndef SINO_PROCESS_H
#define SINO_PROCESS_H
#include "api.h"
#include "kernel.h"
#include "object.h"
extern volatile u32 user_switch_out;
extern u32 process_faults, process_switches, process_exits;
void process_init(void);
int process_launch(const char *, u32);
int process_launch_args(const char *, u32, const char *);
/* Trusted kernel launch only. No user syscall can select compatibility mode. */
int process_launch_strict(const char *, const char *);
void process_completion_release(int);
void process_stream_release(int);
/* Trusted kernel entry points; the user syscall validates all pointers first. */
int process_run_start(Handle *, const RunSpec *);
int process_run_start_directory(Handle *, const RunSpec *, u32, u32);
int process_run_start_bound(Handle *, const RunSpec *, u32, u32, const u32 *);
int process_run_result(Handle *, u32, RunResult *);
int process_run_read(Handle *, u32, u32, u32, void *, u32);
int process_cancel_owned(Handle *, u32);
Handle* process_terminal_target(Handle*,u32,u32*);
int process_terminal_cancel(u32);
void process_run_limits(RunLimits *);
int process_list(ProcessInfo *, u32);
int process_alive(u32);
int process_kill(u32, i32);
void process_step(void);
int process_interrupt(Frame *);
void process_message(u32, UiEvent);
void process_tests(void);
#endif
