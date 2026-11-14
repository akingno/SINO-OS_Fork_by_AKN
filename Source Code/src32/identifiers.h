#ifndef SINO_IDENTIFIERS_H
#define SINO_IDENTIFIERS_H
#include "types.h"

// 单核、内核不可抢占；这些编号只由普通内核路径修改，不由 IRQ 分配。
namespace ids {
template<u32 Limit> struct Counter {
  u32 value;
  bool available() const { return value < Limit; }
  u32 issue() {
    if (!available()) { return 0; }
    return ++value;
  }
};

enum class State : u32 { Free, Reserved, Live, Retired };
template<u32 Count, u32 SlotBits, u32 Limit> struct ProcessPool {
  static_assert(Count < (1u << SlotBits), "slot zero is reserved");
  static_assert(Limit && Limit <= (0x7fffffffu >> SlotBits), "positive int tokens");
  struct Slot { Counter<Limit> generation; State state; } slots[Count];
  int reserve() {
    for (u32 i = 0; i < Count; i++) {
      if (slots[i].state != State::Free) { continue; }
      if (!slots[i].generation.available()) { slots[i].state = State::Retired; continue; }
      slots[i].state = State::Reserved;
      return (int)i;
    }
    return -1;
  }
  u32 commit(u32 slot) {
    if (slot >= Count || slots[slot].state != State::Reserved) { return 0; }
    u32 generation = slots[slot].generation.issue();
    if (!generation) { return 0; }
    slots[slot].state = State::Live;
    return (generation << SlotBits) | (slot + 1);
  }
  bool abort(u32 slot) {
    if (slot >= Count || slots[slot].state != State::Reserved) { return false; }
    slots[slot].state = State::Free;
    return true;
  }
  int lookup(u32 token) const {
    u32 slot = (token & ((1u << SlotBits) - 1)) - 1;
    if (!token || token > 0x7fffffffu || slot >= Count) { return -1; }
    const Slot& s = slots[slot];
    return s.state == State::Live && s.generation.value == (token >> SlotBits) ? (int)slot : -1;
  }
  bool release(u32 token) {
    int slot = lookup(token);
    if (slot < 0) { return false; }
    auto& s = slots[slot];
    s.state = s.generation.available() ? State::Free : State::Retired;
    return true;
  }
};

// cookie 按低字节槽号、跨所有句柄表分配。不能按进程重置，否则外来旧句柄可能别名。
template<u32 Count, u32 Limit> struct Cookies {
  static_assert(Count < 32 && Limit <= 0x7fffffu, "unchanged positive handle encoding");
  Counter<Limit> counters[Count];
  int choose(u32 freeMask) const {
    int best = -1;
    for (u32 i = 0; i < Count; i++) {
      if (!(freeMask & (1u << i)) || !counters[i].available()) { continue; }
      if (best < 0 || counters[i].value < counters[best].value) { best = (int)i; }
    }
    return best;
  }
  u32 issue(u32 slot) { return slot < Count ? counters[slot].issue() : 0; }
};

struct Stats {
  unsigned long long processes, abortedProcesses, objects, handles, threads;
  u32 processLimits, objectLimits, handleLimits, threadLimits;
};
static_assert(sizeof(Stats) == 56, "read-only host observation layout");
}
extern "C" { extern ids::Stats identifier_stats; }
#endif
