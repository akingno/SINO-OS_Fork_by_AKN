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
void wakeInput(u32 object, task::WaitReason reason);
void release(const Object& object);
}
#endif
