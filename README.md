# SINO OS — AKN Fork

> 基于 [SkysoftStudio-Felix/SINO-OS](https://github.com/SkysoftStudio-Felix/SINO-OS) (`bf0cad1`) 的独立衍生版本。原版是一个用 GPT-6 辅助开发的 32 位 x86 桌面操作系统原型；本 fork 由 [akingno](https://github.com/akingno) 维护，借助 AI 工具重写内核与子系统，重点在于完善用户态与内核态的权限隔离和内存管理，底层完善与 CLI 应用开发。

## 图形桌面

SINO OS 提供经典窗口式图形桌面，支持中英文界面切换和拼音输入。菜单栏可以访问原生应用、文件操作、显示设置和工具。桌面图标支持拖放，右侧面板提供磁盘和应用程序快捷入口。

内置应用包括文件管理器（F1）、计算器（F4）、时钟（F6）、文本编辑器、SysBASIC 可视化 IDE（F11）、软件安装器（F8）和应用管理器（F9）。其中文件管理器、计算器和时钟已迁移到用户态（Ring 3），作为独立 `.snx` 程序运行，崩溃不影响系统。

## SI-SHELL — 用户态 Shell

按 F2、点击菜单中的终端或 Terminal.app 打开 SINO Shell（`gsh`），这是一个完全运行在 Ring 3 的命令行环境，120 列 × 25 行终端窗口。F12 可打开只读会话。

![SI-SHELL 在 QEMU 中运行](<Documentation/Screenshots/New Shell.png>)

Shell 支持以下命令：

| 类别 | 命令 |
| --- | --- |
| 导航 | `cd`、`pwd`、`ls`、`stat`、`tree`、`find` |
| 文件查看 | `cat`、`view`（分页）、`head`、`tail`、`wc`、`grep`、`hexdump` |
| 文件操作 | `write`、`cp`、`mv`、`sync`（提交到磁盘快照） |
| 系统信息 | `date`、`time`、`uptime`、`uname`、`ver`、`about`、`credits` |
| 程序与进程 | `run`、`install`、`kill`（仅限本会话子进程） |
| Shell 内建 | `echo`、`history`、`help`、`which`、`clear`、`status`、`exit` |

编辑功能：255 字节 UTF-8 行编辑、16 条历史召回（上下方向键）、命令和路径的 Tab 补全、Ctrl+C 取消、Ctrl+D EOF。底部状态栏显示快捷键提示。

## 内存管理

内核的内存子系统已独立为 `mm/` 模块：

- **物理页分配器**（`page.c`）：管理全部可用物理内存，支持连续页分配与自检
- **分页管理**（`paging.c`）：每个进程拥有独立页目录，内核映射共享且仅限 Ring 0 访问；用户映像从 `0x40000000` 开始，代码页只读，数据页可写，线程栈之间有未映射守护页
- **用户缓冲区校验**（`uaccess.c`）：所有系统调用的用户指针经过页表权限检查，跨页和无效尾页均有覆盖
- **匿名内存映射**：用户程序可申请私有匿名页，首次访问时按需分配零填充物理页（lazy allocation）；配额耗尽返回明确错误
- **SNX v2 格式**：用户程序内存上限从 192 KiB（v1）提升到 16 MiB

## 进程与权限隔离

系统支持 8 个进程槽位，每进程最多 4 线程。每个进程有独立页目录和专用的 4 KiB 内核入口栈（TSS）。PIT 每两个 tick 抢占用户代码。

权限模型分为两层：

- **兼容策略**：继承原有全局文件和桌面访问，供尚未迁移的旧应用使用
- **严格策略**（`caps=0`）：默认拒绝一切，仅允许白名单中的系统调用——新的 Shell、文件工具和用户程序均运行在此策略下

已实现的系统接口（通过 feature 位逐步启用，当前掩码 16383）：

- 进程完成记录与非阻塞结果查询
- 有限 CLI（参数传递、标准流捕获、超时）
- 目录授权与文件对象（显式 OpenAt、区间 I/O、版本化枚举）
- 管道（全局 8 个，EOF 与关闭语义）
- 文本会话（独立控制/程序端，行/raw 模式，回显与 Ctrl+C）
- 交互启动代理（原子绑定 CLI、目录、标准流与会话）
- 日历与时间查询、写工作区与目录提交、行编辑接口

## 当前限制

- 32 位 x86 BIOS/MBR，仅在 QEMU 上验证（标准 VGA + PS/2）
- 文本编辑器、系统设置、磁盘安装器仍在内核态
- 无 SMP、无动态链接、无用户账户、无 UEFI
- 网络仅回环验证
- 不是生产安全系统

## 仓库结构

```
Source Code/          唯一的开发源码目录
  src32/              内核（C + x86 汇编）
    mm/               内存管理子系统
  user/               Ring 3 用户态程序
  tools/              构建与测试工具
  docs/               技术契约文档
Documentation/        项目文档与截图
Build Output/         构建产物（不提交）
```

## 参与开发

以下模块尚未实现或仍需完善，欢迎贡献：

- **文件系统**：当前为全局 128 节点的 VFS 和 FAT12，缺少真正的文件权限、日志和大文件支持
- **网络**：仅有回环栈，宿主桥接、TCP/IP 和实际网络服务尚未实现
- **用户态应用**：文本编辑器仍在内核态，缺少更多 Ring 3 原生工具程序
- **图形与终端**：Shell 当前为纯文本，无 ANSI 转义序列、无脚本语法、无通配符展开
- **通用进程能力**：无 cwd/env 继承、无通用子进程委托、无线程级阻塞等待

也欢迎直接参与上游 [SINO-OS](https://github.com/SkysoftStudio-Felix/SINO-OS)，本 fork 的改动不代表上游方向。

## 许可

项目代码采用 [MIT](LICENSE)，保留上游版权。字体（SIL OFL）、拼音数据（MIT）、NASM（BSD-2-Clause）各自保留原许可，详见 [Documentation/Licenses](Documentation/Licenses) 和 [Source Code/SOURCES.md](<Source Code/SOURCES.md>)。

## 致谢

- 原项目：[SkysoftStudio-Felix/SINO-OS](https://github.com/SkysoftStudio-Felix/SINO-OS)（SINO OS 0.10 / SysBASIC 0.3.0）
- 本 fork 的开发借助了 Claude（Anthropic）和 Codex（OpenAI）
