#ifndef SINO_TERMINAL_H
#define SINO_TERMINAL_H
#include "filesys.h"
void terminal_release(int, int);
int terminal_call(Handle*, u32, u32, const TerminalRequest*, FileAccess);
#endif
