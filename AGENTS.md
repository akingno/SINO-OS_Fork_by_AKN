# SINO 开发对话入口

适用范围：本仓库。先确定任务所属区域，读下表列出的入口与对应测试；接口或证据不足时再沿调用链扩大范围。无需先扫描全仓。

## 仓库结构

| 目录 | 说明 |
| --- | --- |
| `Source Code/` | 唯一开发源码目录 |
| `Source Code/src32/` | 内核（C + x86 汇编） |
| `Source Code/src32/mm/` | 独立内存管理子系统（page.c、paging.c、uaccess.c） |
| `Source Code/user/` | Ring 3 用户态程序（gsh、files、calculator、clock 等） |
| `Source Code/tools/` | 构建、测试与开发工具 |
| `Source Code/docs/` | 技术契约文档 |
| `Source Code/disk32/` | 启动盘内容与字体资源 |
| `Documentation/` | 项目文档、许可与截图 |
| `Build Output/` | 构建产物（不提交） |
| `abandoned/` | 已归档的旧代码（如旧 Ring 0 终端） |
| `akn_learning/` | 本地个人教材，干净 clone 中没有它 |

解压发行包（`SINO-*-SysBASIC-*-Chinese/`）是参考副本，不是开发源码。

## 当前测试约定

日常验收范围：启动、严格进程、文件与持久化、流与会话、Shell。GUI 整体退出日常验收，包括桌面、应用、终端窗口与渲染、菜单、快捷键、QMP 键鼠和截图检查。默认不追加 GUI 手测，用户单独要求时再提供操作与预期结果。

每个开发步骤只自动运行与改动直接相关的命令和接口测试，合计最多 50 项（含选跑的旧用例）。每个 Phase 的自动专项验收可执行最多 20 个相关用例。运行前确认用例数，优先通过摘要脚本查看结果，只在失败时查看对应日志片段。

## 常用命令（Windows PowerShell，仓库根目录）

| 意图 | 命令 | 说明 |
| --- | --- | --- |
| 确认基线 | `git status -sb`、`git log -1 --oneline` | 编辑前先看；保留已有改动 |
| 运行系统 | `.\启动开发版.cmd` | 重新构建并打开可见 QEMU；先关闭旧开发窗口 |
| 只编译 | `powershell -NoProfile -ExecutionPolicy Bypass -File .\dev.ps1 -BuildOnly` | 生成 ISO，不启动 VM |
| 编译并验证 | `powershell -NoProfile -ExecutionPolicy Bypass -File .\dev.ps1 -VerifyOnly` | 无窗口启动、运行 probe、退出 |
| 执行客户机命令 | `powershell -NoProfile -ExecutionPolicy Bypass -File .\test.ps1 -Command 'echo hello'` | 在私有无窗口 VM 中执行 |
| Shell 通道自测 | `powershell -NoProfile -ExecutionPolicy Bypass -File .\test.ps1 -SelfTest` | 验证文本、文件、管道、状态及协议 |
| Shell 命令检查 | `powershell -NoProfile -ExecutionPolicy Bypass -File .\test.ps1 -SelfTest -Suite commands` | 112 项命令与语法检查 |
| 全量基础回归 | `powershell -NoProfile -ExecutionPolicy Bypass -File .\stage2-test.ps1` | 8 组门禁，私有盘冷启动 |
| 查看回归结果 | `.\stage2-summary.ps1 -RunDirectory 'Build Output/dev/runs/<编号>'` | 不重跑，正常通过不展开 |
| 运行独立 CLI | `.\run.ps1 -Program /boot/clitest.snx -Arguments echo` | 严格进程，独立 stdout/stderr |
| 找接口 | `rg -n 'SYS_READ' 'Source Code/src32/process.c' 'Source Code/user/sdk.h'` | 替换符号和文件 |

专项测试示例：先 BuildOnly，用该次输出目录设置环境（不要猜最后一次构建是哪次）：

