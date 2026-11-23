#ifndef SINO_TERMINAL_H
#define SINO_TERMINAL_H
#include "filesys.h"
#include "interactive.h"
#include "task/wait.h"
#ifdef __cplusplus
extern "C" {
#endif
int terminal_bind_interactive(void*,Handle*,u32,CliInfo*);
int terminal_call(Handle*, u32, u32, const TerminalRequest*, u32* user_pd);
#ifdef __cplusplus
}
#endif
namespace terminal {
struct ControlIdentity { u32 index, generation; };
ControlIdentity controlIdentity(void* session);
void completionPublished(ControlIdentity control, u32 pid);
int consoleReady(const Object& control, const ConsoleWaitRequest& request,
                 u32 shellPid, u32 childPid, u32 completed);
bool armConsoleKeyboard(task::WaitKey key);
void disarmConsoleKeyboard(task::WaitKey key);
void keyboardQueued();
#ifdef SINO_SHELL_BRIDGE
void consoleSelfTest();
#endif
void wakeWaiter(u32 object, task::WaitReason reason);
void release(const Object& object);
}
#endif
