# 构建与测试

日常 AI 选测先读 [模块与命令指南](../tests/TESTING.md)，只读相关条目；本文用于环境、构建和具体操作。开发中专项使用 [临时测试目录](../tests/development/README.md)，活动项登记在 INDEX 中。

## 日常开发

仓库根目录运行 `启动开发版.cmd`（调用 `dev.ps1`），完成编译、生成 ISO 并启动 QEMU 可见窗口。

| 意图 | 命令 | 说明 |
| --- | --- | --- |
| 编译并启动 | `.\启动开发版.cmd` | 重新构建并打开 QEMU 窗口；先关闭旧开发窗口 |
| 只编译 | `dev.ps1 -BuildOnly` | 生成 ISO，不启动 VM |
| 编译并验证 Ring 3 | `dev.ps1 -VerifyOnly` | 转入 process 无界面专项（8 项），使用私有盘 |

构建产物在 `Build Output/dev/runs/<构建编号>/`。GUI 已移除，可见 QEMU 窗口显示 80×25 文字控制台并自动进入 gsh；US 键盘直接输入，没有中文输入法。启动与中断信息仍写入 kernel.log。普通启动不自动发送键鼠输入或生成截图。文字控制台的 6 项基础检查与 3 项跨重启自动保存专项见 [text-console.md](text-console.md)。

## 测试源码位置

- `tests/host/` 存放宿主 C 检查、Node.js 套件和 PowerShell 验证入口。
- `tests/guest/` 存放编译为 SNX 的独立测试程序。`tools/build-user.js` 维护源码与镜像内程序名的对应关系。
- `tests/development/` 存放显式登记的开发中专项，不加入默认稳定回归；完成后逐项提升、合并、迁入压力或删除。
- `tests/support/` 存放测试夹具和套件辅助模块。构建、发布与测试共用的宿主工具仍在 `tools/`。

当前入口按功能命名；历史阶段入口及其 `tools/` 转发已移除。新用例和套件实现放入对应的测试目录。

## 无界面启动检查

从仓库根目录运行 `dev.ps1 -BuildOnly` 后，把该次输出的构建目录传给：

```powershell
node 'Source Code/tests/host/test-boot.js' 'Build Output/dev/runs/<本轮构建编号>'
```

检查该目录内 ISO 的哈希、El Torito 启动目录和内嵌启动镜像。随后从只读 CD 启动私有 VM，通过内核日志确认构建编号、FAT12 挂载、计时器中断和启动完成。检查普通镜像的 ShellBridge 隔离。每次执行使用新的空白数据盘，日志保存在构建目录下的 `boot-*`。此入口仅使用日志，无键鼠和截图操作。

## 发行启动器

`release/Start SINO.cmd` 和 `release/launch.ps1` 与 `SINO.iso` 放在同一目录。
启动器始终从 ISO 启动，首次运行创建 16 MiB 的 `Data/data.img`，个人文件由 Shell 的 `sync` 提交。
装包、安装向导、预装硬盘发布与升级流水线已删除。

旧 GUI 启动器测试已删除。

## 环境要求

- **宿主**：Windows，GCC/GNU Binutils（能生成 i386 目标），Node.js，Python（构建时 locale 校验），源码包含 NASM 3.02
- **环境变量**：`SINO_TOOLCHAIN`（GCC bin 目录）、`SINO_QEMU_DIR`（QEMU 目录）、`SINO_PYTHON`（Python 路径）
- **客户机**：32 位 x86，64 MiB RAM，标准 VGA/VBE，PS/2，BIOS/MBR，ATA IDE

构建检查内核不超过约 160 KB、BSS 在保留页表之前、用户文件不超过 64 KiB、SNX v1 映像/数据不超过 192 KiB。

## Shell 命令通道测试

从仓库根目录：

```
powershell -NoProfile -ExecutionPolicy Bypass -File .\test.ps1 -SelfTest -Full
```

