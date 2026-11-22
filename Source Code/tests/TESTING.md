# 按模块选择测试

这是 AI 日常选测入口。先匹配本次修改的模块及符号，只读对应行；完整目录见 [test-catalog.md](../docs/test-catalog.md)，环境和操作说明见 [testing.md](../docs/testing.md)。不为查找命令逐个阅读测试实现。

## 选择与执行规则

1. 根据改动行为选择下表命令。`task/process.cpp` 等共享文件按修改的函数/系统调用判断，不能因为文件相同就运行所有子系统测试。
2. 运行前简述“修改模块、选择的套件及原因、预计用例数”。统一入口先加 `-List` 预览；数量以实际计划为准，表内数字只作参考。
3. 只运行直接相关项。每步累计最多 50 项、每专项最多 20 项；不把上限当目标，不以拆批方式规避累计上限。通过后停止，只有新的修改、失败或明确的未验证影响才重跑/扩大范围。
4. 只有“追加条件”成立才增加对应项。不能为求保险默认追加 core、测试器检查或压力；`-Full` 只供用户显式要求全量时使用。
5. 同一轮组合共享一次构建；独立专项复用同次且身份仍匹配的构建。源码/测试身份改变后遵守现有重建检查，不修改记录来强行复用旧镜像。
6. 成功只读取摘要；失败只读取失败用例对应的日志片段。报告检查仅在修改报告或测试框架时运行。文档/目录说明修改只检查命令和链接，不自动启动系统回归。
7. 没有匹配项时，沿受影响接口补充小范围验证并说明原因；不要回退到全量扫描。新增功能先按 [临时测试规则](development/README.md) 建立专项。

## 模块 → 默认命令 → 追加条件

命令均从仓库根目录的 PowerShell 执行。组合套件用逗号，例如 `.\regression.ps1 -Suite 'files,persistence'`；运行前对同一选择加 `-List`。

