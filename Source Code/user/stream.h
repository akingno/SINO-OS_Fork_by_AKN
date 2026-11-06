#ifndef SINO_STREAM_H
#define SINO_STREAM_H
/* Bounded waits prefer kernel blocking. A zero timeout makes one attempt. */
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
static inline int StreamTransferFor(u32 call, int handle, const void* buffer,
                                    u32 bytes, u32 begin, u32 timeout) {
  if (bytes & STREAM_BLOCK_FLAG) return S_INVALID;
  int result = api(call, (u32)handle, (u32)buffer, bytes);
  if (result != S_AGAIN) return result;
  if (StreamTimeoutExpired(StreamTicks(), begin, timeout)) return S_TIMEOUT;
  if (ThreadWaitAvailable()) {
    u32 elapsed = StreamTicks() - begin;
    if (elapsed >= timeout) return S_TIMEOUT;
    if (call == SYS_STREAM_READ) {
      result = StreamReadBlocking(handle, (void*)buffer, bytes, timeout - elapsed);
      if (result != S_BADHANDLE) return result;
      elapsed = StreamTicks() - begin;
      if (elapsed >= timeout) return S_TIMEOUT;
      return TerminalReadBlocking(handle, (void*)buffer, bytes, timeout - elapsed);
    }
    result = StreamWriteBlocking(handle, buffer, bytes, timeout - elapsed);
    if (result != S_BADHANDLE) return result;
    /* Terminal output still uses the ordinary bounded retry path. */
    result = StreamWrite(handle, buffer, bytes);
  }
  while (result == S_AGAIN) {
    if (StreamTimeoutExpired(StreamTicks(), begin, timeout)) return S_TIMEOUT;
    Yield(1);
    result = api(call, (u32)handle, (u32)buffer, bytes);
  }
  return result;
}
static inline int StreamReadFor(int handle, void* out, u32 bytes, u32 timeout) {
  if (timeout > STREAM_WAIT_MAX) return S_INVALID;
  return StreamTransferFor(SYS_STREAM_READ, handle, out, bytes, StreamTicks(), timeout);
}
static inline int StreamWriteFor(int handle, const void* data, u32 bytes, u32 timeout) {
  if (timeout > STREAM_WAIT_MAX) return S_INVALID;
  return StreamTransferFor(SYS_STREAM_WRITE, handle, data, bytes, StreamTicks(), timeout);
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
    int n = StreamTransferFor(SYS_STREAM_WRITE, handle, (const u8*)data + *written,
                              bytes - *written, begin, timeout);
    if (n <= 0) return n ? n : S_IO;
    *written += (u32)n;
    if (*written == bytes) return 0;
    if (StreamTimeoutExpired(StreamTicks(), begin, timeout)) return S_TIMEOUT;
  }
  return 0;
}
static inline int ResultWaitFor(u32 call, int handle, void* out, u32 bytes, u32 timeout) {
  if (timeout > STREAM_WAIT_MAX) return S_INVALID;
  u32 begin = StreamTicks();
  int result = api(call, (u32)handle, (u32)out, bytes);
  if (result != S_AGAIN) return result;
  if (StreamTimeoutExpired(StreamTicks(), begin, timeout)) return S_TIMEOUT;
  if (ThreadWaitAvailable()) {
    u32 elapsed = StreamTicks() - begin;
    if (elapsed >= timeout) return S_TIMEOUT;
    return call == SYS_RUN_RESULT ? WaitRun(handle, out, timeout - elapsed)
                                  : WaitProcess(handle, out, timeout - elapsed);
  }
  while (result == S_AGAIN) {
    if (StreamTimeoutExpired(StreamTicks(), begin, timeout)) return S_TIMEOUT;
    Yield(1);
    result = api(call, (u32)handle, (u32)out, bytes);
  }
  return result;
}
static inline int RunWaitFor(int handle, RunResult* out, u32 timeout) {
  return ResultWaitFor(SYS_RUN_RESULT, handle, out, sizeof(*out), timeout);
}
static inline int ProcessWaitFor(int handle, ProcessCompletion* out, u32 timeout) {
  return ResultWaitFor(SYS_PROCESS_RESULT, handle, out, sizeof(*out), timeout);
}
#endif
