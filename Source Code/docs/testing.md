# 构建与测试

## 日常开发

仓库根目录运行 `启动开发版.cmd`（调用 `dev.ps1`），完成编译、生成 ISO 并启动 QEMU 可见窗口。

| 意图 | 命令 | 说明 |
| --- | --- | --- |
| 编译并启动 | `.\启动开发版.cmd` | 重新构建并打开 QEMU 窗口；先关闭旧开发窗口 |
| 只编译 | `dev.ps1 -BuildOnly` | 生成 ISO，不启动 VM |
| 编译并验证 Ring 3 | `dev.ps1 -VerifyOnly` | 无窗口启动，通过键盘运行 probe 并保存截图 |

构建产物在 `Build Output/dev/runs/<构建编号>/`。

## 测试源码位置

- `tests/host/` 存放宿主 C 检查、Node.js 套件和 PowerShell 验证入口。
- `tests/guest/` 存放编译为 SNX 的独立测试程序。`tools/build-user.js` 维护源码与镜像内程序名的对应关系。
- `tests/support/` 存放测试夹具和套件辅助模块。构建、发布与测试共用的宿主工具仍在 `tools/`。

仓库根的 PowerShell 命令、`verify-system.js`、`verify-desktop.js` 和原 `tools/` 下的独立测试脚本入口可照常使用。新用例和套件实现放入对应的测试目录。

## 无界面启动检查

从仓库根目录运行 `dev.ps1 -BuildOnly` 后，把该次输出的构建目录传给：

```powershell
node 'Source Code/tests/host/test-boot.js' 'Build Output/dev/runs/<本轮构建编号>'
```

检查该目录内 ISO 的哈希、El Torito 启动目录和内嵌启动镜像。随后从只读 CD 启动私有 VM，通过内核日志确认构建编号、FAT12 挂载、计时器中断和启动完成。检查普通镜像的 ShellBridge 隔离。每次执行使用新的空白数据盘，日志保存在构建目录下的 `boot-*`。此入口仅使用日志，无键鼠和截图操作。

## 环境要求

- **宿主**：Windows，GCC/GNU Binutils（能生成 i386 目标），Node.js，Python（构建时 locale 校验），源码包含 NASM 3.02
- **环境变量**：`SINO_TOOLCHAIN`（GCC bin 目录）、`SINO_QEMU_DIR`（QEMU 目录）、`SINO_PYTHON`（Python 路径）
- **客户机**：32 位 x86，64 MiB RAM，标准 VGA/VBE，PS/2，BIOS/MBR，ATA IDE

构建检查内核不超过约 160 KB、BSS 在保留页表之前、用户文件不超过 64 KiB、SNX v1 映像/数据不超过 192 KiB。

## Shell 命令通道测试

从仓库根目录：

```
powershell -NoProfile -ExecutionPolicy Bypass -File .\test.ps1 -SelfTest
```

构建带命令通道的专用 ISO，在私有无窗口 VM 中运行自测。参数：

- `-Command 'echo hello'`：执行单条命令
- `-SelfTest -Suite commands`：112 项命令与语法检查
- `-ExpectedExitCode 1`：断言预期失败
- `-TimeoutSeconds 5`：单命令超时

报告和输出在 `Build Output/dev/runs/shell-.../`。通道仅在 `-ShellBridge` 构建中启用，普通开发桌面构建不启用。

## Stage 2 全量门禁

```
powershell -NoProfile -ExecutionPolicy Bypass -File .\stage2-test.ps1
```

构建新的带 ShellBridge 内核，在私有无窗口 VM 中运行 8 组测试，覆盖：

- 基础设施 / RTC / Shell 契约
- Ring 3 指针/权限/句柄/文件/资源检查
- 严格进程策略（Stage A poltest.snx）
- 完成记录（Stage B comptest.snx）
- 有限 CLI（Stage C 客户端套件 + managed-cli 协议）
- 文件对象与冷启动字节检查（Stage D fstest.snx）
- 管道与并发传输（Stage E pipetest.snx）
- 文本会话（Stage F termtest.snx）
- 桌面终端前端模型与 QMP 按键（Stage G）
- 回环网络
- 冷启动持久化与恢复
- 应用逻辑自测

排除画板、GUI 文本编辑器、视觉设计器交互和像素断言。

精简查看结果：

```
.\stage2-summary.ps1 -RunDirectory 'Build Output/dev/runs/<本轮编号>'
```

总状态、各组计数、异常片段。不重跑测试，正常通过时不展开成功子报告。

## 独立 CLI 运行

```
powershell -NoProfile -ExecutionPolicy Bypass -File .\run.ps1 -Program /boot/clitest.snx -Arguments echo -InputFile .\input.bin
```

构建新 ISO，在严格进程中运行单个程序，取得独立 stdout/stderr 和最终状态。输入 ≤ 4 KiB，详见 [bounded-cli.md](bounded-cli.md)。

## 完整回归门禁（23 组）

```
node 'Source Code/verify-system.js'
```

需要按 testing.md 准备普通和 SELFTEST 两种镜像。21 组覆盖：启动/应用、指针/键盘、文件/拖放、中文/拼音、设置/模式、安装/包管理、网络、IDE/断点、画板隐藏/文件管理器、桌面终端替换。其中 `tests/host/test-iso.js` 通过 QMP 启动 Shell 并保存截图。

## src32 基础设施契约测试

```
node 'Source Code/tests/host/test-src32-refactor.js'
```

43 项独立测试：用实际 `storage.c` 配合内存磁盘和受控 VFS 边界编译运行，测试 klog 字节/十六进制输出和 locale 查询/格式化（含 UTF-8 名称和容量错误），以及资源生成器的缺失/重复键拒绝。不修改运行时源码或日常磁盘。

## Stage 3 内存管理专项

```
node 'Source Code/tests/host/test-stage3-mm.js' g <本轮目录> <G前构建目录>
node 'Source Code/tests/host/test-stage3-mm.js' d <本轮目录>
```

使用私有 64 MiB / 1 GiB VM 和同一 ISO。`g` 测高页与 SNX v2，`d` 补测匿名内存。需要对显式指定的 ShellBridge 构建运行。

## 测试约定

- 日常验收范围：启动、严格进程、文件与持久化、流与会话、Shell
- GUI 整体退出日常验收（桌面、应用、终端窗口与渲染、菜单、快捷键、QMP 键鼠和截图检查）
- 每个开发步骤最多运行 50 项相关测试
- 运行前确认用例数，优先用摘要脚本查看结果
- 专项 VM 测试使用独立测试盘，不用日常开发盘

## 限制

- 无硬件在环、外部网卡/互操作测试
- 无安全认证或全面的畸形包/BASIC 模糊测试
- Ring 0 遗留应用的崩溃仍会影响内核
- 快照存储有有界目录/文件数，无断电硬件模拟器
- 包 CRC 检测损坏，不提供签名