构建带命令通道的专用 ISO，在私有无窗口 VM 中运行自测。参数：

- `-Command 'echo hello'`：执行单条命令
- `-SelfTest -Suite commands -Full`：112 项命令与语法检查
- `-ExpectedExitCode 1`：断言预期失败
- `-TimeoutSeconds 5`：单命令超时

报告和输出在 `Build Output/dev/runs/shell-.../`。通道仅在 `-ShellBridge` 构建中启用，普通开发构建不启用。

## 非 GUI 回归与独立持久化检查

日常开发先做普通镜像启动检查，再选与修改直接相关的专项。完整基础回归按需运行：

```powershell
.\regression.ps1 -Suite core -Full
```

`core` 由 23 个功能组、261 个用例组成，包含基础设施、RTC、Shell 通道/命令、进程、策略、完成记录、CLI、文件、管道、文本会话、交互代理、回环网络、独立持久化及测试器失败拒绝。它不调用终端窗口、QMP 键鼠、绘制场景或截图接口。宿主的终端/编辑模型属于无 GUI 逻辑测试，仍保留。

独立验证持久化：

```powershell
.\regression.ps1 -Suite persistence
```

共 4 项：通过无界面探针和命令通道提交二进制、文本数据，宿主核对 CRC 有效快照中的精确字节，再用同一 ISO 和私有盘副本冷启动读取。该入口不运行全量原生套件，也不依赖 GUI 数据或历史镜像。

两种选择都会构建带 ShellBridge 的新 ISO，保存同次构建的符号。报告位于 `Build Output/dev/runs/regression-.../regression-report.json`；查看摘要不重跑：

```powershell
.\test-summary.ps1 -RunDirectory 'Build Output/dev/runs/<本轮编号>'
```

## 独立功能套件与组合

```powershell
.\regression.ps1 -Suite process
.\regression.ps1 -Suite files
.\regression.ps1 -Suite 'policy,files'
```

`process` 为 8 项（含自己的冷启动），`policy` 为 2 项，`completion` 为 5 项，`files` 为 2 项（含自己的冷启动），`cli`、`pipes`、`terminal`、`interactive`、`network` 各 1 项；`managed-cli` 为 12 项。逗号组合共享同一次构建，各业务组仍在独立空白私有 VM 中执行。读回组只能使用自己写入组的私有盘副本，原盘哈希必须不变。未知名称在构建前拒绝；重叠选择按固定用例 ID 合并，每个用例只运行一次。

目录职责：

- `tests/host/suites/`：按功能组织的业务用例，禁止调用其他业务套件。
- `tests/support/guest-context.js`：命令通道、探针、资源计数和内存检查。
- `tests/support/regression-suites.js`、`regression-catalog.js`：组合关系、业务入口、显式冷启动种子。
- `tests/host/test-core.js`、`tests/support/regression-vm.js`：调度、私有 VM 生命周期、构建身份及结果检查。

报告按独立业务组展示；格式版本为 4，包含计划总数和实际执行的固定用例 ID。`test-report-summary.js --group contracts|selections|io` 分别验证 18/4/4 项报告契约，默认全部 26 项。

## 用例计划、数量限制与压力测试

```powershell
.\regression.ps1 -Suite 'process,files,process' -List
.\regression.ps1 -Suite stress
```

`-List` 只显示去重后的组、固定 ID 和用例数。默认总计最多 50 项、单组最多 20 项；超过时在构建前拒绝。用户显式手动全量运行可用 `-Full`，开发步骤自动验收仍遵守约定上限。`core`、50 项基础设施、25 项 Shell 通道和 112 项 Shell 命令都需要这个显式全量参数。普通单命令 `test.ps1 -Command ...` 不受整套自测影响；`test.ps1 -SelfTest` 同样先检查数量。

