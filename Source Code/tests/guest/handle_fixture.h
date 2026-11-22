#ifndef SINO_TEST_HANDLE_FIXTURE_H
#define SINO_TEST_HANDLE_FIXTURE_H

#include "sdk.h"

namespace handle_fixture {

inline int duplicateIntoClosedSlot(int source, u32 rights, u32 closed) {
  constexpr u32 SlotMask = 0xffu;
  int copies[24];
  unsigned count = 0;
  int result = S_LIMIT;
  // Keep other slots occupied until the allocator selects the closed slot.
  while (count < sizeof(copies) / sizeof(copies[0])) {
    const int handle = api(SYS_DUP, (u32)source, rights, 0);
    if (handle < 0 || ((u32)handle & SlotMask) == (closed & SlotMask)) {
      result = handle;
      break;
    }
    copies[count++] = handle;
  }
  while (count) {
    const int status = Close(copies[--count]);
    if (status != S_OK) { result = status; }
  }
  return result;
}

}

#endif
