# SINO 开发对话入口

适用范围：本仓库。先确定任务所属区域，读下表列出的入口与对应测试；接口或证据不足时再沿调用链扩大范围。无需先扫描全仓。

## 仓库结构

| 目录 | 说明 |
| --- | --- |
| `Source Code/` | 唯一开发源码目录 |
| `Source Code/src32/` | 内核（C + x86 汇编） |
| `Source Code/src32/mm/` | 独立内存管理子系统（page.c、paging.c、uaccess.c） |
| `Source Code/user/` | Ring 3 CLI 程序（gsh、gcmd、tview、writer 等） |
| `Source Code/tools/` | 构建、发布与共享开发工具 |
| `Source Code/tests/` | 稳定套件、Ring 3 探针、共享工具；TESTING.md 为 AI 选测入口 |
| `Source Code/tests/development/` | 当前功能的临时测试；显式登记和收尾，不自动回归 |
| `Source Code/docs/` | 技术契约文档 |
| `Source Code/tutorials/` | 学习用 HTML 教程，开发时不修改；已 gitignore |
| `Source Code/disk32/` | 启动盘内容与字体资源 |
| `Documentation/` | 项目文档、许可与截图 |
| `Build Output/` | 构建产物（不提交） |
| `abandoned/` | 已归档的旧代码（如旧 Ring 0 终端） |
| `akn_learning/` | 本地个人教材，干净 clone 中没有它 |

解压发行包（`SINO-*-SysBASIC-*-Chinese/`）是参考副本，不是开发源码。

## 当前测试约定

日常验收范围：启动、严格进程、文件与持久化、流与会话、Shell。GUI 及其测试、截图辅助与旧转发入口已删除；文字控制台专项允许仅针对其 PS/2 输入发送 QMP 按键并读取 VGA 文字内存，不再提供 GUI 验收。键盘扫描码、终端和编辑器的宿主逻辑测试保留。

每个开发步骤只自动运行与改动直接相关的命令和接口测试，合计最多 50 项（含选跑的旧用例）。每个专项的自动验收可执行最多 20 个相关用例。运行前确认用例数，优先通过摘要脚本查看结果，只在失败时查看对应日志片段。

**AI 选测必须先读 [模块 → 测试命令指南](Source Code/tests/TESTING.md)，只匹配本次模块及追加条件。** 不逐个查阅所有测试，不因修改 process.c 等共享文件就全跑关联模块。完整目录仅在指南没有对应项、需要明确追加或用户要求列举时查阅。

- 运行前说明模块、命令、选择原因和用例数；统一入口用同一选择加 `-List` 预览。重复/重叠选择自动去重；漏跑、重跑或额外 ID 不能通过。
- 通过后停止。仅因新的修改、失败或明确的未验证影响扩大或重跑；50/20 是上限，不是目标。测试器检查仅在测试框架变化时运行，纯文档/目录说明修改不启动系统回归。
- `-Full` 仅供用户显式全量验收，AI 不以此绕过本步限制。压力和全量 core 不自动追加。同一轮共用构建与摘要；源码/测试身份变化后遵守重建检查，不强行复用旧镜像。

历史阶段测试和 GUI 入口已移除，不保留转发。稳定业务用例在 tests/host/suites/，不互相调用；固定 ID 在 tests/support/regression-cases.json，组合仅放在 regression-suites.js / regression-catalog.js。统一入口保留 24 个功能组 / 270 个固定 ID（core 23 组 / 261 项，stress 1 组 / 9 项）。新增或修改入口时同步更新模块指南；全部数量和其他专项见 [当前测试目录](Source Code/docs/test-catalog.md)。

常用测试选择（仓库根目录，PowerShell）：

```powershell
.\regression.ps1 -Suite persistence   # 独立持久化检查，4 项
.\regression.ps1 -Suite core -Full    # 非 GUI 基础回归，仅显式手动全量运行
.\regression.ps1 -BuildOnly           # 仅准备 ShellBridge 镜像及符号，不执行测试
```

开发中临时测试放在 [tests/development/](Source Code/tests/development/README.md)，按模块/功能命名，在 [INDEX.md](Source Code/tests/development/INDEX.md) 登记命令、数量及启用/收尾条件。只在当前功能需要时运行，不默认加入稳定回归；功能完成必须决定提升稳定覆盖、合并重复、迁入压力专项或删除临时验证，并清理活动登记。

## 常用命令（Windows PowerShell，仓库根目录）

