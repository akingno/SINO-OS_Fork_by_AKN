# 用户堆宿主验证

入口为 `user/heap.h`。开发用户堆的分配、释放或块回收逻辑时运行。专项验证大小类、块容量、分配对齐、复用、大块映射、失败边界和调试填充，共 8 项。

从仓库根目录执行，使用仓库 MinGW 工具链。两种宿主构建分别验证常规路径和 `HEAP_DEBUG`。产物与摘要写入 `Build Output/dev/runs/user-heap-*/`。

```powershell
$env:SINO_TOOLCHAIN = Split-Path -Parent (Get-Command gcc.exe).Source
node 'Source Code/tests/development/user/heap/test-user-heap.js' --list
node 'Source Code/tests/development/user/heap/test-user-heap.js'
```

SDK 接入并完成客户机验收后，将长期宿主契约保留在稳定宿主套件中，沿用用例 ID，移除本目录的活动登记。