| 模块 / 修改范围 | 默认命令与参考数量 | 仅在这些改动时追加 |
| --- | --- | --- |
| 开机文字控制台、ASCII 键盘和 Shell 前端 | 普通 `dev.ps1 -BuildOnly` 后运行 `node 'Source Code/tests/host/test-console.js' <构建目录>`；9 项，`--list` 预览 | 自动保存变化加同入口 `--persistence`（3 项，支持 `--list --persistence`）及 files / persistence；键盘滚屏或行编辑显示变化加 `test-terminal-model.cpp` / `test-line-model.cpp` 宿主模型；设备授权改动追加 policy；会话等待变化加 shell-waits |
| 引导、镜像、FAT12 启动卷 | `.\dev.ps1 -BuildOnly`，再 `node 'Source Code/tests/host/test-boot.js' '<本次普通构建目录>'`；1 项 | 涉及用户态启动/隔离时追加相应进程或内存专项 |
| 匿名映射与懒分配；`anon_map/unmap` | 下方内存命令的 `anonymous`；7 项 | 映像、高页或线程栈布局变化时追加 `images` |
| 用户态 slab 分配器；`user/heap.h` | `node 'Source Code/tests/host/test-user-heap.js'`；20 项，先 `--list` 预览 | 影响真实映射、回收或 SDK 可见行为时追加 `heap`，7 项 |
| SDK 堆入口及堆锁；`user/runtime.cpp / user/sdk.h` | `node 'Source Code/tests/host/test-memory.js' heap '<ShellBridge 构建目录>'`；7 项，先 `heap --list` 预览 | 运行时入口、初始化或共享同步机制变化加 process 8 项及普通启动 1 项；匿名边界夹具改动选 anonymous 7 项，宽映像堆探针改动选 images 11 项 |
| 匿名映射保护与片段身份；`protect_anon` | `mprotect` 6 项覆盖整区权限与缺页重试，`e1` 6 项覆盖范围改权、合并和分配身份 | 分配或释放变化加 `anonymous`，可丢弃条件或回收路径变化加 `purgeable` 或 `reclaim` |
| 匿名范围解除映射；`anon_unmap_range` | `e2` 6 项，覆盖配额、空洞复用、定向分配失败和阻塞读输出缓冲区重校验 | 影响匿名预留或释放时加 `anonymous`，修改 syscall 准入时加 `policy` |
| 匿名分配 ID、字节和片段配额；`ANON_MAX_REGIONS / ANON_MAX_BYTES / ANON_MAX_FRAGMENTS` | `anonymous` 7 项、`e1` 6 项、`e2` 6 项、`process` 8 项及普通启动 1 项，共 28 项 | 现有用例验证跨位图字槽复用、128 MiB 预留，以及 512 片段满额时的回滚和批量回收 |
| 物理页、页表、高页、SNX、用户栈；`page_* / paging_*` | 下方内存命令的 `images`；11 项，含 v2 clitest 参数与完成记录、v2 poltest 栈回收与复用 | 涉及匿名映射/OOM 时加 `anonymous`；启动页表/物理池初始化变化时加普通启动检查 |
| E820 对齐/重叠、地址溢出、uaccess 复制契约 | `node 'Source Code/tests/host/test-mm-boundaries.js'`；10 项，`--list` 预览 | 替代页表与分配边界的宿主检查；系统路径仍按下列 images / anonymous 及追加条件选测 |
| 用户缓冲区；`uaccess_*` | `anonymous`；7 项，含跨页文件/管道 I/O | 文件或终端系统调用校验路径变化时，加对应 `files` 或 `terminal-waits`；新的非法指针边界另加针对性探针 |
| SNX 头和加载兼容性；`native_header_valid / launch_policy / linker.ld` | `images` 11 项、`process,policy` 10 项及普通启动 1 项 | process 现有 Ring 3 用例先运行 v2 副本，再修改同一文件的版本字段，验证 v1 拒绝和资源对账 |
| 调度、进程/线程退出、故障回收；`process_step / release / entry.asm` | `.\regression.ps1 -Suite process`；8 项 | 完成通知/等待器变化加 `completion-waits`；管道/终端唤醒变化只加相应等待专项 |
| 系统调用策略、权限、句柄授权；`launch_policy / syscall / object` | `.\regression.ps1 -Suite policy`；2 项 | 对象生命周期变化加受影响对象的套件；线程/进程资源变化加 `process` |
| 内存回收优先级；`launch_policy / SYS_RECLAIM_PRIORITY / terminalForeground` | `.\regression.ps1 -Suite 'process,policy,shell-waits'`；15 项 | 进程启动与回收 8 项、评分 ABI/权限及 managed run 初始分 2 项、前台交接 5 项。另加普通启动 1 项，验证 conhost 与 gsh 的评分，共 16 项 |
| 物理页水位、回收待办和缺页重试；`page_watermark / reclaimLowest / retryAnonFault` | `node 'Source Code/tests/host/test-memory.js' reclaim '<ShellBridge 构建目录>'`；5 项，先用 `reclaim --list` 预览 | 水位初始化和匿名落页接入加 `images` 11 项、`anonymous` 7 项，进程回收及空闲决策变化加 `process` 8 项、`shell-waits` 5 项及普通启动 1 项 |
| 双池页计数、水位和低端兜底；`page_watermark_low / page_watermark_high / page_alloc` | `node 'Source Code/tests/host/test-memory.js' pools '<ShellBridge 构建目录>'`；4 项，先用 `pools --list` 预览 | 启动发布和落页策略变化加 `images` 11 项、`anonymous` 7 项及普通启动 1 项，回收和丢弃日志变化加 `reclaim`、`purgeable` 各 5 项。双池使用 4096 MiB，纯低端使用 1024 MiB |
| 进程组继承、查询与回收；`SYS_PROCESS_GROUP / runStartBound / interactiveStart / reclaimLowest` | `.\regression.ps1 -Suite 'process,cli,shell-waits'`；14 项，共用本次镜像执行 `test-memory.js reclaim` 5 项 | 进程生命周期 8 项、managed run 1 项、交互组继承与结果交付 5 项、回收专项 5 项、普通启动 1 项，共 20 项 |
| 可丢弃匿名内存；`SYS_ANON_PURGEABLE / SYS_ANON_LOCK / SYS_ANON_UNLOCK / purgeOneRegion` | `node 'Source Code/tests/host/test-memory.js' purgeable '<ShellBridge 构建目录>'`；5 项，先用 `purgeable --list` 预览 | 匿名落页、解除映射和失效重建加 `anonymous` 7 项；水位回收及空闲待办变化加 `reclaim` 5 项、`process` 8 项、`shell-waits` 5 项；普通启动 1 项，共 31 项 |
| 手动复位 Event、阻塞等待、Set/Reset/Close | `node 'Source Code/tests/host/test-event-waits.js' '<ShellBridge 构建目录>'`；8 项，先 `--list` | 调度/等待接入加 process 8；ABI/授权变化加 policy 2；SDK/启动能力位变化加普通启动 1。Event 独立专项不自动加入 core，复用同次构建、共享 VM 工具和私有盘 |
| 线程传参、返回、join/detach 与 TID | `node 'Source Code/tests/host/test-thread-waits.js' '<ShellBridge 构建目录>'`；8 项，先 `--list` | 创建/退出接入加 process 8；用户栈变化加 images 11；新 ABI/授权加 policy 2；公共等待撤销增加 join 占用清理时加 Event 8、completion-waits 8；普通启动 1。本轮组合 46 项。线程模式由 probe.snx（SNX v2）运行，独立专项不自动加入 core |
| PID 槽位/代数、陈旧标识、Object/cookie/TID 寿命 | `node 'Source Code/tests/host/test-pids.js' '<ShellBridge 构建目录>'`；8 项；`node 'Source Code/tests/host/test-identifiers.js'`；6 项；均先 `--list` | 进程资源变化加 process 8、历史完成记录加 completion 5、终端归属查找加 interactive 1、句柄代数/选槽变化加 policy 2、Event 8、线程 8；普通启动 1，共 47 项。独立专项不自动加入 core，不改统一 267 个 ID。真实负载消耗写入 PID 专项 shell-report 的 identifierAudit；耗尽边界用同一分配器的缩小范围及 ShellBridge 启动自检，不能在运行中重置用户编号 |
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