| 意图 | 命令 | 说明 |
| --- | --- | --- |
| 确认基线 | `git status -sb`、`git log -1 --oneline` | 编辑前先看；保留已有改动 |
| 运行系统 | `.\启动开发版.cmd` | 重新构建并打开可见 QEMU；先关闭旧开发窗口 |
| 只编译 | `powershell -NoProfile -ExecutionPolicy Bypass -File .\dev.ps1 -BuildOnly` | 生成 ISO，不启动 VM |
| 无界面启动检查 | `node 'Source Code/tests/host/test-boot.js' 'Build Output/dev/runs/<本轮构建编号>'` | 使用 BuildOnly 目录，核对 ISO 和启动日志 |
| 编译并验证 Ring 3 | `powershell -NoProfile -ExecutionPolicy Bypass -File .\dev.ps1 -VerifyOnly` | 转入 process 无界面专项，8 项，使用私有盘 |
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

`node 'Source Code/verify-system.js'` 仅保留 1 个宿主键盘逻辑检查，不需要 ISO；系统功能验收使用 regression.ps1。注册表变更运行 tests/host/test-regression-plan.js（12 项），报告选择变更运行 tests/host/test-report-summary.js --group selections（4 项）；路径相对 Source Code/。

发行启动文件在 `Source Code/release/`：将 `SINO.iso`、`Start SINO.cmd`、`launch.ps1` 放在同一目录，从 ISO 启动，个人文件写入 `Data/data.img`。旧安装向导、装包、预装硬盘发布流程及 GUI 启动器测试已删除。

Shell 宿主测试参数：`-ExpectedExitCode 1` 断言预期失败，`-TimeoutSeconds 5` 设置超时。报告在 `Build Output/dev/runs/shell-.../`。通道仅在 `-ShellBridge` 构建中启用。

## 系统地图（路径相对 Source Code/）

32 位 x86 BIOS，单体内核。GUI 移除已完成计划四步：桌面、窗口管理、图形绘制、设置与内置 Ring 0 应用已移除；窗口相关系统调用保留编号并返回错误。GUI Ring 3 终端前端和文件管理器，以及 UIASSET.BIN、PINYIN.TXT、CJK_OFL.TXT 已删除且不再构建或打包。普通镜像通过 VGA 80×25 文字控制台自动进入 Ring 3 gsh；ShellBridge 镜像继续提供独立测试通道，gcmd、tview、writer 保留；brokerqa 是无界面 Shell / 会话探针，继续构建并打包到 ShellBridge 镜像。GUI 测试、注册项和旧转发入口已删除。普通镜像自动启动 conhost.snx 和 gsh.snx，日志依次确认 `SINO CLI ready` 与 `CONSOLE SHELL ready`；US 键盘直接输入，无中文输入法或拼音字典。文字屏幕仅显示 ASCII，UTF-8 文件数据保留。主循环依次推进 RTC、ShellBridge（启用时）、网络、用户线程；语言状态由 locale.c 持有。SYS_CONSOLE=0x150 仅授予内核启动的 conhost；普通或严格启动的相同程序也不能取得设备权限。

启动镜像仍固定 2.88 MB，删除资源减少文件占用，不改变镜像总容量。

