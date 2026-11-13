#ifndef SINO_WORK_H
#define SINO_WORK_H
#include "types.h"
// 空闲前的有界状态检查：IF=0，不在这里处理请求或等待硬件。
// UART 状态读取会清除错误位，查询必须将其登记为待处理错误。
// 未来到期的工作返回 false，由保留的周期 PIT 唤醒主循环。
namespace rtc { bool hasWork(u32 now); }
namespace net { bool hasWork(u32 now); }
namespace shellbridge { bool hasWork(u32 now); }
namespace runbridge { bool hasWork(u32 now); }
#endif
