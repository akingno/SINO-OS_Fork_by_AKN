# 用户堆宿主验证

入口为 `user/heap.h`。开发用户堆的分配、释放或块回收逻辑时运行。共 20 项，验证大小类、块容量、对齐、复用、大块直通、`realloc`、hot/cold 缓存和失败回退。宿主映射后端通过解除物理提交模拟 cold 丢弃，保持虚拟地址预留。调试构建另覆盖填充行为。

从仓库根目录执行，使用仓库 MinGW 工具链。两种宿主构建分别验证常规路径和 `HEAP_DEBUG`。产物与摘要写入 `Build Output/dev/runs/user-heap-*/`。

```powershell
$env:SINO_TOOLCHAIN = Split-Path -Parent (Get-Command gcc.exe).Source
node 'Source Code/tests/development/user/heap/test-user-heap.js' --list
node 'Source Code/tests/development/user/heap/test-user-heap.js'
```

SDK 接入并完成客户机验收后，将长期宿主契约保留在稳定宿主套件中，沿用用例 ID，移除本目录的活动登记。
