#ifndef SINO_MM_TEST_FAULT_H
#define SINO_MM_TEST_FAULT_H
#ifdef SINO_SHELL_BRIDGE
#include "../types.h"

// 仅供只读启动卷上的指定探针使用；不提供用户态控制接口。
namespace mmtest {
enum class Stage : u32 { None = 0, Image = 1, Table = 2, Anonymous = 3 };
void arm(const u32* owner, Stage stage, u32 allowed, bool repeat = false);
bool fail(const u32* owner, Stage stage);
void clear(const u32* owner);
}
#endif
#endif
