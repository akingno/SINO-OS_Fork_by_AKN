# SINO 开发对话入口

适用范围：本仓库。先确定任务所属区域，读下表列出的入口与对应测试；接口或证据不足时再沿调用链扩大范围。无需先扫描全仓。

## 仓库结构

| 目录 | 说明 |
| --- | --- |
| `Source Code/` | 唯一开发源码目录 |
| `Source Code/src32/` | 内核（C + x86 汇编） |
| `Source Code/src32/mm/` | 独立内存管理子系统（page.c、paging.c、uaccess.c） |
| `Source Code/user/` | Ring 3 用户态程序（gsh、files、calculator、clock 等） |
| `Source Code/tools/` | 构建、发布与共享开发工具 |
| `Source Code/tests/` | 宿主套件、Ring 3 测试探针与测试辅助模块 |
| `Source Code/docs/` | 技术契约文档 |
| `Source Code/disk32/` | 启动盘内容与字体资源 |
| `Documentation/` | 项目文档、许可与截图 |
| `Build Output/` | 构建产物（不提交） |
| `abandoned/` | 已归档的旧代码（如旧 Ring 0 终端） |
| `akn_learning/` | 本地个人教材，干净 clone 中没有它 |

解压发行包（`SINO-*-SysBASIC-*-Chinese/`）是参考副本，不是开发源码。

## 当前测试约定

日常验收范围：启动、严格进程、文件与持久化、流与会话、Shell。GUI 整体退出日常验收，包括桌面、应用、终端窗口与渲染、菜单、快捷键、QMP 键鼠和截图检查。默认不追加 GUI 手测，用户单独要求时再提供操作与预期结果。

每个开发步骤只自动运行与改动直接相关的命令和接口测试，合计最多 50 项（含选跑的旧用例）。每个专项的自动验收可执行最多 20 个相关用例。运行前确认用例数，优先通过摘要脚本查看结果，只在失败时查看对应日志片段。

历史 Stage 2/3/4 测试入口已移除，不保留兼容转发。当前回归按功能命名。`regression.ps1 -Suite persistence` 独立运行 4 项无 GUI 写入/冷启动检查；`-Suite terminal-gui` 只在明确要求 GUI 验证时运行，并自行准备与核对 GUI 持久化数据。普通回归不调用 GUI 套件。

常用测试选择（仓库根目录，PowerShell）：

```powershell
.\regression.ps1 -Suite persistence   # 独立持久化检查，4 项
.\regression.ps1 -Suite core -Full    # 非 GUI 基础回归，仅显式手动全量运行
.\regression.ps1 -Suite terminal-gui  # GUI，仅在明确要求时运行
```

日常优先选择单个功能：`-Suite process`（8 项）、`policy`（2 项）、`completion`（5 项）、`files`（2 项）、`cli`、`pipes`、`terminal`、`interactive`、`network`（各 1 项）。可用 `-Suite 'policy,files'` 组合互不重叠的功能，只构建一次；未知名称在构建前拒绝；重复或重叠选择按固定用例 ID 自动去重。`process`、`files` 和 `persistence` 各自包含自己的写入与冷启动读取，不能依赖其他业务套件准备数据。GUI 可单选 `frontend-gui`、`shell-gui`、`file-tools-gui`、`shell-tools-gui`、`workspace-gui`、`line-edit-gui`、`files-gui`。

套件组合仅放在 `tests/support/regression-suites.js` 和 `regression-catalog.js`；业务用例放在 `tests/host/suites/` 或对应 GUI 文件，禁止互相调用。通道、内存观察、GUI 操作分别由 `guest-context.js`、`gui-context.js` 提供，业务套件自行建立资源基线和夹具。新增业务套件只注册自己的入口，不修改其他业务套件。

执行前用 `regression.ps1 -Suite 'process,files' -List` 查看去重后的用例 ID、组数和数量，不构建、不启动 VM。固定用例清单在 `tests/support/regression-cases.json`；实际报告按 ID 核对，漏跑、重复或额外用例不能通过。默认超过总计 50 项或单组 20 项时在构建前停止。`-Full` 仅供用户显式手动全量验收，开发步骤的自动验收仍遵守 50/20 上限。