`stress` 有 9 项（基线准备 + 8 次恢复），从普通进程套件移出。完成、终端和 Shell 等待专项已删除重复的基础整套探针和其他专项的相同唤醒用例；SDK 专项中的普通内核 Shell 语法重复检查由 112 项命令套件负责。64 MiB 与 1 GiB 内存场景仍分别保留，不能以探针名相同为由合并。

一个探针执行算一个用例，其内部断言或命名条件另行保留。RTC 的反复缓存读取不再被当作上百个独立用例；内存模式由 13/18 个条件改为 9/5 次真实执行，覆盖条件均保留。当前全部入口与数量见 [test-catalog.md](test-catalog.md)。

## 独立 CLI 运行

```
powershell -NoProfile -ExecutionPolicy Bypass -File .\run.ps1 -Program /boot/clitest.snx -Arguments echo -InputFile .\input.bin
```

构建新 ISO，在严格进程中运行单个程序，取得独立 stdout/stderr 和最终状态。输入 ≤ 4 KiB，详见 [bounded-cli.md](bounded-cli.md)。

## 宿主键盘检查

```powershell
node 'Source Code/verify-system.js'
```

此入口仅运行 `tests/host/test-keyboard.js`，编译实际 keyboard.c 做扫描码逻辑检查（1 个宿主程序），不需要 ISO。原 GUI 套件、辅助模块和旧转发入口已删除；系统功能使用 `regression.ps1` 按模块选测，原 GUI 选择名称会在构建前被拒绝。

## src32 基础设施契约测试

```
node 'Source Code/tests/host/test-src32-refactor.js'
```

43 项独立测试：用实际 `storage.c` 配合内存磁盘和受控 VFS 边界编译运行，测试 klog 字节/十六进制输出和 locale 查询/格式化（含 UTF-8 名称和容量错误），以及资源生成器的缺失/重复键拒绝。不修改运行时源码或日常磁盘。

## 内存与阻塞等待专项

对显式指定的同次 ShellBridge 构建运行，均使用私有盘：

| 入口（位于 tests/host/） | 范围 | 用例数 |
| --- | --- | --- |
| `test-memory.js images <构建目录>` | 64 MiB / 1 GiB、高页、当前 SNX v1/v2、冷启动 | 9 |
| `test-memory.js anonymous <构建目录>` | 匿名映射、配额、I/O、故障与 OOM | 5 |
| `test-pipe-waits.js <构建目录>` | 管道阻塞与回收 | 10 |
| `test-completion-waits.js <构建目录>` | 完成记录阻塞等待 | 8 |
| `test-terminal-waits.js <构建目录>` | 终端阻塞读取 | 11 |
| `test-shell-waits.js <构建目录>` | SDK 与 Shell 等待集成 | 5 |

内存测试仅使用当前构建，不再比较历史阶段二进制。Shell 等待测试需要构建目录内的 `kernel-normal.exe`；历史收尾模式已合并到这一个功能入口。没有匹配构建时先运行 `regression.ps1 -BuildOnly`，准备 ShellBridge ISO 和同次符号而不连带运行其他测试；再将输出目录传给专项。此开关单独使用，不与 `-Suite`、`-List` 或 `-Full` 混用。普通启动仍使用 `dev.ps1 -BuildOnly` 的非桥接镜像。

## 测试约定

- 日常验收范围：启动、严格进程、文件与持久化、流与会话、Shell
- GUI 及其窗口、键鼠和截图测试已删除；宿主终端/编辑模型继续保留
- 每个开发步骤最多运行 50 项相关测试
- 运行前确认用例数，优先用摘要脚本查看结果
- 专项 VM 测试使用独立测试盘，不用日常开发盘

## 限制

- 无硬件在环、外部网卡/互操作测试
- 无安全认证或全面的畸形映像模糊测试
- 无桌面或窗口终端；普通镜像提供 VGA 文字 Shell，ShellBridge 镜像提供宿主命令通道
- 快照存储有有界目录/文件数，无断电硬件模拟器
