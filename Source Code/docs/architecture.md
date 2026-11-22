# SINO 系统架构

## 基础决策

使用 BIOS/MBR 引导链、自有内核、VFS 和 VGA 文字控制台。采用原生 ABI，不兼容 Win32/PE。GCC PE/COFF 仅作宿主链接中间格式，客户机加载 SNX 平面可执行文件。

## 硬件与引导

32 位 x86 保护模式，BIOS 引导。2.88 MB 的 FAT12 镜像以 El Torito 类型 3 软盘仿真启动，并预加载到物理地址 2 MiB。布局定义在 `src32/boot-layout.h`。E820 分配器管理 52 MiB 起、4 GiB 以下的可用物理页。内核直接映射到 1 GiB，高页通过 `kmap` 临时访问。

快照存储使用 CRC 与交替提交持久化 `/home/user`。

## 内存管理（mm/）

内核内存子系统独立为 `src32/mm/` 模块：

- **物理页分配器**（`page.cpp`、`page_internal.h`）：Buddy 双向链表管理低端和高端两个池的 E820 可用物理页，支持对齐拆分/合并及连续页线性兜底（`kpage_alloc_run`），启动自检覆盖位压缩元数据、zone 隔离与空闲链一致性
- **Slab 缓存**（`slab.cpp`）：8–1024 字节八个 power-of-2 大小类，整页管理，分配位图检测双释放，空页即还 buddy
- **内核通用堆**（`kmalloc.cpp`）：`kmalloc`/`kzalloc`/`kfree`，≤1024 字节走 slab。大请求优先取完整 buddy 块，快路径失败时搜索符合请求页数的连续区间。页归属隔离普通内核页与堆页。
- **分页管理**（`paging.cpp`）：
  - 启动阶段：`paging_bootstrap` 建立初始临时页表，`paging_kernel_init` 切换到正式内核页表（两阶段 CR3 不同）
  - 每进程独立页目录，内核恒等映射共享且仅限 Ring 0
  - 用户映像从 `0x40000000` 开始，代码页只读，数据页可写
  - 线程栈之间有未映射守护页
  - `paging_user_map` / `paging_user_find_gap` 支持跨 4 MiB 边界映射和动态地址窗口
- **用户缓冲区校验**（`uaccess.cpp`）：所有系统调用的用户指针经过页表权限检查，覆盖跨页、无效尾页和嵌套指针场景
- **用户虚拟区间**（`mm/vma.cpp`、`task/process.cpp`）：进程的 VMA 树登记映像、参数页、线程栈和匿名区。匿名映射按需分配零页，支持同一分配内的范围改权和部分解除映射。预留配额或 VMA 元数据耗尽时返回 `S_LIMIT`。物理页不足时走缺页重试或系统调用的 `S_NOMEM` 路径。
- **SNX v2 格式**：加载器接受版本 2，用户程序的内存映像上限为 16 MiB，链接基址为 `0x40000000`

## 进程与线程

64 个进程槽位，每进程最多 4 个线程和 24 个句柄。每进程有独立页目录和 32 KiB 内核入口栈，栈下方有 4 KiB 未映射保护页。用户选择子 1B/23，IOPL=0，无 I/O 位图。PIT 每两个 tick 抢占用户代码；内核和系统调用有意不可抢占。上下文切换仅保存整数寄存器；ABI 1 程序不得使用 FPU/SIMD。

INT 0x80 提供有界的文件、进程、线程、同步事件、句柄和文本会话原语。CPL3 故障终止该进程，CPL0 故障仍然 panic。代际标记句柄检查对象类型和访问权限，进程退出时回收对象和地址空间。

## 权限策略

权限模型分两层：

- **兼容策略**：按启动时授予的能力访问旧式全局接口，供兼容程序和探针使用
- **严格策略**（`caps=0`）：默认拒绝一切，仅允许白名单中的系统调用。Shell、文件工具和新用户程序均运行在此策略下

严格进程的标准流和文件权限通过显式句柄授予。ShellBridge 中的 `run -s` 选择严格策略。

## 系统调用接口

通过 feature 位逐步启用，当前掩码为 8388607，共 23 个机制。

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
| EVENT_WAIT | 32768 | Event 阻塞等待 |
| THREAD_JOIN | 65536 | 线程创建、join 和 detach |
| RECLAIM | 131072 | 进程回收优先级 |
| PROCESS_GROUP | 262144 | 进程组 |
| PURGEABLE | 524288 | 可丢弃匿名内存 |
| MPROTECT | 1048576 | 匿名映射读写保护 |
| MPROTECT_RANGE | 2097152 | 匿名分配内的范围改权 |
| ANON_UNMAP_RANGE | 4194304 | 匿名分配内的部分解除映射 |

详细契约见 [os-interface.md](os-interface.md)。

## 文字控制台与用户程序

普通镜像由内核启动唯一受信任的 `conhost.snx`，再启动严格 Ring 3 的 `gsh.snx`。
conhost 管理 128 行历史、键盘滚屏和输入行显示；内核提交 80×25 的固定单色 VGA 文字帧。
Shift+PageUp/PageDown 浏览历史时保持视口，并将当前输入光标所在行固定在底部。
ShellBridge 镜像保持独立无窗口测试通道。

Shell、gcmd、tview 和 writer 均为 Ring 3 程序，启动代理按现有目录授权和标准流契约运行它们。

## RTC 与时间

`rtc.cpp` 为 SYS_TIME 和 Shell 的 date/time 提供缓存样本。采样或解码失败时保留上次样本，样本超过 200 tick 时返回 `S_AGAIN`。SYS_TIME 校验输出缓冲区后复制缓存值。

## 当前限制

- 无 SMP、阻塞式内核续体、动态链接器、用户账户或加固安全边界
- 文件权限仅区分可写用户 home 和受保护系统路径
- 不是生产安全系统

## 参考

- [Intel SDM](https://www.intel.com/content/www/us/en/developer/articles/technical/intel-sdm.html) Volume 3：保护模式、分页和 TSS
- 原创项目代码，未导入 Windows/FreeDOS/Linux 内核实现
