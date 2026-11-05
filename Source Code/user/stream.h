#ifndef SINO_STREAM_H
#define SINO_STREAM_H
/* User-side bounded waiting. No sleeping kernel stack or retained user pointer.
   Readiness is a hint: retry the actual operation after S_AGAIN. */
#include "sdk.h"
static inline u32 StreamTicks(void) { return (u32)api(SYS_TICKS, 0, 0, 0); }
static inline int StreamTimeoutExpired(u32 now, u32 begin, u32 timeout) {
  return (u32)(now - begin) >= timeout;
}
static inline int StreamWaitFor(int handle, u32 wanted, u32 timeout) {
  if ((wanted != STREAM_READABLE && wanted != STREAM_WRITABLE) || timeout > STREAM_WAIT_MAX)
    return S_INVALID;
  u32 begin = StreamTicks();
  for (;;) {
    int state = StreamReady(handle);
    if (state < 0 || ((u32)state & wanted)) return state;
    if (state & STREAM_HANGUP) return S_CLOSED;
    if (state & STREAM_LIMIT) return S_LIMIT;
    if (StreamTimeoutExpired(StreamTicks(), begin, timeout)) return S_TIMEOUT;
    Yield(1);
  }
}
static inline int StreamReadFor(int handle, void* out, u32 bytes, u32 timeout) {
  if (timeout > STREAM_WAIT_MAX) return S_INVALID;
  u32 begin = StreamTicks();
  for (;;) {
    int n = StreamRead(handle, out, bytes);
    if (n != S_AGAIN) return n;
    if (StreamTimeoutExpired(StreamTicks(), begin, timeout)) return S_TIMEOUT;
    Yield(1);
  }
}
static inline int StreamWriteFor(int handle, const void* data, u32 bytes, u32 timeout) {
  if (timeout > STREAM_WAIT_MAX) return S_INVALID;
  u32 begin = StreamTicks();
  for (;;) {
    int n = StreamWrite(handle, data, bytes);
    if (n != S_AGAIN) return n;
    if (StreamTimeoutExpired(StreamTicks(), begin, timeout)) return S_TIMEOUT;
    Yield(1);
  }
}
/* One deadline covers the whole operation. The output always records accepted
   bytes, including on timeout/error; retry only the unaccepted suffix. */
static inline int StreamWriteAllFor(int handle, const void* data, u32 bytes, u32 timeout, u32* written) {
  if (!written) return S_INVALID;
  *written = 0;
  if (timeout > STREAM_WAIT_MAX || bytes > RUN_IO_MAX) return S_INVALID;
  u32 begin = StreamTicks();
  if (!bytes) return StreamWrite(handle, data, 0);
  while (*written < bytes) {
    int n = StreamWrite(handle, (const u8*)data + *written, bytes - *written);
    if (n > 0) *written += (u32)n;
    else if (n != S_AGAIN) return n ? n : S_IO;
    if (*written == bytes) return 0;
    if (StreamTimeoutExpired(StreamTicks(), begin, timeout)) return S_TIMEOUT;
    if (n == S_AGAIN) Yield(1);
  }
  return 0;
}
static inline int RunWaitFor(int handle, RunResult* out, u32 timeout) {
  if (timeout > STREAM_WAIT_MAX) return S_INVALID;
  u32 begin = StreamTicks();
  for (;;) {
    int n = GetRunResult(handle, out);
    if (n != S_AGAIN) return n;
    if (StreamTimeoutExpired(StreamTicks(), begin, timeout)) return S_TIMEOUT;
    Yield(1);
  }
}
static inline int ProcessWaitFor(int handle, ProcessCompletion* out, u32 timeout) {
  if (timeout > STREAM_WAIT_MAX) return S_INVALID;
  u32 begin = StreamTicks();
  for (;;) {
    int n = GetProcessResult(handle, out);
    if (n != S_AGAIN) return n;
    if (StreamTimeoutExpired(StreamTicks(), begin, timeout)) return S_TIMEOUT;
    Yield(1);
  }
}
#endif
