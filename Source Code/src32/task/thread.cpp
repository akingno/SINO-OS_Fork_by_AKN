#include "internal.h"
#include "../lib/kstring.h"
#include "../panic.h"

namespace task {
void Thread::initialize(Process* owner, u32 slot, u32 entry, u32 stack) {
  if (schedLink.linked() || timer.slot || waitNotified) {
    panic("Reinitialize linked thread", slot, timer.slot);
  }
  memset(this, 0, sizeof(*this));
  process = owner;
  index = slot;
  state = ThreadState::READY;
  wake = ticks;
  saved.frame.eip = entry;
  saved.frame.cs = 0x1b;
  saved.frame.eflags = 0x202;
  saved.useresp = stack;
  saved.ss = 0x23;
  enqueueReady(*this);
}
int Thread::waitExpired(u32 now) const {
  return waitTimed && (i32)(now - waitDeadline) >= 0;
}
}
