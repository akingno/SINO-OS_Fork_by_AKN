#ifndef SINO_CONSOLE_H
#define SINO_CONSOLE_H
#include "types.h"
#define CON_COLS 80
#define CON_ROWS 25
void con_init(void);
void con_clear(void);
void con_put(char);
void con_write(const char *);
void con_flush(void);
void console_present(const u16*, u32);
void console_panic(const char*, u32, u32);
#endif
