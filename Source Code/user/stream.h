#ifndef SINO_STREAM_H
#define SINO_STREAM_H
/* Bounded waits suspend on S_AGAIN. A zero timeout makes one attempt. */
#include "sdk.h"
#ifdef __cplusplus
extern "C" {
#endif
static inline u32 StreamTicks(void) { return (u32)api(SYS_TICKS, 0, 0, 0); }
static inline int StreamTimeoutExpired(u32 now, u32 begin, u32 timeout) {
  return (u32)(now - begin) >= timeout;
}
static inline int StreamTransferFor(u32 call, int handle, const void* buffer,
                                    u32 bytes, u32 begin, u32 timeout) {
  if (bytes & STREAM_BLOCK_FLAG) return S_INVALID;
  int result = api(call, (u32)handle, (u32)buffer, bytes);
  if (result != S_AGAIN) return result;
  u32 elapsed = StreamTicks() - begin;
  if (elapsed >= timeout) return S_TIMEOUT;
  return call == SYS_STREAM_READ ? StreamReadBlocking(handle, (void*)buffer, bytes, timeout - elapsed)
                                 : StreamWriteBlocking(handle, buffer, bytes, timeout - elapsed);
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
  u32 elapsed = StreamTicks() - begin;
  if (elapsed >= timeout) return S_TIMEOUT;
  return call == SYS_RUN_RESULT ? WaitRun(handle, (RunResult*)out, timeout - elapsed)
                                : WaitProcess(handle, (ProcessCompletion*)out, timeout - elapsed);
}
static inline int RunWaitFor(int handle, RunResult* out, u32 timeout) {
  return ResultWaitFor(SYS_RUN_RESULT, handle, out, sizeof(*out), timeout);
}
static inline int ProcessWaitFor(int handle, ProcessCompletion* out, u32 timeout) {
  return ResultWaitFor(SYS_PROCESS_RESULT, handle, out, sizeof(*out), timeout);
}
#ifdef __cplusplus
}
#endif
#endif
