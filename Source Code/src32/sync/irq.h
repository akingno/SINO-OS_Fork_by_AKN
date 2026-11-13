#ifndef SINO_SYNC_IRQ_H
#define SINO_SYNC_IRQ_H
#include "../types.h"

namespace sync {
using IrqState = u32;
inline constexpr IrqState InterruptEnabled = 1u << 9;
IrqState readState();
IrqState saveAndDisable();
void restore(IrqState state);

// 仅限短临界区：不分配、不阻塞、不做 I/O，不跨用户态切换持有。
class IrqGuard {
  IrqState saved_;
public:
  IrqGuard() : saved_(saveAndDisable()) {}
  ~IrqGuard() { restore(saved_); }
  IrqGuard(const IrqGuard&) = delete;
  IrqGuard& operator=(const IrqGuard&) = delete;
};
void selfTest();
}
#endif
