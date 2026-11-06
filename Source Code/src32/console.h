#ifndef SINO_CONSOLE_H
#define SINO_CONSOLE_H
#include "kernel.h"
#define CON_COLS 80
#define CON_ROWS 26
void con_init(void);
void con_clear(void);
void con_color(u8);
void con_put(char);
void con_write(const char *);
void con_flush(void);
#endif
