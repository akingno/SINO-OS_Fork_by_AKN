#ifndef SINO_PIPE_H
#define SINO_PIPE_H
#include "object.h"
#ifdef __cplusplus
extern "C" {
#endif
int pipe_create(Handle*, u32, PipePair*);
void pipe_release(Object*);
u32 pipe_peer(const Object*);
int pipe_acquire(Handle*, u32, int);
int pipe_transfer(Object*, int, void*, u32);
int pipe_ready(Object*);
void pipe_limits(PipeLimits*);
#ifdef __cplusplus
}
#endif
#endif
