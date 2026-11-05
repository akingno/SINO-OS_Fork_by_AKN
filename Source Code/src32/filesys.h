#ifndef SINO_FILESYS_H
#define SINO_FILESYS_H
#include "object.h"
/* User memory is checked by the process layer; policy is checked before dispatch. */
typedef int (*FileAccess)(u32, u32, int);
typedef int (*FileString)(u32, char*, u32);
int filesys_call(Handle*, u32, u32, u32, u32, u32, FileAccess, FileString);
int filesys_acquire_directory(Handle*, u32, u32);
void filesys_release(int);
#endif
