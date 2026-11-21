# SINO OS Fork by Akingno

SINO 是一个 32 位 x86 操作系统，专注内核、内存管理和用户态 CLI，启动后直接进入单色文字 Shell。

> Fork 于 [SkysoftStudio-Felix/SINO-OS](https://github.com/SkysoftStudio-Felix/SINO-OS) (`bf0cad1`) 的独立衍生版本。本 fork 独立维护内核与用户态 CLI。

## SI-SHELL — 用户态 Shell

Shell 支持以下命令：

| 类别       | 命令                                                                                     |
| ---------- | ---------------------------------------------------------------------------------------- |
| 导航       | `cd`、`pwd`、`ls`、`stat`、`tree`、`find`                                    |
| 文件查看   | `cat`、`view`（分页）、`head`、`tail`、`wc`、`grep`、`hexdump`             |
| 文件操作   | `touch`、`mkdir`、`rm`、`rmdir`、`write`、`append`、`cp`、`mv`、`sync` |
| 系统信息   | `date`、`time`、`uptime`、`uname`、`ver`、`about`、`credits`               |
| 程序与进程 | `run`、`kill`、`ps`                                                                |
| Shell 内建 | `echo`、`history`、`help`、`which`、`clear`、`status`、`exit`              |

支持 255 字节行编辑、16 条历史（上下方向键）、命令和路径的 Tab 补全、Ctrl+C 取消、Ctrl+D EOF。Shift+PageUp / Shift+PageDown 浏览最多 128 行历史；浏览时新输出保持视口，当前输入行固定显示在最底行。

文件命令由用户态 `tview.snx` 和 `writer.snx` 等工具执行。Shell 的逻辑根目录对应 `/home/user`，成功修改文件后自动提交用户快照；保存失败时可用 `sync` 重试。当前显示与键盘输入仅支持 ASCII，文件中的 UTF-8 数据保留。控制台交互详见 [文字控制台契约](<Source Code/docs/text-console.md>)。

## 内存管理

内核的内存子系统在 `mm/` 和 `task/` 模块，已实现：

- E820 物理页管理、分阶段初始化（启动池 → 内核页表 → 高内存扩展）
- 低端和高端 buddy 页池、slab 缓存以及内核通用堆 `kmalloc` / `kzalloc` / `kfree`
- 每进程独立页目录，用户映像从 `0x40000000` 起，代码只读、栈有守护页
- 用户地址校验与安全复制（`uaccess`）
- VMA 区间树管理用户映射。匿名内存按需分配，每进程最多 32 份分配、96 个匿名片段和 16 MiB 预留空间，支持范围改权与部分解除映射
- 1 GiB 以上高页通过 `kmap/kunmap` 临时访问
- 逐进程物理页计量（`pages_used`），用于回收决策
- 分级水位线（Normal / Low / Critical），按空闲页占已发布页池的比例划分
- 内存压力时按回收优先级自动终止进程，每轮最多一个受害者，负分进程受保护
- 可丢弃匿名内存（purgeable），用户程序可标记缓存区为可回收；内存压力时内核优先丢弃未锁定的缓存页，避免终止进程

详见 [内存访问契约](<Source Code/docs/mm-access.md>)。

## 进程与线程

任务子系统在 `task/` 模块，已实现：

- 64 进程 × 4 线程（slot 0 主线程，1-3 worker），每进程独立页目录与 32 KiB 内核栈 + 4 KiB 保护页
- 对象每槽独立 32 位 generation，句柄 cookie 跨所有进程均衡分配
- joinable 线程支持单消费者阻塞 join（带超时）与 detach；退出先释放栈，记录在消费后回收
- 侵入式 FIFO 就绪队列，用户态每 2 tick 抢占，内核不可抢占；空闲时 `sti; hlt` 休眠
- 管道读写、终端读取、进程完成、Event 和 join 均支持带超时的线程级阻塞，等待按对象代数校验
- 手动复位 Event 支持显式阻塞与广播唤醒
- 回收优先级评分（−1000 ~ 400），按前后台状态动态调整；进程组共享回收命运

权限模型：严格策略（`caps=0`）默认拒绝，仅白名单授权——新程序均在此策略下运行；兼容策略按启动时授予的能力访问兼容接口。

详见 [任务子系统契约](<Source Code/docs/task-subsystem.md>) 与 [编号生命周期](<Source Code/docs/identifier-lifetime.md>)。

## 当前限制

- 32 位 x86 BIOS/MBR，仅在 QEMU 上验证（标准 VGA + PS/2）
- 文字控制台使用 ASCII 显示与 US 键盘输入
- VFS 全局最多 128 个节点，单文件上限 64 KiB；`/boot` 为只读 FAT12 启动卷
- 无 SMP、无动态链接、无用户账户、无 UEFI
- 网络仅回环验证
- 不是生产安全系统

## 仓库结构

```
Source Code/          唯一的开发源码目录
  src32/              内核（C++17 + x86 汇编）
    mm/               内存管理子系统
    task/             进程、线程与调度
    sync/             单核同步基础
  user/               Ring 3 用户态程序
  tools/              构建、发布与共享开发工具
  tests/              宿主测试、客户机探针与测试辅助代码
  docs/               技术契约文档
Documentation/        项目文档与许可
Build Output/         构建产物（不提交）
```

默认运行配置为 **4 GiB RAM、单核**，目标为约 1–2 GHz 的单核 PC。32 位非 PAE 根据 E820 管理 4 GiB 以下的可用页，不保证全部安装内存可用。

## 构建与验证

Windows 下运行 `启动开发版.cmd`，重新构建并启动 QEMU；仅构建使用：

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File .\dev.ps1 -BuildOnly
```

源码使用 freestanding C++17，关闭异常和 RTTI。构建需要 Node.js、Python、支持 i386 目标的 G++ / GNU Binutils 和仓库内的 NASM，运行需要 QEMU。环境与目录说明见 [源码 README](<Source Code/README.md>)；验证按 [模块选测指南](<Source Code/tests/TESTING.md>) 选择相关专项。

## 许可

项目代码采用 [MIT](LICENSE)，保留上游版权。随源码提供的 NASM 采用 BSD-2-Clause，详见 [Documentation/Licenses](Documentation/Licenses) 和 [Source Code/SOURCES.md](<Source Code/SOURCES.md>)。
