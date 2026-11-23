#include "test_fault.h"
#ifdef SINO_SHELL_BRIDGE
#include "../klog.h"
#include "../panic.h"
#include "page.h"

namespace mmtest {
namespace {
struct Fault {
  const u32* owner{};
  Stage stage{Stage::None};
  u32 allowed{};
  bool repeat{};
  Stage next{Stage::None};

  bool take(Stage request) {
    if (stage != request) {
      return false;
    }
    if (allowed) {
      --allowed;
      return false;
    }
    pr_debug("MM PROBE injected stage=");
    pr_debug_hex(static_cast<u32>(stage));
    pr_debug("\n");
    if (!repeat) {
      stage = next;
      next = Stage::None;
    }
    return true;
  }
};
// 当前同时至多 8 个进程；记录不持有资源，在页目录回收前撤销。
Fault faults[8]{};
const u32* pressureOwner;
}

void arm(const u32* owner, Stage stage, u32 allowed, bool repeat, Stage next) {
  if (!owner || stage == Stage::None) {
    panic("Invalid memory probe owner", 0, 0);
  }
  clear(owner);
  for (auto& fault : faults) {
    if (!fault.owner) {
      fault = Fault{owner, stage, allowed, repeat, next};
      return;
    }
  }
  panic("Memory probe slots exhausted", 0, 0);
}

bool fail(const u32* owner, Stage stage) {
  for (auto& fault : faults) {
    if (fault.owner == owner && fault.take(stage)) {
      if (stage == Stage::ReclaimData && !pressureOwner) {
        if (!overrideWatermarks(free_pages + 1, free_pages + 1, owner)) {
          panic("Invalid reclaim probe watermarks", free_pages, 0);
        }
        pressureOwner = owner;
      }
      return true;
    }
  }
  return false;
}

void clear(const u32* owner) {
  if (pressureOwner == owner) {
    restoreWatermarks(owner);
    pressureOwner = nullptr;
  }
  for (auto& fault : faults) {
    if (fault.owner == owner) {
      fault = Fault{};
    }
  }
}
}
#endif
