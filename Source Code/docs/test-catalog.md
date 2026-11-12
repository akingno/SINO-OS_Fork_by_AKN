# 当前测试目录

日常按 [模块选测指南](../tests/TESTING.md) 选择命令；本文是完整目录，无需每次通读。临时开发项独立登记在 [development/INDEX.md](../tests/development/INDEX.md)，不计入下面稳定回归。

统一入口登记 **24 个功能组、267 个固定用例 ID**：`core` 23 组/258 项，`stress` 1 组/9 项。GUI 的 9 组/32 项及旧独立 GUI 入口已删除。此数不包括下面独立列出的启动、内存、测试器单元检查和原有系统专项。组合按固定 ID 去重，重复入口不产生新覆盖；内部断言、夹具、兼容转发和辅助文件不额外计数。

## 统一入口：命令与接口

仓库根目录 PowerShell：`regression.ps1 -Suite <选择>`。`-List` 不构建、不启动 VM，显示计划中的 ID 和数量。默认总计最多 50 项、单组最多 20 项；表中标记全量的选择需要用户显式 `-Full`。开发步骤自动验收仍遵守 50/20 上限。

| 选择 | 用例数 | 范围 |
| --- | ---: | --- |
| `process` | 8 | 进程、故障隔离、抢占、资源与冷启动 |
| `policy` | 2 | 严格策略、权限与降级拒绝 |
| `completion` | 5 | 完成记录与观察者生命周期 |
| `cli` | 1 | 有限 CLI 参数、输入输出与回收 |
| `managed-cli` | 12 | SCS2 管理协议、取消、配额和故障 |
| `files` | 2 | 文件对象、授权、版本提交与冷启动 |
| `persistence` | 4 | 独立文本/二进制持久化 |
| `pipes` | 1 | 管道基础与并发传输 |
| `terminal` | 1 | 文本会话后端 |
| `interactive` | 1 | 交互启动代理 |
| `network` | 1 | Ring 3 回环网络 |
| `pipe-waits` | 10 | 管道阻塞与回收 |
| `completion-waits` | 8 | 完成记录阻塞等待 |
| `terminal-waits` | 11 | 终端阻塞读取 |
| `shell-waits` | 5 | Shell/SDK 等待集成 |
| `infrastructure`（手动全量） | 47 | 存储、日志、语言资源和 6 个宿主模型 |
| `rtc-driver` | 1 | RTC 解码、采样和缓存 |
| `shell-transport`（手动全量） | 25 | Shell 通道和帧恢复 |
| `shell-commands`（手动全量） | 112 | 命令、语法和 Shell 文件操作 |
| `runner-rejects-failure` | 1 | 测试器拒绝客户机失败 |

`process` 和 `files` 包含各自的冷启动读取组；`persistence` 包含独立写入与读取组。这些数据依赖由注册表显式标明，业务套件不互相调用。

## 压力专项

`regression.ps1 -Suite stress`：9 项，包含 1 次显式资源基线准备和 8 次相同探针恢复循环。也可用 `-Suite process-stress`。不纳入普通 `core`。

## 普通启动与内存

| 入口（tests/host/） | 用例数 | 范围 |
| --- | ---: | --- |
| `test-console.js <普通 BuildOnly 目录> --persistence` | 3 | 不调用 sync；创建、更新/改名、删除后跨 QEMU 重启验证，保留其他文件 |
| `test-console.js <普通 BuildOnly 目录>` | 6 | 开机提示、编辑/历史、工作区、取消、ASCII 输入、退出重启；QMP 键盘和 VGA 文字内存，不读取 GUI 场景或截图 |
| `test-boot.js <普通 BuildOnly 目录>` | 1 | ISO 身份、El Torito、FAT12、计时器、普通启动隔离；ID 为 boot.ordinary |
| `test-memory.js images <ShellBridge 目录>` | 9 | 64 MiB / 1 GiB、SNX v1/v2、高页、线程栈与冷启动 |
| `test-memory.js anonymous <ShellBridge 目录>` | 5 | 匿名映射、配额、跨页 I/O、故障和 OOM |

上述入口均支持 `--list`（内存入口为 `test-memory.js images --list`）。内存用例以一次实际探针执行计数，原有 13/18 个命名条件均保留在报告的 conditions 中，未删除覆盖。两种内存容量的同名探针有不同上下文，不合并。

四个 `test-*-waits.js <ShellBridge 目录>` 现仅转入同一统一注册表：管道 10、完成记录 8、终端 11、Shell/SDK 5。它们与 `-Suite *-waits` 共用完全相同的用例，不额外计数；支持 `--list`。

## 测试器与宿主检查

| 入口（tests/host/） | 数量 | 范围 |
| --- | ---: | --- |
| `test-regression-plan.js` | 12 | 去重、固定 ID、数量上限、依赖顺序、漏跑/重跑和报告身份 |
| `test-report-summary.js --group contracts` | 18 | 报告契约与失败状态 |
| `test-report-summary.js --group selections` | 4 | 功能选择与范围校验 |
| `test-report-summary.js --group io` | 4 | 报告读取、损坏/缺失报告与失败详情 |
| `test-src32-refactor.js` | 43 | 存储 25、klog 3、locale 14、源码未变 1；是 infrastructure 的组成部分 |
| `test-keyboard.js` | 1 个宿主程序 | 实际 keyboard.c 的逻辑检查，不计为 GUI 自动操作 |

infrastructure 还编译 6 个宿主模型：`test-terminal-model.c`、`test-gsh.c`、`test-file-text.c`、`test-text-scan.c`、`test-line-edit.c`、`test-line-model.c`。`test-src32-storage.c`、`test-klog.c`、`test-locale.c` 由基础设施入口编译。`test-rtc.c` 由 rtc-driver 编译；其中的多次断言不是独立运行用例。

## 保留的独立宿主入口

`node 'Source Code/verify-system.js'` 仅运行 `test-keyboard.js`（1 个宿主程序），不读取 ISO、不启动 VM、不生成截图。原来的桌面、启动器和 GUI 自测入口及转发已删除。系统功能验收使用上面的统一入口。

## 客户机探针与辅助模块

客户机源码由宿主套件驱动，并非另一套重复运行的测试。当前 tests/guest/ 中有：

- `cli_tests.c`
- `completion_tests.c`
- `file_tests.c`
- `interactive_tests.c`
- `memory_v2_test.c`
- `nettest.c`
- `pipe_tests.c`
- `policy_tests.c`
- `probe.c`
- `read_tests.c`
- `terminal_tests.c`

测试夹具、通道、QMP、资源观察和工具转发都是基础设施，不作为新测试项。固定清单为 `tests/support/regression-cases.json` 和 `memory-cases.json`；标签调整时保留已有 ID，新增或删除覆盖时同步调整清单与注册表。

## 内存范围与用户复制宿主边界

`node 'Source Code/tests/host/test-mm-boundaries.js' --list` 预览；去掉 --list 执行 10 项固定 ID（mm.boundaries.*）。
覆盖地址对齐溢出、E820 内外取整/重叠/4 GiB、所属与活动页目录、权限、准备先于分配、部分落页 OOM、字符串和双向复制。
独立入口不计入统一注册表的 267 项。产物在 Build Output/dev/runs/mm-boundaries-*；契约见 [mm-access.md](mm-access.md)。
