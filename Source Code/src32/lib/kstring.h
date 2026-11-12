#ifndef SINO_KSTRING_H
#define SINO_KSTRING_H
#include "../types.h"
#ifdef __cplusplus
extern "C" {
#endif

void* memcpy(void*, const void*, u32);
void* memset(void*, int, u32);
u32 strlen(const char*);
int streq(const char*, const char*);
void number(u32, char*);

#ifdef __cplusplus
}
#endif
#endif