`dev.ps1 -VerifyOnly` 运行 `process` 套件（8 项）。`verify-system.js` 运行宿主键盘检查。

共享调度器/内存等底层机制确实影响多个接口时，应按实际影响追加，不能为少跑而漏掉必要边界。表中没有给出小入口的解析器、语言资源或存储故障检查，应先找到对应宿主测试或建立小专项，不以 `-Full` 代替选测。

task 子系统提取按实际接口选测：process 8 项、policy 2 项；Thread 等待成员迁移追加 shell-waits 5 项，普通启动 1 项。IRQ 嵌套恢复自检并入原有启动/进程用例，不新增 ID；契约见 [task-subsystem.md](../docs/task-subsystem.md)。

内核入口栈大小/生命周期变化选 process 8 项及 images 11 项，普通启动追加 1 项。KernelStack 的 9 页布局、共享内核 PTE 权限、边界读写、重复分配/释放、保护记录耗尽后的失败回滚与重试由启动自检覆盖，并在现有 process / boot 用例中断言标记，不新增 ID。编译器逐函数栈用量见 build32/*-normal.su；需要连同中断帧和实际调用链评估，不能当作整条调用链上界。

仅变更 VMA 区间树（`mm/vma.cpp`）时选普通启动 1 项。启动自检覆盖区间边界、插入和删除后的 AVL 不变量、节点身份及资源归还，现有 `boot.ordinary` 断言自检标记。

匿名 first-fit 选址变更选 `anonymous` 7 项及 `e2` 6 项，验证未驻留预留及部分解除映射后的空洞复用。宽映像避让与跨 PDE 探针变更加 `images` 11 项，调整选址与水位检查顺序时加 `reclaim` 5 项。选址算法的窗口边界由普通启动自检覆盖。

加载器或用户栈 VMA 登记变更选 `test-memory.js vma` 2 项（`vma --list`），检查映像登记失败回滚及工作线程栈登记失败后的槽位复用。映像或栈布局变化时按表选 `images` 或 `thread-waits`。

主栈或工作栈布局、线程创建/退出及系统调用退役共同变更时，选 `thread-waits` 8 项、`process,policy` 10 项、`images` 11 项、`vma` 2 项和普通启动 1 项，共 32 项。工作栈专项从线程入口 ESP 推导栈界限，覆盖 Guard 缺页、未领取记录时的地址复用，以及 Guard/Stack 登记、部分数据页和 join 对象申请失败后在同一进程内的资源对账。

物理页 buddy 分配器、slab 缓存或内核通用堆（kmalloc/kfree）核心变化，选 process 8 项、images 11 项、anonymous 7 项及普通启动 1 项，共 27 项，不新增 ID。启动自检覆盖 buddy 位压缩元数据、连续页分配、守护页与空闲链一致性；images 核对 E820 和 8 字节/PFN 元数据预算。普通启动和 process 启动用例核对 slab（大小类、跨页扩容、对象隔离、非法/重复释放、空页归还）及堆自检标记（大小类边界、清零、多页释放、页归属拒绝、页计数恢复）。调用模块从 kpage_alloc 迁移到 kmalloc/kzalloc 后按对应模块选测。

调度/主循环空闲决策变化选 process 8 项、shell-waits 5 项、普通启动 1 项；后台待办查询涉及网络与 RTC 时各加对应 1 项。process 原有启动/抢占用例包含就绪/到期状态自检、实际 idle_halt 的 `sti; hlt; ret` 字节、`probe.snx /schedule` 主动让出/有限睡眠/竞争读重试，以及有界候选和空闲统计检查，不新增固定 ID。`task_schedule_stats` 仅供宿主观察，不是用户 ABI。

## VM 配置基线

就绪/对象等待/定时队列共同迁移时，选 `process,completion-waits,pipe-waits,terminal-waits,shell-waits`，共 42 项；普通启动追加 1 项。真实集合算法另运行 `node 'Source Code/tests/host/test-task-queues.js'`（4 项，先 `--list`）：96 节点 FIFO/撤销、定时堆容量/取消、tick 回绕/同期限次序、移除重插。启动自检覆盖对象代数复用、重复通知及到期优先；`probe.snx /schedule` 覆盖通知后条件失效重试和剩余数据交接。宿主统计布局为 60 字节，新增目标队列遍历/有限唤醒/广播/定时到期计数，不是用户 ABI。

开发、发行与普通/接口验收默认 4096 MiB RAM、单核；QMP 报告实际配置。定向内存边界专项可显式选择 1024 或 2048 MiB。
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

高页/SNX 改用 `images`，水位回收使用 `reclaim`，可丢弃页使用 `purgeable`，双池水位和低端兜底使用 `pools`。按表中实际受影响的范围组合模式，不默认连续运行。普通启动检查使用 `dev.ps1 -BuildOnly` 的普通镜像。

## 临时测试与收尾

临时测试放在 `tests/development/<模块>/<功能>/`，先在 [INDEX.md](development/INDEX.md) 登记模块、命令、用例数和启用条件；AI 仅查看当前功能对应条目。只有当前功能开发或明确的缺陷调查才执行，不自动加入稳定注册表。阶段/功能完成时必须逐项决定：提升为稳定契约、合并重复、迁入压力专项或删除临时验证，并清理登记。具体规则见 [README.md](development/README.md)。
