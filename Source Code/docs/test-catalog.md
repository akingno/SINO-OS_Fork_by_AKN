# 当前测试目录

日常按 [模块选测指南](../tests/TESTING.md) 选择命令；本文是完整目录，无需每次通读。临时开发项独立登记在 [development/INDEX.md](../tests/development/INDEX.md)，不计入下面稳定回归。

统一入口登记 **24 个功能组、263 个固定用例 ID**：`core` 23 组/254 项，`stress` 1 组/9 项。此数不包括下面独立列出的启动、内存、测试器单元检查和原有系统专项。组合按固定 ID 去重，重复入口不产生新覆盖；内部断言、夹具、兼容转发和辅助文件不额外计数。

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
| `shell-commands`（手动全量） | 108 | 命令、语法和 Shell 文件操作 |
| `runner-rejects-failure` | 1 | 测试器拒绝客户机失败 |

`process` 和 `files` 包含各自的冷启动读取组；`persistence` 包含独立写入与读取组。这些数据依赖由注册表显式标明，业务套件不互相调用。

## 压力专项

`regression.ps1 -Suite stress`：9 项，包含 1 次显式资源基线准备和 8 次相同探针恢复循环。也可用 `-Suite process-stress`。不纳入普通 `core`。

## 普通启动与内存

`node 'Source Code/tests/host/test-task-queues.js'` 是 task 集合算法的 4 项独立宿主检查（`--list` 预览）：直接使用内核的侵入式 FIFO 和可撤销定时堆，验证 96 节点、所有权/重复登记、取消重插、容量、计时回绕及同期限次序。报告位于 `Build Output/dev/runs/task-queues-*/task-queues-report.json`，不自动加入 core，不改变统一注册表的 263 项。

| 入口（tests/host/） | 用例数 | 范围 |
| --- | ---: | --- |
| `test-console.js <普通 BuildOnly 目录> --persistence` | 3 | 不调用 sync；创建、更新/改名、删除后跨 QEMU 重启验证，保留其他文件 |
| `test-console.js <普通 BuildOnly 目录>` | 9 | 开机提示、编辑/历史、工作区、取消、ASCII 输入、退出重启、滚屏与输入固定；QMP 键盘和 VGA 文字内存 |
| `test-boot.js <普通 BuildOnly 目录>` | 1 | ISO 身份、El Torito、FAT12、计时器、普通启动隔离；ID 为 boot.ordinary |
| `test-event-waits.js <ShellBridge 目录>` | 8 | 严格策略能力发现、旧查询兼容、预置位/手动复位、广播/阻塞、重试保留原期限、超时/参数边界、关闭/复用、重复句柄权限、取消/故障回收；支持 --list，不自动加入 core |
| `test-thread-waits.js <ShellBridge 目录>` | 8 | SNX v2 参数/初始栈帧/正常返回、退出记录与栈回收、超时/输出解除映射、detach/TID 复用、单消费者/自身/外进程拒绝、参数/配额回滚、取消/故障、最后活动线程与未领取记录；支持 --list，不自动加入 core |
| `test-pids.js <ShellBridge 目录>` | 8 | PID 槽复用/旧 kill 拒绝、容量/畸形标识、历史完成记录取消隔离、映像失败回滚；Shell/文件/管道/Event/线程实际编号消耗；支持 --list，不自动加入 core |
| `test-identifiers.js` | 6 | 直接使用内核编号算法；缩小代数耗尽、预留撤销、陈旧/畸形 PID、完整正整数边界、cookie 全局槽均衡与退役、Object/TID 不回绕；支持 --list |
| `test-memory.js images <ShellBridge 目录>` | 7 | 4 GiB 和 2 GiB 单核/E820 与高页、定向映像/页表 OOM 回滚、SNX v2 启动与参数、线程栈复用 |
| `test-memory.js anonymous <ShellBridge 目录>` | 7 | 匿名映射、整窗稀疏预留、跨页 I/O、定向 OOM/首次触页故障及可写副本注入隔离 |
| `test-memory.js heap <ShellBridge 目录>` | 7 | SDK 分配与容量、四线程、独立大块、`realloc`、预留失败重试、hot/cold 缓存及内核实际丢弃后的重建 |
| `test-memory.js reclaim <ShellBridge 目录>` | 5 | 水位精确边界、预留准入、按评分和驻留页排序、回收后补页、失败语义、进程组继承与回收 |
| `test-memory.js purgeable <ShellBridge 目录>` | 5 | 可丢弃 ABI、锁计数、LOW 丢弃和重建、CRITICAL 回收顺序及最后解锁重启丢弃 |
| `test-memory.js pools <ShellBridge 目录>` | 4 | 4096 MiB 双池边界和独立转换、低端 Critical 时优先用高端、1024 MiB 纯低端兜底、全局准入与回收独立性 |
| `test-memory.js vma <ShellBridge 目录>` | 2 | 映像和工作线程栈的 VMA 登记失败回滚及资源复用 |
| `test-memory.js mprotect <ShellBridge 目录>` | 6 | 整区权限校验和恢复、未驻留与驻留页的写保护故障、跨页 I/O、缺页重试 |
| `test-memory.js e1 <ShellBridge 目录>` | 6 | 匿名权限拆分与合并、分配身份、旧基址要求、片段上限、VMA 节点分配失败回滚及可丢弃条件 |
| `test-memory.js e2 <ShellBridge 目录>` | 6 | 尾部与中间解除映射、配额、跨片段及非法范围、空洞复用、VMA 节点分配失败回滚、阻塞读唤醒时按当前映射校验 |

