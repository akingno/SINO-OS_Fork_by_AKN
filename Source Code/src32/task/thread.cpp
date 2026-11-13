#include "internal.h"
#include "../lib/kstring.h"

namespace task {
  void Thread::initialize(u32 entry, u32 stack) {
    memset(this, 0, sizeof(*this));
    state = ThreadState::ACTIVE;
    wake = ticks;
    saved.frame.eip = entry;
    saved.frame.cs = 0x1b;
    saved.frame.eflags = 0x202;
    saved.useresp = stack;
    saved.ss = 0x23;
  }
  /* A zero timeout waits indefinitely. Finite waits use signed tick comparisons. */
  void Thread::block(u32 object, WaitReason op, u32 timeout) {
    if (timeout > 0x7fffffffu) {
      timeout = 0x7fffffffu;
    }
    waitObject = object;
    waitOp = op;
    waitDeadline = timeout ? ticks + timeout : 0;
    waitTimed = timeout != 0;
    state = ThreadState::BLOCKED;
  }
  void Thread::finishWait(int result, u32 now) {
    saved.frame.eax = (u32)result;
    waitObject = 0;
    waitOp = WaitReason::NONE;
    waitDeadline = 0;
    waitTimed = 0;
    wake = now;
    state = ThreadState::ACTIVE;
  }
  int Thread::waitExpired(u32 now) const {
    return waitTimed && (i32)(now - waitDeadline) >= 0;
  }
}
