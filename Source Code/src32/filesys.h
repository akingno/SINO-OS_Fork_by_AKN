#ifndef SINO_FILESYS_H
#define SINO_FILESYS_H
#include "object.h"
#ifdef __cplusplus
extern "C" {
#endif
/* Policy is checked before dispatch. The caller's address space stays active. */
int filesys_call(Handle*, u32, u32, u32, u32, u32, u32* user_pd);
int filesys_acquire_directory(Handle*, u32, u32);
int filesys_is_boot_directory(Object*);
int filesys_boot_reader(Handle*);
void filesys_release(int);
int filesys_workspace(Handle*);
#ifdef __cplusplus
}
#endif
#endif
