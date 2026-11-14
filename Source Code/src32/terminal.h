#ifndef SINO_TERMINAL_H
#define SINO_TERMINAL_H
#include "filesys.h"
#include "interactive.h"
#ifdef __cplusplus
extern "C" {
#endif
int terminal_bind_interactive(void*,Handle*,u32,CliInfo*);
void terminal_release(int, int);
int terminal_call(Handle*, u32, u32, const TerminalRequest*, u32* user_pd);
#ifdef __cplusplus
}
#endif
namespace terminal { u32 readState(u32 object); }
#endif
