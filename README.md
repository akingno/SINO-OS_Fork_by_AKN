# SINO OS — AKN Fork

当前版本专注内核、内存管理和用户态 CLI，启动后直接进入单色文字 Shell。桌面、窗口系统和图形应用已暂时移除（最后有gui的版本在 old_gui 分支）。

> Fork 于 [SkysoftStudio-Felix/SINO-OS](https://github.com/SkysoftStudio-Felix/SINO-OS) (`bf0cad1`) 的独立衍生版本。原版是一个用 GPT-6 辅助开发的 32 位 x86 桌面操作系统原型；本 fork 由 [akingno](https://github.com/akingno) 维护，借助 AI 工具重写内核与子系统，重点在于完善用户态与内核态的权限隔离，内存管理，底层完善与 CLI 应用开发。

## SI-SHELL — 用户态 Shell

普通镜像通过 VGA 80×25 文字控制台自动启动 `conhost.snx` 和 `gsh.snx`。控制台前端和 Shell 均运行在 Ring 3；内核只向受信任的 conhost 授予屏幕与键盘接口。

Shell 支持以下命令：

| 类别 | 命令 |
| --- | --- |
| 导航 | `cd`、`pwd`、`ls`、`stat`、`tree`、`find` |
| 文件查看 | `cat`、`view`（分页）、`head`、`tail`、`wc`、`grep`、`hexdump` |
| 文件操作 | `touch`、`mkdir`、`rm`、`rmdir`、`write`、`append`、`cp`、`mv`、`sync` |
| 系统信息 | `date`、`time`、`uptime`、`uname`、`ver`、`about`、`credits` |
| 程序与进程 | `run` （未完善）|
| Shell 内建 | `echo`、`history`、`help`、`which`、`clear`、`status`、`exit` |

支持 255 字节行编辑、16 条历史（上下方向键）、命令和路径的 Tab 补全、Ctrl+C 取消、Ctrl+D EOF。Shift+PageUp / Shift+PageDown 浏览最多 128 行历史；浏览时新输出保持视口，当前输入行固定显示在最底行。

文件命令由用户态 `tview.snx` 和 `writer.snx` 等工具执行。Shell 的逻辑根目录对应 `/home/user`，成功修改文件后自动提交用户快照；保存失败时可用 `sync` 重试。当前显示与键盘输入仅支持 ASCII，文件中的 UTF-8 数据保留。控制台交互详见 [文字控制台契约](<Source Code/docs/text-console.md>)。

## 内存管理

内核的内存子系统在 `mm/` 模块，已实现：

- E820 物理页管理、分阶段初始化（启动池 → 内核页表 → 高内存扩展）
- 每进程独立页目录，用户映像从 `0x40000000` 起，代码只读、栈有守护页
- 用户地址校验与安全复制（`uaccess`）
- 匿名内存按需分配（每进程 32 区域 / 16 MiB），`malloc/free` 已接入
- 1 GiB 以上高页通过 `kmap/kunmap` 临时访问

尚未实现：buddy / slab 分配器、共享页引用计数、COW、小对象堆复用。详见 [内存访问契约](<Source Code/docs/mm-access.md>)。

## 进程与权限隔离

系统支持 8 个进程槽位，每进程最多 4 线程。每个进程有独立页目录和专用的 4 KiB 内核入口栈（TSS）。PIT 每两个 tick 抢占用户代码。

权限模型分为两层：

- **兼容策略**：保留部分旧式系统调用访问；已删除的 GUI 接口返回错误
- **严格策略**（`caps=0`）：默认拒绝一切，仅允许白名单中的系统调用——新的 Shell、文件工具和用户程序均运行在此策略下

已实现的系统接口：

- 进程完成记录与结果查询（支持阻塞等待）
- 有限 CLI（参数传递、标准流捕获、超时）
- 目录授权与文件对象（显式 OpenAt、区间 I/O、版本化枚举）
- 管道（全局 8 个，EOF 与关闭语义，支持阻塞读写）
- 文本会话（独立控制/程序端，行/raw 模式，回显与 Ctrl+C，支持阻塞读取）
- 交互启动代理（原子绑定 CLI、目录、标准流与会话）
- 线程级阻塞等待（管道、终端、进程完成均支持带超时的内核阻塞，调度器跳过休眠线程）
- 日历与时间查询、写工作区与目录提交、行编辑接口

## 当前限制

- 32 位 x86 BIOS/MBR，仅在 QEMU 上验证（标准 VGA + PS/2）
- 文字控制台仅支持 ASCII 显示与 US 键盘输入，尚无交互式文本编辑器
- VFS 全局最多 128 个节点，单文件上限 64 KiB；`/boot` 为只读 FAT12 启动卷
- 无 SMP、无动态链接、无用户账户、无 UEFI
- 网络仅回环验证
- 不是生产安全系统

## 仓库结构

```
Source Code/          唯一的开发源码目录
  src32/              内核（C++17 + x86 汇编）
    mm/               内存管理子系统
  user/               Ring 3 用户态程序
  tools/              构建、发布与共享开发工具
  tests/              宿主测试、客户机探针与测试辅助代码
  docs/               技术契约文档
Documentation/        项目文档、许可与历史截图
Build Output/         构建产物（不提交）
```

默认运行配置为 **4 GiB RAM、单核**，目标为约 1–2 GHz 的单核 PC。32 位非 PAE 根据 E820 管理 4 GiB 以下的可用页，不保证全部安装内存可用；64 MiB 运行兼容不再作为目标。

## 构建与验证

Windows 下运行 `启动开发版.cmd`，重新构建并启动 QEMU；仅构建使用：

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File .\dev.ps1 -BuildOnly
```

源码使用 freestanding C++17，关闭异常和 RTTI。构建需要 Node.js、支持 i386 目标的 G++ / GNU Binutils、QEMU 和仓库内的 NASM。环境与目录说明见 [源码 README](<Source Code/README.md>)；验证按 [模块选测指南](<Source Code/tests/TESTING.md>) 选择相关专项。

## 参与开发

以下模块尚未实现或仍需完善，欢迎贡献：

- **文件系统**：当前为全局 128 节点的 VFS 和 FAT12，缺少真正的文件权限、日志和大文件支持
- **网络**：仅有回环栈，宿主桥接、TCP/IP 和实际网络服务尚未实现
- **内存管理**：完善连续页搜索、临时映射契约和启动资源回收；按实际需求引入小对象堆、buddy / slab 与 COW
- **用户态应用**：增加交互式文本编辑器和更多 Ring 3 工具程序
- **Shell 与控制台**：完善命令行体验、脚本语法和通配符展开
- **通用进程能力**：无 cwd/env 继承、无通用子进程委托

也欢迎直接参与上游 [SINO-OS](https://github.com/SkysoftStudio-Felix/SINO-OS)，本 fork 的改动不代表上游方向。

## 许可

项目代码采用 [MIT](LICENSE)，保留上游版权。字体（SIL OFL）、拼音数据（MIT）、NASM（BSD-2-Clause）各自保留原许可，详见 [Documentation/Licenses](Documentation/Licenses) 和 [Source Code/SOURCES.md](<Source Code/SOURCES.md>)。

## 致谢

- 原项目：[SkysoftStudio-Felix/SINO-OS](https://github.com/SkysoftStudio-Felix/SINO-OS)（SINO OS 0.10 / SysBASIC 0.3.0）
- 本 fork 的开发借助了 Claude　和 Codex
