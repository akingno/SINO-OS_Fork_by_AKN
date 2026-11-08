# SINO 系统架构

## 基础决策

保留 BIOS/MBR 引导链、自有内核、VFS 和 VGA 文字控制台。采用原生 ABI，不兼容 Win32/PE。GCC PE/COFF 仅作宿主链接中间格式，客户机加载 SNX 平面可执行文件。

## 硬件与引导

32 位 x86 保护模式，BIOS 引导。E820 分配器从 52 MiB 起分配物理页，当前上限为 1 GiB；低端仍有历史保留区，GUI 已移除。FAT12 预加载至 2 MiB。启动镜像 2.88 MB，El Torito 类型 3 软盘仿真，布局定义在 `src32/boot-layout.h`（NASM/C/JS 共用）。RAM 盘起始于 2 MiB；普通镜像不再打包图形资源。

快照存储使用 CRC 与交替提交持久化 `/home/user`。

## 内存管理（mm/）

内核内存子系统独立为 `src32/mm/` 模块：

- **物理页分配器**（`page.c`）：管理当前地址布局允许的 E820 可用物理页，支持连续页分配（`page_alloc_run`）与启动自检
- **分页管理**（`paging.c`）：
  - 启动阶段：`paging_bootstrap` 建立初始临时页表，`paging_kernel_init` 切换到正式内核页表（两阶段 CR3 不同）
  - 每进程独立页目录，内核恒等映射共享且仅限 Ring 0
  - 用户映像从 `0x40000000` 开始，代码页只读，数据页可写
  - 线程栈之间有未映射守护页
  - `paging_user_map` / `paging_user_find_gap` 支持跨 4 MiB 边界映射和动态地址窗口
- **用户缓冲区校验**（`uaccess.c`）：所有系统调用的用户指针经过页表权限检查，覆盖跨页、无效尾页和嵌套指针场景
- **匿名内存映射**（`process.c: anon_map / anon_unmap`）：用户程序可申请私有匿名页，首次访问时按需分配零填充物理页（lazy allocation）。配额耗尽时系统调用返回 `S_NOMEM`，释放后缺页则触发故障退出
- **SNX v2 格式**：用户程序内存上限从 192 KiB（v1）提升到 16 MiB，链接基址均为 `0x40000000`

## 进程与线程

8 个进程槽位，每进程最多 4 线程。每进程有独立页目录、专用 4 KiB 内核入口栈（TSS）。用户选择子 1B/23，IOPL=0，无 I/O 位图。PIT 每两个 tick 抢占用户代码；内核和系统调用有意不可抢占。上下文切换仅保存整数寄存器；ABI 1 程序不得使用 FPU/SIMD。

INT 0x80 提供有界的文件、进程、线程、同步事件、句柄和文本会话原语。CPL3 故障终止该进程，CPL0 故障仍然 panic。代际标记句柄检查对象类型和访问权限，进程退出时回收对象和地址空间。

## 权限策略

权限模型分两层：

- **兼容策略**：按启动时授予的能力访问旧式全局接口，供兼容程序和探针使用
- **严格策略**（`caps=0`）：默认拒绝一切，仅允许白名单中的系统调用。Shell、文件工具和新用户程序均运行在此策略下

`run -s` 启动的进程强制 caps=0，后续按白名单逐项增加标准流、显式文件授权等。旧 `run` 与已有应用继续兼容策略。

## 系统调用接口

通过 feature 位逐步启用，当前掩码为 32767，共 15 个已实现机制。

| Feature 位 | 值 | 说明 |
| --- | --- | --- |
| SYSTEM_INFO | 1 | 只读系统查询 |
| STRICT_POLICY | 2 | 严格进程策略 |
| START_INFO | 4 | 启动信息查询 |
| COMPLETION | 8 | 进程完成记录 |
| BOUNDED_RUN | 16 | 有限 CLI（参数、标准流、超时） |
| FILE_OBJECTS | 32 | 目录授权、文件对象、区间 I/O |
| PIPES | 64 | 管道（全局 8 个） |
| TERMINALS | 128 | 文本会话后端 |
| CALENDAR | 256 | 日历查询 |
| WORKSPACE | 512 | 写工作区与目录提交 |
| LINE_EDIT | 1024 | 行编辑接口 |
| ANON_MEMORY | 2048 | 匿名内存映射 |
| ANON_LAZY | 4096 | 按需分配 |
| SNX_V2 | 8192 | 16 MiB 用户程序格式 |
| THREAD_WAIT | 16384 | 管道、完成记录、终端字节和交互结果的线程级阻塞等待 |

详细契约见 [os-interface.md](os-interface.md)。

## 文字控制台与用户程序

普通镜像由内核启动唯一受信任的 `conhost.snx`，再启动严格 Ring 3 的 `gsh.snx`。
conhost 管理 128 行历史、键盘滚屏和输入行显示；内核提交 80×25 的固定单色 VGA 文字帧。
Shift+PageUp/PageDown 浏览历史时保持视口，并将当前输入光标所在行固定在底部。
ShellBridge 镜像保持独立无窗口测试通道。

GUI 窗口管理、绘图、事件队列、Forms 和图形语言资源已移除；旧系统调用编号保留并返回错误。
Shell、gcmd、tview 和 writer 均为 Ring 3 程序，启动代理按现有目录授权和标准流契约运行它们。

## RTC 与时间

`rtc.c` 是 SYS_TIME、顶栏和 Shell date/time 的唯一硬件采样源。采样和解码失败时保留最后缓存字节；样本超过 200 tick 时返回 `S_AGAIN`。SYS_TIME 仅验证输出字节并复制值，无硬件访问或能力提升。顶栏不依赖任何用户态时钟进程。

## 当前限制

- 无 SMP、阻塞式内核续体、动态链接器、用户账户或加固安全边界
- 全局 VFS 和桌面仍为遗留内核服务
- 文件权限仅区分可写用户 home 和受保护系统路径
- 旧 Ring 0 应用中的崩溃仍会影响内核
- 不是生产安全系统

## 参考

- [Intel SDM](https://www.intel.com/content/www/us/en/developer/articles/technical/intel-sdm.html) Volume 3：保护模式、分页和 TSS
- 原创项目代码，未导入 Windows/FreeDOS/Linux 内核实现