| 任务 | 起读文件 / 符号 | 对应验证入口 |
| --- | --- | --- |
| 开机文字 Shell | user/console_host.c、src32/console.c、process.c:SYS_CONSOLE、loader.asm | `node 'Source Code/tests/host/test-console.js' <普通 BuildOnly 目录>`；6 项；追加 `--persistence` 为 3 项跨重启自动保存检查；契约 docs/text-console.md |
| 构建、引导 | build32.ps1、build-image32.js、src32/loader.asm、src32/kernel.c:kmain | `tests/host/test-boot.js <BuildOnly 构建目录>` |
| FAT12 启动卷 | src32/fs/boot_fat12.c:fs_init / fs_read / fs_read_at、src32/boot-ramdisk.h | 普通镜像无窗口启动；ShellBridge 验证 `/boot` 读取 |
| 物理页分配、启动页表 | src32/mm/page.c:page_init / page_alloc_run、src32/mm/paging.c:paging_bootstrap / paging_kernel_init | 普通镜像无窗口启动，核对启动日志 |
| 用户页表、权限和线程栈 | src32/mm/paging.c、src32/mm/uaccess.c:uaccess_prepare、src32/process.c:launch_policy / alloc_user_stack | ShellBridge 中 `run -s /boot/poltest.snx paging-a/b` |
| 多用户页表与动态地址窗口 | src32/mm/paging.c:paging_user_map / paging_user_find_gap | ShellBridge 中 `run -s /boot/poltest.snx paging-c` 系列 |
| 匿名内存与 SDK 堆 | src32/process.c:anon_map / anon_unmap、user/runtime.c | ShellBridge 中 `run -s /boot/fstest.snx anon-*` 系列 |
| 用户缓冲区校验 | src32/mm/uaccess.c、src32/process.c | ShellBridge 中 fstest.snx / termtest.snx 的 uaccess 系列 |
| 匿名页按需分配 | src32/process.c:anon_map / process_anon_page / process_interrupt | ShellBridge 中 anon-lazy / anon-oom / anon-fault |
| 高页、SNX v2 与匿名内存 | src32/mm/page.c、src32/mm/paging.c、user/linker.ld | `tests/host/test-memory.js images <ShellBridge 构建目录>` 或 `anonymous <同一构建目录>`，不依赖历史镜像 |
| 调度、故障回收 | src32/process.c:process_step / release、src32/entry.asm | `regression.ps1 -Suite process`；8 项 |
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
| 交互启动代理 | src32/interactive.h、process.c、terminal.c；tests/guest/interactive_tests.c | `regression.ps1 -Suite interactive`；契约 docs/interactive-runs.md |
| Shell | user/gsh.c、gsh_parse.h、gcmd.c | `regression.ps1 -Suite shell-waits`；契约 docs/user-shell.md |
| 只读文件工具 | user/tview.c、file_text.h | tests/host/test-file-text.c 宿主模型；契约 docs/file-tools.md |
| 常用命令与欢迎文本 | user/gsh.c、gcmd.c、shell_commands.h、shell_banner.h | shell-commands（112 项，仅显式全量）；契约 docs/shell-tools.md |
| 写工作区 | user/write_tool.c、gsh.c；process.c 可信来源 | 文件机制按 files 选测；契约 docs/workspace-shell.md；原 GUI 集成覆盖已移除 |
| 行编辑与文本模型 | user/gsh.c、line_edit.h、terminal_edit_model.h、terminal_model.h | tests/host/test-line-edit.c / test-line-model.c / test-terminal-model.c；契约 docs/shell-editing.md |
| 保存与恢复 | src32/vfs.c → src32/storage.c → src32/disk.c | files / persistence |
| 键盘解码 | src32/keyboard.c | tests/host/test-keyboard.js（宿主逻辑） |
| 时钟与 RTC | src32/rtc.c、src32/process.c:SYS_TIME | `regression.ps1 -Suite rtc-driver` |
| 网络 | src32/net.h、src32/net.c:net_poll、user/netlib.c | `regression.ps1 -Suite network`（仅回环） |
| 日志与语言资源 | src32/klog.c/.h、src32/locale.c/.h、resources/locale/ | tests/host/test-src32-refactor.js（43 项） |
| QEMU 测试基础设施 | tools/shell-test.js、tools/qmp.js、tests/support/regression-vm.js | 按改动选择；verify-system.js 仅宿主键盘检查 |

## 关键边界

- 用户栈 16 KiB，SNX v1 映像上限 192 KiB，v2 上限 16 MiB，基址 `0x40000000`
- VFS 全局 128 节点，单文件 64 KiB（含磁盘 SNX）
- 8 进程，每进程 4 线程，24 句柄
- 底层 Write 更新内存、Sync 提交用户目录快照；可写 gsh 的写入/创建/改名/删除命令成功后自动提交，再返回提示符，无需手动 sync；`/boot` 只读
- 启动镜像 2.88 MB，布局入口 `src32/boot-layout.h`
- Feature 掩码 32767（15 个已实现机制），详见 docs/os-interface.md
- 网络仅回环；宿主桥接和服务器通道是规划
- 严格策略（caps=0）默认拒绝，按白名单授权；兼容策略保留旧应用访问
- 管道读写、完成记录查询、终端字节读取和交互命令结果支持显式线程级阻塞，超时为 0 表示无限等待，有限等待最多 60000 tick
- 通用子进程委托、cwd/env 继承尚未实现

## 提交与推送

仅在用户明确要求时 push，并核对目标：origin 是 AKN fork，upstream 是原项目。结束时报告实际验证结果；修改入口、限制或测试命令时同步更新此页。
