# 按模块选择测试

这是 AI 日常选测入口。先匹配本次修改的模块及符号，只读对应行；完整目录见 [test-catalog.md](../docs/test-catalog.md)，环境和操作说明见 [testing.md](../docs/testing.md)。不为查找命令逐个阅读测试实现。

## 选择与执行规则

1. 根据改动行为选择下表命令。`task/process.cpp` 等共享文件按修改的函数/系统调用判断，不能因为文件相同就运行所有子系统测试。
2. 运行前简述“修改模块、选择的套件及原因、预计用例数”。统一入口先加 `-List` 预览；数量以实际计划为准，表内数字只作参考。
3. 只运行直接相关项。每步累计最多 50 项、每专项最多 20 项；不把上限当目标，不以拆批方式规避累计上限。通过后停止，只有新的修改、失败或明确的未验证影响才重跑/扩大范围。
4. 只有“追加条件”成立才增加对应项。不能为求保险默认追加 core、测试器检查、压力或 GUI；`-Full` 只供用户显式要求全量时使用。
5. 同一轮组合共享一次构建；独立专项复用同次且身份仍匹配的构建。源码/测试身份改变后遵守现有重建检查，不修改记录来强行复用旧镜像。
6. 成功只读取摘要；失败只读取失败用例对应的日志片段。报告检查仅在修改报告或测试框架时运行。文档/目录说明修改只检查命令和链接，不自动启动系统回归。
7. 没有匹配项时，沿受影响接口补充小范围验证并说明原因；不要回退到全量扫描。新增功能先按 [临时测试规则](development/README.md) 建立专项。

## 模块 → 默认命令 → 追加条件

命令均从仓库根目录的 PowerShell 执行。组合套件用逗号，例如 `.\regression.ps1 -Suite 'files,persistence'`；运行前对同一选择加 `-List`。

| 模块 / 修改范围 | 默认命令与参考数量 | 仅在这些改动时追加 |
| --- | --- | --- |
| 开机文字控制台、ASCII 键盘和 Shell 前端 | 普通 `dev.ps1 -BuildOnly` 后运行 `node 'Source Code/tests/host/test-console.js' <构建目录>`；9 项，`--list` 预览 | 自动保存变化加同入口 `--persistence`（3 项，支持 `--list --persistence`）及 files / persistence；键盘滚屏或行编辑显示变化加 test-terminal-model.c / test-line-model.c 宿主模型；设备授权改动追加 policy；会话等待变化加 shell-waits；不恢复 GUI 套件 |
| 引导、镜像、FAT12 启动卷 | `.\dev.ps1 -BuildOnly`，再 `node 'Source Code/tests/host/test-boot.js' '<本次普通构建目录>'`；1 项 | 涉及用户态启动/隔离时追加相应进程或内存专项 |
| 匿名映射、懒分配、SDK 堆；`anon_map/unmap` | 下方内存命令的 `anonymous`；7 项 | 映像、高页或线程栈布局变化时追加 `images` |
| 物理页、页表、高页、SNX、用户栈；`page_* / paging_*` | 下方内存命令的 `images`；11 项 | 涉及匿名映射/OOM 时加 `anonymous`；启动页表/物理池初始化变化时加普通启动检查 |
| E820 对齐/重叠、地址溢出、uaccess 复制契约 | `node 'Source Code/tests/host/test-mm-boundaries.js'`；10 项，`--list` 预览 | 替代页表与分配边界的宿主检查；系统路径仍按下列 images / anonymous 及追加条件选测 |
| 用户缓冲区；`uaccess_*` | `anonymous`；7 项，含跨页文件/管道 I/O | 文件或终端系统调用校验路径变化时，加对应 `files` 或 `terminal-waits`；新的非法指针边界另加针对性探针 |
| 调度、进程/线程退出、故障回收；`process_step / release / entry.asm` | `.\regression.ps1 -Suite process`；8 项 | 完成通知/等待器变化加 `completion-waits`；管道/终端唤醒变化只加相应等待专项 |
| 系统调用策略、权限、句柄授权；`launch_policy / syscall / object` | `.\regression.ps1 -Suite policy`；2 项 | 对象生命周期变化加受影响对象的套件；线程/进程资源变化加 `process` |
| 完成记录、取消、观察者 | `.\regression.ps1 -Suite completion`；5 项 | 阻塞/超时/唤醒变化加 `completion-waits`；8 项 |
| 文件对象、目录授权、VFS 版本与发布 | `.\regression.ps1 -Suite files`；2 项，含自有冷启动 | 提交、Sync、磁盘快照或恢复变化加 `persistence`；4 项；故障注入路径另选针对性边界 |
| 存储快照、磁盘持久化与恢复 | `.\regression.ps1 -Suite persistence`；4 项 | 发布/目录/文件语义变化加 `files`；额外存储故障检查按具体路径选择，不自动跑 50 项 infrastructure |
| 管道 IPC、标准流；`pipe / transfer_pipe` | `.\regression.ps1 -Suite pipes`；1 项 | 阻塞、关闭、超时或等待缓冲区变化加 `pipe-waits`；10 项 |
| 终端 IPC / 字节会话后端 | `.\regression.ps1 -Suite terminal`；1 项 | 阻塞读取、控制者关闭或唤醒变化加 `terminal-waits`；11 项 |
| 公共等待机制；`retry_wait` | 按受影响对象选择 `completion-waits` / `pipe-waits` / `terminal-waits` | 确认影响所有对象时组合三项，共 29 项；涉及 Shell/SDK 等待再加 `shell-waits`，不得带入其他无关套件 |
| 有限 CLI / 标准输入输出 | `.\regression.ps1 -Suite cli`；1 项 | SCS2 帧、租约、取消、捕获或宿主协议变化加 `managed-cli`；12 项 |
| Shell / SDK 阻塞等待 | `.\regression.ps1 -Suite shell-waits`；5 项 | 影响具体管道或终端等待 API 时追加对应等待专项；纯命令语法另按其解析器/命令选择，不自动跑 112 项 |
| 交互启动代理 / 会话所有权 | `.\regression.ps1 -Suite interactive`；1 项 | Shell 结果交付变化加 `shell-waits`；控制者/终端生命周期变化加 `terminal-waits` |
| 回环网络 | `.\regression.ps1 -Suite network`；1 项 | 只追加实际影响的标准流/进程机制；不扩大为外部网络验收 |
| RTC 采样、解码、缓存 | `.\regression.ps1 -Suite rtc-driver`；1 项 | 仅追加实际受影响的时间接口 |
| 测试计划、ID、去重或数量限制 | `node 'Source Code/tests/host/test-regression-plan.js'`；12 项 | 报告选择变化加 `test-report-summary.js --group selections`；4 项；报告文件读取变化加 `io` 组，4 项；报告契约变化选 `contracts` 组，18 项 |
| 键盘扫描码解码 | `node 'Source Code/verify-system.js'`；1 个宿主程序 | 无需 ISO；不启动 VM |