`-Suite stress` 为 9 项进程恢复压力测试（1 次显式基线准备 + 8 次恢复循环），不属于 `core`。聚焦等待专项为 `pipe-waits`（10）、`completion-waits`（8）、`terminal-waits`（11）、`shell-waits`（5），不会重复调用基础整套探针。`core` 共 23 组、261 个用例；终端 GUI 共 32 个用例。内部断言和同一探针的多个条件单独记录，不计作额外用例。完整清单见 [当前测试目录](Source Code/docs/test-catalog.md)。

## 常用命令（Windows PowerShell，仓库根目录）

| 意图 | 命令 | 说明 |
| --- | --- | --- |
| 确认基线 | `git status -sb`、`git log -1 --oneline` | 编辑前先看；保留已有改动 |
| 运行系统 | `.\启动开发版.cmd` | 重新构建并打开可见 QEMU；先关闭旧开发窗口 |
| 只编译 | `powershell -NoProfile -ExecutionPolicy Bypass -File .\dev.ps1 -BuildOnly` | 生成 ISO，不启动 VM |
| 无界面启动检查 | `node 'Source Code/tests/host/test-boot.js' 'Build Output/dev/runs/<本轮构建编号>'` | 使用 BuildOnly 目录，核对 ISO 和启动日志 |
| 编译并验证 Ring 3 | `powershell -NoProfile -ExecutionPolicy Bypass -File .\dev.ps1 -VerifyOnly` | 无窗口启动，通过键盘运行 probe 并保存截图 |
| 执行客户机命令 | `powershell -NoProfile -ExecutionPolicy Bypass -File .\test.ps1 -Command 'echo hello'` | 在私有无窗口 VM 中执行 |
| Shell 通道自测 | `powershell -NoProfile -ExecutionPolicy Bypass -File .\test.ps1 -SelfTest -Full` | 验证文本、文件、管道、状态及协议 |
| Shell 命令检查 | `powershell -NoProfile -ExecutionPolicy Bypass -File .\test.ps1 -SelfTest -Suite commands -Full` | 112 项命令与语法检查 |
| 全量基础回归 | `powershell -NoProfile -ExecutionPolicy Bypass -File .\regression.ps1 -Suite core -Full` | 按功能组合的非 GUI 回归，仅按需全量运行 |
| 查看回归结果 | `.\test-summary.ps1 -RunDirectory 'Build Output/dev/runs/<编号>'` | 不重跑，正常通过不展开 |
| 运行独立 CLI | `.\run.ps1 -Program /boot/clitest.snx -Arguments echo` | 严格进程，独立 stdout/stderr |
| 找接口 | `rg -n 'SYS_READ' 'Source Code/src32/process.c' 'Source Code/user/sdk.h'` | 替换符号和文件 |

日常启动检查先执行 `-BuildOnly`，再把该次输出目录传给 `tests/host/test-boot.js`。其他接受 `SINO_ISO` 的套件可用同次构建的显式路径：

```powershell
$env:SINO_ISO = (Resolve-Path 'Build Output/dev/runs/<编号>/SINO.iso').Path
$env:SINO_TOOLCHAIN = Split-Path (Get-Command gcc.exe).Source
$env:SINO_QEMU_DIR = 'C:\Program Files\qemu'
```

含 GUI 的完整回归（15 组，仅按需运行）：`node 'Source Code/verify-system.js'`，按 `docs/testing.md` 准备镜像。

发行启动文件在 `Source Code/release/`：将 `SINO.iso`、`Start SINO.cmd`、`launch.ps1` 放在同一目录，从 ISO 启动，个人文件写入 `Data/data.img`。旧安装向导、装包与预装硬盘发布流程已删除。启动器 GUI 专项入口为 `Source Code/tests/host/test-launcher.ps1`（按需运行，可用 `SINO_ISO` 指定镜像）。

Shell 宿主测试参数：`-ExpectedExitCode 1` 断言预期失败，`-TimeoutSeconds 5` 设置超时。报告在 `Build Output/dev/runs/shell-.../`。通道仅在 `-ShellBridge` 构建中启用。

## 系统地图（路径相对 Source Code/）

32 位 x86 BIOS，单体内核。桌面和剩余旧工具在 Ring 0。文件管理器（F1）、计算器（F4）、时钟（F6）已迁到 Ring 3。SI-SHELL（F2 / Terminal.app 可写，F12 只读）完全在 Ring 3 运行。画板已隐藏。文本编辑器仍在 Ring 0。主循环依次推进 RTC、桌面、网络、用户线程。

