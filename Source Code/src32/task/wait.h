#ifndef SINO_TASK_WAIT_H
#define SINO_TASK_WAIT_H
#include "../types.h"
namespace task {
enum class WaitReason : u32 { NONE = 0, PIPE_READ = 1, PIPE_WRITE, COMPLETION, TERM_READ, IX_RESULT, EVENT, Count };
struct WaitKey { u32 object, generation; WaitReason reason; };
// object 使用索引 + 1；代数来自稳定 Object，不增加端点引用。
void objectCreated(u32 object);
void objectClosed(u32 object);
void notify(u32 object, WaitReason reason, bool all = false);
void handoff(WaitKey key);
}
#endif
