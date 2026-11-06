#ifndef SINO_TERMINAL_H
#define SINO_TERMINAL_H
#include "filesys.h"
#include "interactive.h"
int terminal_bind_interactive(void*,Handle*,u32,CliInfo*);
void terminal_release(int, int);
int terminal_call(Handle*, u32, u32, const TerminalRequest*, u32* user_pd);
#endif
