#ifndef SINO_TASK_SCHEDULE_H
#define SINO_TASK_SCHEDULE_H
#include "../types.h"

namespace task {
enum class StepResult : u32 { NoDispatch, Ran };
// 仅描述本轮进度；不得由此推断能否休眠。
StepResult step();
// 调用者关闭 IRQ；只读、有界，不切地址空间、不分配或重试系统调用。
bool hasWork(u32 now);
void beforeIdle();
void continued();
struct ScheduleStats {
  u32 passes, dispatches, noDispatch, candidates, retriesBlocked, cleanups;
  u32 continued, idleHalts, idleWithWork, maxCandidates;
};
static_assert(sizeof(ScheduleStats) == 40, "host scheduler inspection layout");
}
// 宿主验收观察用的内核符号，不是用户 ABI。
extern "C" task::ScheduleStats task_schedule_stats;
#endif
