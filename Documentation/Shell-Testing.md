# 宿主直接执行 SINO Shell

在仓库根目录使用 Windows PowerShell，工具要求与 Development.md 一致。

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File .\test.ps1 -Command 'echo hello'
powershell -NoProfile -ExecutionPolicy Bypass -File .\test.ps1 -Command 'echo one two | wc'
powershell -NoProfile -ExecutionPolicy Bypass -File .\test.ps1 -Command 'cat missing.txt' -ExpectedExitCode 1
powershell -NoProfile -ExecutionPolicy Bypass -File .\test.ps1 -SelfTest
powershell -NoProfile -ExecutionPolicy Bypass -File .\test.ps1 -SelfTest -Suite commands
```

每次调用会编译专用 ISO，启动无窗口 QEMU，在本轮新建的 16 MiB 私有磁盘上执行命令，然后关闭测试 VM。开发桌面可以继续使用自己的磁盘。构建和测试期间持有当前 checkout 的 build.lock，以免其他开发入口覆盖共享中间产物。

命令是单行 UTF-8，最大 255 字节。cwd 和 `$?` 在同一测试 VM 的连续请求中保留。命令走测试镜像中 src32/shell_bridge.c 的历史无窗口解释器，支持变量、管道、脚本及重定向语法。G6 桌面终端通过 Ring 3 Shell 运行，其命令和授权见 [桌面 Shell](../Source%20Code/docs/desktop-shell.md)。普通输出和错误使用现有合并语义。文件只保存在本轮私有盘中，需要跨请求验证时使用 SelfTest 或扩展同一测试会话。

## 执行结果

控制台打印每个请求的结果，以及 shell-report.json 的位置。单命令模式还打印命令输出。宿主进程退出 0 表示预期状态和输出断言通过，1 表示失败。客户机命令本身的状态记在 actual.status，预期值默认是 0。未知命令遵循现有 Shell 行为，返回 1。

本轮输出保存在 Build Output/dev/runs/shell-编号 内。

`-SelfTest` 默认运行命令通道边界测试，`-Suite commands` 运行 112 项命令行为用例。后者对已收到的响应逐项比较状态和输出，普通断言失败后继续汇总差异，连接或执行超时则结束会话。`history` 在宿主请求中保持空记录，交互终端键盘命令仍记录正常历史。

| 文件 | 内容 |
| --- | --- |
| SINO.iso、build.json、prepare.json | 本轮镜像、哈希、构建编号与输入指纹 |
| shell-job.json | 请求参数 |
| shell-report.json | 阶段、逐项期望和实际、耗时、异常与清理结果 |
| command-N.out | 精确的命令输出字节 |
| kernel.log | 内核诊断和用户程序 SYS_LOG，与命令结果分开 |
| qemu-stderr.log | QEMU 启动错误 |
| shell-data.img | 本轮测试数据盘 |

`-TimeoutSeconds` 控制每条命令的等待上限，默认 15 秒。超时会报告失败并关闭本轮 QEMU。当前同步 Shell 的 sleep、halt 和慢 I/O 仍可阻塞主循环，宿主超时终止 VM 负责收束测试。

`run /boot/probe.snx` 等命令返回的是启动请求结果。Ring 3 应用现有日志写入 kernel.log，收集其最终退出状态与标准输入输出需要后续进程等待和 CLI 接口。

## 连接与协议

构建脚本的显式 -ShellBridge 开关定义 SINO_SHELL_BRIDGE。启用后，内核主循环通过 COM1 轮询收发，每轮最多推进 128 个字节，发送端就绪前会返回主循环。命令执行在正常内核环境中进行。

宿主只在 127.0.0.1 的临时端口监听，QEMU 串口作为客户端接入。测试框架核对客户机构建编号、协议启用日志、ISO 哈希和私有磁盘挂载。传输是本机开发控制接口，具备当前内核 Shell 的能力，应仅在自己的测试 VM 中启用。

所有整数字段为 little-endian，版本编码在 magic 中。

| 方向 | 帧格式 |
| --- | --- |
| 请求 | SCS1，u32 请求编号，u32 命令字节数，命令原始字节 |
| 响应 | SCR1，u32 请求编号，i32 Shell 状态，u32 flags，u32 输出字节数，输出原始字节 |

一次只允许一个未完成请求。输出容量 8192 字节，flags bit 0 表示输出溢出。普通命令遇到溢出时，宿主将该次验证判为失败，自测中则有专门验证溢出标志的用例。输出始终按长度解析，文本中的 SCR1 或 PASS 不具备控制含义。非法长度、NUL 或换行返回 S_INVALID。客户机在两秒无接收进展后丢弃不完整请求。

命令超时后保留已有报告和内核日志。尚未收到完整响应时，客户端无法保证命令输出的完整性。需要实时流式输出、更大结果、并发会话或用户态程序 stdin/stdout 时，应另行扩展协议和内核资源管理。

## 本轮验证记录

2026-11-03 已在本机完成带通道构建及 25 项自测，包括 UTF-8、管道、重定向、状态、分段请求、非法帧和输出溢出。另以独立私有盘验证期望不匹配和命令超时的失败报告、非零退出与 VM 清理，以及预期客户机错误的成功断言。

默认关闭通道的普通/自测镜像也已重新构建。原有 run-selftest.js、test-iso.js 和 test-usermode.js 通过，覆盖 Shell 键盘、历史、补全、脚本、桌面、实际 ISO 启动及用户态隔离与回收。完整上游回归尚待阶段集成时执行。

## stat 与 help 维护

stat 分别显示内容后端、用户快照范围和数据卷可用性。RAM 表示当前工作集位于内存，Persistence 表示节点的快照归属，Data disk 表示数据卷是否就绪。具体文件版本是否已经持久化仍以写入后的成功 Sync 为准。/home/user 根目录会在启动时重建，后代节点才写入快照。只读的内置应用标识文件显示 RAM (read-only)，启动资源显示 FAT12。

help 已补齐公开命令、ls 选项、SNX 启动参数、脚本别名与会话终止说明。命令测试会从当前公开命令表检查 help 覆盖率，并验证 stat 的目录边界和后端分类。

2026-11-03 维修验证已通过 112 项命令断言、25 项通道断言及数据卷不可用专项。普通/自测构建、原有 Shell/桌面内核自测和损坏快照恢复测试通过。桌面回归首次在窗口创建时序断言失败，原样复跑全部通过。完整上游回归尚未执行。
