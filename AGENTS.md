# SINO 开发对话入口

适用范围：本仓库。先确定任务所属区域，读下表列出的入口与对应测试；接口或证据不足时再沿调用链扩大范围。无需先扫描全仓。唯一开发源码目录是 `Source Code/`。解压发行包是参考副本；`Build Output/` 是产物；`akn_learning/` 是本地个人教材，干净 clone 中没有它。

## 常用命令（Windows PowerShell，仓库根目录）

| 意图 | 命令 | 结果 / 前提 |
| --- | --- | --- |
| 确认基线与用户改动 | `git status -sb`、`git log -1 --oneline` | 编辑前先看；保留已有改动 |
| 读 AI 速查 / 详细构建说明 | `Get-Content -Encoding UTF8 AGENTS.md`、`Get-Content -Encoding UTF8 Documentation/Development.md` | 本页是最短入口 |
| 运行系统 | `.\启动开发版.cmd` | 重新构建并打开可见 QEMU；先保存并关闭旧开发窗口 |
| 只编译 | `powershell -NoProfile -ExecutionPolicy Bypass -File .\dev.ps1 -BuildOnly` | 生成独立 ISO，可与已启动窗口共存 |
| 编译并验证 Ring 3 | `powershell -NoProfile -ExecutionPolicy Bypass -File .\dev.ps1 -VerifyOnly` | 无窗口启动、运行 probe、退出；使用持久开发盘，先关闭旧开发窗口 |
| 执行客户机 Shell 命令 | `powershell -NoProfile -ExecutionPolicy Bypass -File .\test.ps1 -Command 'echo hello'` | 编译带命令通道的专用 ISO，在私有盘、无窗口 VM 中执行，打印输出和报告路径 |
| 检查 Shell 命令通道 | `powershell -NoProfile -ExecutionPolicy Bypass -File .\test.ps1 -SelfTest` | 自动验证文本、文件、管道、状态及协议边界 |
| 检查 Shell 命令行为 | `powershell -NoProfile -ExecutionPolicy Bypass -File .\test.ps1 -SelfTest -Suite commands` | 112 项文档/帮助命令与语法检查，输出差异汇总到报告 |
| 找某接口 | `rg -n 'SYS_READ|SYS_WRITE' 'Source Code/src32/process.c' 'Source Code/user/sdk.h'` | 替换符号及目标文件 |
| 读测试前置条件 | `Get-Content 'Source Code/docs/testing.md'` | 查清普通镜像、自测镜像与工具要求 |
| 查看本地教材 | `Start-Process .\akn_learning\architecture\index.html` | 仅当本地存在，供人阅读 |

专项测试示例：先对当前源码运行 BuildOnly，用该次输出目录设置环境（不要猜最后一次构建是哪次）：

```powershell
$env:SINO_ISO = (Resolve-Path 'Build Output/dev/runs/<本轮编号>/SINO.iso').Path
$env:SINO_TOOLCHAIN = Split-Path (Get-Command gcc.exe).Source
$env:SINO_QEMU_DIR = 'C:\Program Files\qemu'
node 'Source Code/tools/test-iso.js'
```

完整回归入口：`node 'Source Code/verify-system.js'`；按 testing.md 准备所需构建与自测镜像。单次启动通过不等于 18 组全通过。专项 VM 测试使用独立测试盘；不要用日常开发盘做破坏性恢复实验。启动日志、构建与验证报告在对应的 runs 子目录。

## 系统地图（路径相对 Source Code/）

Shell 宿主测试会生成 `Build Output/dev/runs/shell-.../` 下的报告和每条命令输出文件。`-ExpectedExitCode 1` 可断言预期失败的命令，`-TimeoutSeconds 5` 设置单命令超时。宿主退出码 0 表示断言通过，1 表示失败，客户机实际状态另记在报告中。普通 Shell 输出和错误暂合流，内核诊断单独保存。`run *.snx` 返回启动结果，用户程序的最终退出状态仍待后续等待接口。

实现入口是根目录 `test.ps1`、`tools/shell-test.js` 和 `src32/shell.c:shell_bridge_poll`。通道仅在显式 `-ShellBridge` 构建中启用，普通开发桌面构建使用默认配置。详见 `Documentation/Shell-Testing.md`。

32 位 x86 BIOS，单体内核。桌面和旧工具在 Ring 0；SNX1 应用在 Ring 3，SDK 通过 INT 0x80 进入内核。主循环依次推进桌面、网络、用户线程；长同步内核操作会拖慢其他工作。

| 任务 | 起读文件 / 符号 | 对应验证入口（tools/） |
| --- | --- | --- |
| 构建、引导 | build32.ps1、build-image32.js、src32/loader.asm、src32/kernel.c:kmain | test-iso.js |
| 内存、调度、故障回收 | src32/kernel.c:memory_init、src32/process.c:process_step / release、src32/entry.asm | test-usermode.js |
| 改系统调用或权限 | src32/api.h → src32/process.c:syscall → src32/object.c；同步 user/sdk.h | test-usermode.js |
| 打开 / 保存 / 恢复 | user/editor.c:save → src32/process.c:SYS_WRITE / SYS_SYNC → src32/vfs.c → src32/storage.c:storage_sync → src32/disk.c | test-native-apps.js、test-recovery.js |
| 文本、窗口、绘制 | user/editor.c:key / draw、src32/gui.c:gui_step、src32/keyboard.c、src32/graphics.c | test-keyboard.js、test-typography.js |
| Shell、软件包与 IDE | src32/shell.c、user/setup.c、user/install_service.c、user/ide.c、user/basic.c | test-packages.js、test-ide.js |
| 通信 | src32/net.h、src32/net.c:net_poll、user/netlib.c | test-network.js（仅回环） |
| QEMU 测试基础设施 | tools/vm-test.js、tools/qmp.js、verify-system.js | 按实际改动选择专项 |

关键边界：用户栈 16 KiB，进程映像 192 KiB；VFS 全局 128 节点，单文件 64 KiB。Write 更新内存，Sync 提交用户目录快照；/boot 只读，更新 ISO 不会替换已安装应用。网络目前仅回环；宿主桥接、服务器通道和 C++ GUI 库都是规划。

仅在用户明确要求时 push，并核对目标：origin 是 AKN fork，upstream 是原项目。结束时报告实际验证结果；修改入口、限制或测试命令时同步更新此页。
