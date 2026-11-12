#ifndef SINO_INTERACTIVE_H
#define SINO_INTERACTIVE_H
#include "sdk.h"
#include "../src32/interactive.h"
#ifdef __cplusplus
extern "C" {
#endif
static inline int InteractiveStart(int t, InteractiveSpec *s, int dir,
                                   int role) {
  return TerminalCall(IX_START, t, s, sizeof(*s), dir, role);
}
static inline int InteractiveSubmit(int t, InteractiveSpec *s) {
  return TerminalCall(IX_SUBMIT, t, s, sizeof(*s), 0, 0);
}
static inline int InteractiveRequest(int t, InteractiveSpec *s) {
  return TerminalCall(IX_REQUEST, t, s, sizeof(*s), 0, 0);
}
static inline int InteractiveResultRead(int t, u32 id, InteractiveResult *s) {
  return TerminalCall(IX_RESULT, t, s, sizeof(*s), id, 0);
}
static inline int InteractiveResultWait(int t, u32 id, InteractiveResult* result) {
  if (ThreadWaitAvailable())
    return TerminalCall(IX_RESULT, t, result, sizeof(*result), id, IX_RESULT_BLOCK);
  int status;
  while ((status = InteractiveResultRead(t, id, result)) == S_AGAIN)
    Yield(1);
  return status;
}
static inline int InteractiveFinish(int t, int task) {
  return TerminalCall(IX_FINISH, t, 0, 0, task, 0);
}
#ifdef __cplusplus
}
#endif
#endif