```powershell
$env:SINO_ISO = (Resolve-Path 'Build Output/dev/runs/<编号>/SINO.iso').Path
$env:SINO_TOOLCHAIN = Split-Path (Get-Command gcc.exe).Source
$env:SINO_QEMU_DIR = 'C:\Program Files\qemu'
node 'Source Code/tools/test-iso.js'
```

完整回归（23 组）：`node 'Source Code/verify-system.js'`，按 `docs/testing.md` 准备镜像。

Shell 宿主测试参数：`-ExpectedExitCode 1` 断言预期失败，`-TimeoutSeconds 5` 设置超时。报告在 `Build Output/dev/runs/shell-.../`。通道仅在 `-ShellBridge` 构建中启用。

## 系统地图（路径相对 Source Code/）

32 位 x86 BIOS，单体内核。桌面和剩余旧工具在 Ring 0。文件管理器（F1）、计算器（F4）、时钟（F6）已迁到 Ring 3。SI-SHELL（F2 / Terminal.app 可写，F12 只读）完全在 Ring 3 运行。画板已隐藏。文本编辑器仍在 Ring 0。主循环依次推进 RTC、桌面、网络、用户线程。

| 任务 | 起读文件 / 符号 | 对应验证入口（tools/） |
| --- | --- | --- |
| 构建、引导 | build32.ps1、build-image32.js、src32/loader.asm、src32/kernel.c:kmain | test-iso.js |
| 物理页分配、启动页表 | src32/mm/page.c:page_init / page_alloc_run、src32/mm/paging.c:paging_bootstrap / paging_kernel_init | 普通镜像无窗口启动，核对启动日志 |
| 用户页表、权限和线程栈 | src32/mm/paging.c、src32/mm/uaccess.c:uaccess_prepare、src32/process.c:launch_policy / alloc_user_stack | ShellBridge 中 `run -s /boot/poltest.snx paging-a/b` |
| 多用户页表与动态地址窗口 | src32/mm/paging.c:paging_user_map / paging_user_find_gap | ShellBridge 中 `run -s /boot/poltest.snx paging-c` 系列 |
| 匿名内存与 SDK 堆 | src32/process.c:anon_map / anon_unmap、user/runtime.c | ShellBridge 中 `run -s /boot/fstest.snx anon-*` 系列 |
| 用户缓冲区校验 | src32/mm/uaccess.c、src32/process.c | ShellBridge 中 fstest.snx / termtest.snx 的 uaccess 系列 |
| 匿名页按需分配 | src32/process.c:anon_map / process_anon_page / process_interrupt | ShellBridge 中 anon-lazy / anon-oom / anon-fault |
| 高页、SNX v2 与 Phase D 回归 | src32/mm/page.c、src32/mm/paging.c、user/linker.ld | `test-stage3-mm.js g <ShellBridge 构建目录> <G 前构建目录>`，随后 `test-stage3-mm.js d <同一构建目录>` |
| 调度、故障回收 | src32/process.c:process_step / release、src32/entry.asm | 非 GUI Ring 3 探针；test-usermode.js 含 GUI |
| 系统调用与权限 | src32/api.h → src32/process.c:syscall → src32/object.c；同步 user/sdk.h | stage2-test.ps1；契约 docs/os-interface.md |
| 完成记录 | src32/process.c:release / SYS_SPAWN_TRACKED / SYS_PROCESS_RESULT | stage2-test.ps1；契约 docs/os-interface.md |
| 有限 CLI | src32/process.c:process_run_start、run_bridge.c、shell_bridge.c | stage2-test.ps1；run.ps1；契约 docs/bounded-cli.md |
| 目录授权与文件对象 | src32/filesys.c、vfs.c、process.c:RunSpecV2；user/file_tests.c | stage2-test.ps1；契约 docs/file-objects.md |
| 管道与标准流 | src32/pipe.c/.h、object.c、process.c:RunSpecV3；user/pipe_tests.c | stage2-test.ps1；契约 docs/stream-pipes.md |
| 管道阻塞读写 | src32/process.c:transfer_pipe / retry_pipe_wait、src32/pipe.c:pipe_release | `test-stage4-pipes.js <ShellBridge 构建目录>`（11 项）；契约 docs/stream-pipes.md |
| 文本会话后端 | src32/terminal.c/.h、process.c:SYS_TERMINAL；user/terminal_tests.c | stage2-test.ps1；契约 docs/terminal-sessions.md |
| 终端前端 | user/terminal_frontend.c、terminal_model.h | stage2-test.ps1；stage2-frontend.js |
| 交互启动代理 | src32/interactive.h、process.c、terminal.c；user/interactive_tests.c | stage2-test.ps1；契约 docs/interactive-runs.md |
| Shell | user/gsh.c、gsh_parse.h、gcmd.c；stage2-shell.js | stage2-test.ps1；契约 docs/user-shell.md |
| 只读文件工具 | user/tview.c、file_text.h | stage2-test.ps1；契约 docs/file-tools.md |
| 常用命令与欢迎界面 | user/gsh.c、gcmd.c、shell_commands.h、shell_banner.h | stage2-test.ps1；契约 docs/shell-tools.md |
| 写工作区 | user/write_tool.c、gsh.c；process.c 可信来源 | stage2-test.ps1；契约 docs/workspace-shell.md |
| 行编辑 | user/gsh.c、line_edit.h、terminal_edit_model.h | stage2-test.ps1；契约 docs/shell-editing.md |
| 桌面终端入口 | src32/gui.c Terminal 入口、user/gsh.c | test-terminal-replacement.js；契约 docs/desktop-shell.md |
| 打开 / 保存 / 恢复 | user/editor.c → src32/vfs.c → src32/storage.c → src32/disk.c | test-native-apps.js、test-recovery.js |
| 文本、窗口、绘制 | user/editor.c、src32/gui.c、src32/keyboard.c、src32/graphics.c | test-keyboard.js、test-typography.js |
| 计算器 | user/calculator.c、src32/api.h、src32/process.c | test-calculator.js |
| 时钟与 RTC | user/clock.c、src32/rtc.c、src32/process.c:SYS_TIME | test-clock.js |
| 文件管理器 | user/files.c、src32/gui.c、src32/process.c | test-files.js、test-desktop.js |
| 画板隐藏 | src32/gui.c、src32/vfs.c、user/files.c | test-paint-hidden.js |
| Shell、软件包与 IDE | user/gsh.c、user/setup.c、user/ide.c、user/basic.c | test-packages.js、test-ide.js |
| 网络 | src32/net.h、src32/net.c:net_poll、user/netlib.c | test-network.js（仅回环） |
| 日志与语言资源 | src32/klog.c/.h、src32/locale.c/.h、resources/locale/ | test-src32-refactor.js（43 项） |
| QEMU 测试基础设施 | tools/vm-test.js、tools/qmp.js、verify-system.js | 按改动选择 |

## 关键边界

- 用户栈 16 KiB，SNX v1 映像上限 192 KiB，v2 上限 16 MiB，基址 `0x40000000`
- VFS 全局 128 节点，单文件 64 KiB（含磁盘 SNX）
- 8 进程，每进程 4 线程，24 句柄
- Write 更新内存，Sync 提交用户目录快照；`/boot` 只读
- 启动镜像 2.88 MB，布局入口 `src32/boot-layout.h`
- Feature 掩码 16383（14 个已实现机制），详见 docs/os-interface.md
- 网络仅回环；宿主桥接和服务器通道是规划
- 严格策略（caps=0）默认拒绝，按白名单授权；兼容策略保留旧应用访问
- 管道支持显式线程级阻塞读写，超时为 0 表示无限等待，有限等待最多 60000 tick
- 通用子进程委托、cwd/env 继承、进程完成与终端输入的线程级阻塞等待尚未实现

## 提交与推送

仅在用户明确要求时 push，并核对目标：origin 是 AKN fork，upstream 是原项目。结束时报告实际验证结果；修改入口、限制或测试命令时同步更新此页。