`dev.ps1 -VerifyOnly` 转入同一个 `process` 套件（8 项），不再通过窗口键盘输入或截图验证；`verify-system.js` 仅保留宿主键盘检查。GUI 测试与选择名称已删除，旧名称在构建前拒绝。

共享调度器/内存等底层机制确实影响多个接口时，应按实际影响追加，不能为少跑而漏掉必要边界。表中没有给出小入口的解析器、语言资源或存储故障检查，应先找到对应宿主测试或建立小专项，不以 `-Full` 代替选测。

task 子系统提取按实际接口选测：process 8 项、policy 2 项；Thread 等待成员迁移追加 shell-waits 5 项，普通启动 1 项。IRQ 嵌套恢复自检并入原有启动/进程用例，不新增 ID；契约见 [task-subsystem.md](../docs/task-subsystem.md)。

内核入口栈大小/生命周期变化选 process 8 项及 images 11 项，普通启动追加 1 项。KernelStack 的 9 页布局、共享内核 PTE 权限、边界读写、重复分配/释放、保护记录耗尽后的失败回滚与重试由启动自检覆盖，并在现有 process / boot 用例中断言标记，不新增 ID。编译器逐函数栈用量见 build32/*-normal.su；需要连同中断帧和实际调用链评估，不能当作整条调用链上界。

调度/主循环空闲决策变化选 process 8 项、shell-waits 5 项、普通启动 1 项；后台待办查询涉及网络与 RTC 时各加对应 1 项。process 原有启动/抢占用例包含就绪/到期状态自检、实际 idle_halt 的 `sti; hlt; ret` 字节、`probe.snx /schedule` 主动让出/有限睡眠/竞争读重试，以及有界候选和空闲统计检查，不新增固定 ID。`task_schedule_stats` 仅供宿主观察，不是用户 ABI。

## VM 配置基线

开发、发行与普通/接口验收默认 4096 MiB RAM、单核；QMP 报告实际配置。64 MiB 运行兼容已撤销。仅定向内存边界专项允许显式 1024/2048 MiB，不恢复低内存验收。
`node 'Source Code/tests/host/test-vm-config.js' --list` 预览，去掉 --list 执行 4 项配置宿主检查；仅 VM 参数/身份验证变化时运行，不默认追加全部基础设施测试。
OOM 探针使用 ShellBridge 内限定只读节点与页目录的映像/页表/匿名页失败注入，退出或启动失败时清理；不耗尽整台 VM。

## 内存专项：只构建一次 ShellBridge 镜像

没有身份匹配的本次构建时先执行下面第一行；已有匹配构建就直接复用。`-BuildOnly` 不运行任何测试，不能与 `-Suite`、`-List` 或 `-Full` 同用。

```powershell
.\regression.ps1 -BuildOnly
$build = '<上一步输出的 ShellBridge 构建目录>'
node 'Source Code/tests/host/test-memory.js' anonymous --list
node 'Source Code/tests/host/test-memory.js' anonymous $build
```

高页/SNX 改用 `images`；只有表中两个范围均受影响时才运行两个模式，不默认连跑。普通启动检查必须使用 `dev.ps1 -BuildOnly` 的普通镜像，不能使用这个启用了 ShellBridge 的镜像。

## 临时测试与收尾

临时测试放在 `tests/development/<模块>/<功能>/`，先在 [INDEX.md](development/INDEX.md) 登记模块、命令、用例数和启用条件；AI 仅查看当前功能对应条目。只有当前功能开发或明确的缺陷调查才执行，不自动加入稳定注册表。阶段/功能完成时必须逐项决定：提升为稳定契约、合并重复、迁入压力专项或删除临时验证，并清理登记。具体规则见 [README.md](development/README.md)。