| 任务 | 起读文件 / 符号 | 对应验证入口 |
| --- | --- | --- |
| 构建、引导 | build32.ps1、build-image32.js、src32/loader.asm、src32/kernel.c:kmain | `tests/host/test-boot.js <BuildOnly 构建目录>` |
| FAT12 启动卷 | src32/fs/boot_fat12.c:fs_init / fs_read / fs_read_at、src32/boot-ramdisk.h | 普通镜像无窗口启动；ShellBridge 验证 `/boot` 读取 |
| 物理页分配、启动页表 | src32/mm/page.c:page_init / page_alloc_run、src32/mm/paging.c:paging_bootstrap / paging_kernel_init | 普通镜像无窗口启动，核对启动日志 |
| 用户页表、权限和线程栈 | src32/mm/paging.c、src32/mm/uaccess.c:uaccess_prepare、src32/process.c:launch_policy / alloc_user_stack | ShellBridge 中 `run -s /boot/poltest.snx paging-a/b` |
| 多用户页表与动态地址窗口 | src32/mm/paging.c:paging_user_map / paging_user_find_gap | ShellBridge 中 `run -s /boot/poltest.snx paging-c` 系列 |
| 匿名内存与 SDK 堆 | src32/process.c:anon_map / anon_unmap、user/runtime.c | ShellBridge 中 `run -s /boot/fstest.snx anon-*` 系列 |
| 用户缓冲区校验 | src32/mm/uaccess.c、src32/process.c | ShellBridge 中 fstest.snx / termtest.snx 的 uaccess 系列 |
| 匿名页按需分配 | src32/process.c:anon_map / process_anon_page / process_interrupt | ShellBridge 中 anon-lazy / anon-oom / anon-fault |
| 高页、SNX v2 与匿名内存 | src32/mm/page.c、src32/mm/paging.c、user/linker.ld | `tests/host/test-memory.js images <ShellBridge 构建目录>` 或 `anonymous <同一构建目录>`，不依赖历史镜像 |
| 调度、故障回收 | src32/process.c:process_step / release、src32/entry.asm | `regression.ps1 -Suite process`；tests/host/test-usermode.js 含 GUI |
| 系统调用与权限 | src32/api.h → src32/process.c:syscall → src32/object.c；同步 user/sdk.h | `regression.ps1 -Suite policy`；契约 docs/os-interface.md |
| 完成记录 | src32/process.c:release / SYS_SPAWN_TRACKED / SYS_PROCESS_RESULT | `regression.ps1 -Suite completion`；契约 docs/os-interface.md |
| 进程完成阻塞等待 | src32/process.c:release / user_completion_result / retry_wait；tests/guest/completion_tests.c | `tests/host/test-completion-waits.js <ShellBridge 构建目录>`（8 项）；契约 docs/os-interface.md |
| 有限 CLI | src32/process.c:process_run_start、run_bridge.c、shell_bridge.c | `regression.ps1 -Suite cli` / `managed-cli`；run.ps1；契约 docs/bounded-cli.md |
| 目录授权与文件对象 | src32/filesys.c、vfs.c、process.c:RunSpecV2；tests/guest/file_tests.c | `regression.ps1 -Suite files`；契约 docs/file-objects.md |
| 管道与标准流 | src32/pipe.c/.h、object.c、process.c:RunSpecV3；tests/guest/pipe_tests.c | `regression.ps1 -Suite pipes`；契约 docs/stream-pipes.md |
| 管道阻塞读写 | src32/process.c:transfer_pipe / retry_wait、src32/pipe.c:pipe_release | `tests/host/test-pipe-waits.js <ShellBridge 构建目录>`（10 项）；契约 docs/stream-pipes.md |
| 文本会话后端 | src32/terminal.c/.h、process.c:SYS_TERMINAL；tests/guest/terminal_tests.c | `regression.ps1 -Suite terminal`；契约 docs/terminal-sessions.md |
| 终端阻塞读取 | src32/terminal.c:wake_readers、process.c:SYS_TERMINAL / retry_wait；tests/guest/terminal_tests.c | `tests/host/test-terminal-waits.js <ShellBridge 构建目录>`（11 项）；契约 docs/terminal-sessions.md |
| 阻塞等待 SDK 与 Shell | user/stream.h、user/sdk.h、user/interactive.h、user/gsh.c:external | `tests/host/test-shell-waits.js <ShellBridge 构建目录>`（5 项，目录须含同次构建的 kernel-normal.exe）；契约 docs/os-interface.md |
| 终端前端 | user/terminal_frontend.c、terminal_model.h | `regression.ps1 -Suite terminal-gui`（GUI 按需）；tests/host/terminal-gui.js |
| 交互启动代理 | src32/interactive.h、process.c、terminal.c；tests/guest/interactive_tests.c | `regression.ps1 -Suite interactive`；契约 docs/interactive-runs.md |
| Shell | user/gsh.c、gsh_parse.h、gcmd.c；tests/host/shell-gui.js | regression.ps1；契约 docs/user-shell.md |
| 只读文件工具 | user/tview.c、file_text.h | 宿主模型；`regression.ps1 -Suite terminal-gui`（GUI 按需）；契约 docs/file-tools.md |
| 常用命令与欢迎界面 | user/gsh.c、gcmd.c、shell_commands.h、shell_banner.h | `regression.ps1 -Suite terminal-gui`（GUI 按需）；契约 docs/shell-tools.md |
| 写工作区 | user/write_tool.c、gsh.c；process.c 可信来源 | `regression.ps1 -Suite terminal-gui`（GUI 按需）；契约 docs/workspace-shell.md |
| 行编辑 | user/gsh.c、line_edit.h、terminal_edit_model.h | 宿主模型；`regression.ps1 -Suite terminal-gui`（GUI 按需）；契约 docs/shell-editing.md |
| 桌面终端入口 | src32/gui.c Terminal 入口、user/gsh.c | tests/host/test-terminal-replacement.js；契约 docs/desktop-shell.md |
| 打开 / 保存 / 恢复 | src32/gui.c → src32/vfs.c → src32/storage.c → src32/disk.c | tests/host/test-desktop.js、tests/host/test-recovery.js（均为 GUI，按需运行） |
| 文本、窗口、绘制 | src32/gui.c、src32/keyboard.c、src32/graphics.c | tests/host/test-keyboard.js、tests/host/test-typography.js |
| 计算器 | user/calculator.c、src32/api.h、src32/process.c | tests/host/test-calculator.js |
| 时钟与 RTC | user/clock.c、src32/rtc.c、src32/process.c:SYS_TIME | tests/host/test-clock.js |
| 文件管理器 | user/files.c、src32/gui.c、src32/process.c | tests/host/test-files.js、tests/host/test-desktop.js |
| 画板隐藏 | src32/gui.c、src32/vfs.c、user/files.c | tests/host/test-paint-hidden.js |
| 网络 | src32/net.h、src32/net.c:net_poll、user/netlib.c | tests/host/test-network.js（仅回环） |
| 日志与语言资源 | src32/klog.c/.h、src32/locale.c/.h、resources/locale/ | tests/host/test-src32-refactor.js（43 项） |
| QEMU 测试基础设施 | tools/vm-test.js、tools/qmp.js、verify-system.js | 按改动选择 |

## 关键边界

- 用户栈 16 KiB，SNX v1 映像上限 192 KiB，v2 上限 16 MiB，基址 `0x40000000`
- VFS 全局 128 节点，单文件 64 KiB（含磁盘 SNX）
- 8 进程，每进程 4 线程，24 句柄
- Write 更新内存，Sync 提交用户目录快照；`/boot` 只读
- 启动镜像 2.88 MB，布局入口 `src32/boot-layout.h`
- Feature 掩码 32767（15 个已实现机制），详见 docs/os-interface.md
- 网络仅回环；宿主桥接和服务器通道是规划
- 严格策略（caps=0）默认拒绝，按白名单授权；兼容策略保留旧应用访问
- 管道读写、完成记录查询、终端字节读取和交互命令结果支持显式线程级阻塞，超时为 0 表示无限等待，有限等待最多 60000 tick
- 通用子进程委托、cwd/env 继承尚未实现

## 提交与推送

仅在用户明确要求时 push，并核对目标：origin 是 AKN fork，upstream 是原项目。结束时报告实际验证结果；修改入口、限制或测试命令时同步更新此页。