上述入口均支持 `--list`（内存入口为 `test-memory.js images --list`）。内存用例按实际探针/配置契约执行计数，详细断言列在报告 conditions 中。默认配置为 4096 MiB、单核，并用 QMP 核对。定向注入覆盖映像、页表和匿名页的分配失败与回滚。

`node 'Source Code/tests/host/test-vm-config.js' --list` 预览 4 项独立宿主检查：默认/专项参数、拒绝超出支持范围的参数、QMP 身份、开发/发行/测试入口一致性。报告为 Build Output/phase00a/vm-config-report.json，不计入统一注册表的 263 项。

四个 `test-*-waits.js <ShellBridge 目录>` 使用统一注册表：管道 10、完成记录 8、终端 11、Shell/SDK 5。它们与 `-Suite *-waits` 共用完全相同的用例，不额外计数；支持 `--list`。

## 测试器与宿主检查

| 入口（tests/host/） | 数量 | 范围 |
| --- | ---: | --- |
| `test-regression-plan.js` | 12 | 去重、固定 ID、数量上限、依赖顺序、漏跑/重跑和报告身份 |
| `test-report-summary.js --group contracts` | 18 | 报告契约与失败状态 |
| `test-report-summary.js --group selections` | 4 | 功能选择与范围校验 |
| `test-report-summary.js --group io` | 4 | 报告读取、损坏/缺失报告与失败详情 |
| `test-src32-refactor.js` | 41 | 存储、日志、语言资源和源码完整性，纳入 infrastructure |
| `test-keyboard.js` | 1 个宿主程序 | 编译 `keyboard.cpp` 检查扫描码解码 |

infrastructure 还编译 6 个宿主模型：`test-terminal-model.cpp`、`test-gsh.cpp`、`test-file-text.cpp`、`test-text-scan.cpp`、`test-line-edit.cpp`、`test-line-model.cpp`。`test-src32-storage.cpp`、`test-klog.cpp`、`test-locale.cpp` 由基础设施入口编译。`test-rtc.cpp` 由 rtc-driver 编译；其中的多次断言不是独立运行用例。

## 独立宿主入口

`node 'Source Code/verify-system.js'` 仅运行 `test-keyboard.js`（1 个宿主程序）。系统功能验收使用 `regression.ps1`。

`node 'Source Code/tests/host/test-user-heap.js'` 验证用户态堆核心（20 项，`--list` 预览）。常规构建覆盖大小类、对齐、槽位复用、容量、`realloc`、缓存预算、丢弃重建及回退。调试构建验证分配与释放涂抹。Windows 后端通过解除物理提交模拟 cold slab 丢弃，保留虚拟预留。报告位于 `Build Output/dev/runs/user-heap-*/user-heap-report.json`。宿主入口独立于 QEMU 专项和统一注册表。

## 客户机探针与辅助模块

客户机源码由宿主套件驱动，并非另一套重复运行的测试。当前 tests/guest/ 中有：

- `cli_tests.c`
- `completion_tests.c`
- `file_tests.c`
- `heap_probe.cpp`（仅随 ShellBridge 镜像打包为 `heaptest.snx`）
- `interactive_tests.c`
- `memory_v2_test.c`
- `nettest.c`
- `pipe_tests.c`
- `policy_tests.c`
- `probe.c`
- `terminal_tests.c`

测试夹具、通道、QMP、资源观察和工具转发都是基础设施，不作为新测试项。固定清单为 `tests/support/regression-cases.json` 和 `memory-cases.json`；标签调整时保留已有 ID，新增或删除覆盖时同步调整清单与注册表。

## 内存范围与用户复制宿主边界

`node 'Source Code/tests/host/test-mm-boundaries.js' --list` 预览；去掉 --list 执行 10 项固定 ID（mm.boundaries.*）。
覆盖地址对齐溢出、E820 内外取整/重叠/4 GiB、所属与活动页目录、权限、准备先于分配、部分落页 OOM、字符串和双向复制。
独立入口不计入统一注册表的 263 项。产物在 Build Output/dev/runs/mm-boundaries-*；契约见 [mm-access.md](mm-access.md)。
