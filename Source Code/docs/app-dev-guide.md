# 用户态应用开发指南

用户态应用是独立运行在 Ring 3 的程序，包括 Shell、控制台宿主、文件工具等，并非 Shell 的专用名称。

## 基本环境

使用 w64devkit GCC/GNU Binutils 和源码包含的 NASM 3.02 交叉编译。不链接宿主 CRT。应用入口为 `int main(void)`，退出或返回终止进程并释放句柄和地址空间。

`user/sdk.h` 和 `user/runtime.cpp` 定义原生 ABI 的文件、进程、流和文本会话包装函数。INT 0x80 进入内核，所有用户指针经过页表权限检查。

## 构建流程

1. 在 `user/` 下编写应用源码
2. 在 `tools/build-user.js` 中登记产物名、源文件和 MODE
3. `dev.ps1 -BuildOnly` 构建内核和所有用户应用

构建检查包括必需的链接符号、64 KiB 文件上限和 16 MiB 的 SNX v2 内存映像上限。

内置 Ring 3 C++ 程序及 SDK 运行时共用 `tools/build-user.js`，启用 GCC `-fstack-clash-protection` 为较大的固定大小栈帧和动态栈分配生成 4 KiB 逐页探测，让越界访问在 Guard 内触发缺页。独立构建的 SNX 和手写汇编需自行保证逐页访问。更换编译器或目标时应复核两种栈分配的指令和 freestanding 链接。

## SNX 格式

SNX v2 的 32 字节头记录 magic、版本 2、映像大小、入口点、数据偏移和内存大小。链接基址为 `0x40000000`。宿主链接以 PE/COFF 为中间格式，客户机加载转换后的 SNX 文件。旧版 SNX v1 程序需要重新构建。

## ABI 限制

- 使用 freestanding C++17 和整数指令，无 FPU/SIMD、宿主 libc、异常或动态链接
- 64 个进程，每进程最多 4 个线程
- 每进程 24 个本地句柄
- 每线程 16 KiB 可用栈，紧邻下方有 16 KiB 无映射保护区。工作栈由内核选址
- `sino::thread::create` 启动的线程可以直接返回退出码，或通过 `sino::thread::exit` 提交退出码
- Read/Write 返回 `src32/api.h` 中定义的统一负数错误码
- Home 写能力不授权写入 `/boot`、`/etc` 或内核内存

## 文字控制台

普通镜像由受信任的 `conhost.snx` 持有 SYS_CONSOLE 设备权限，使用 VGA 80×25 单色文字帧。
应用通过文本会话和标准流收发数据，不直接访问显存或键盘。Shift+PageUp/PageDown 由 conhost 处理，历史浏览时输入光标所在行固定在底部。

## 权限策略

### 兼容策略

按启动时授予的能力访问旧式全局接口，主要供兼容程序和探针使用。

### 严格策略（caps=0）

严格进程仅能调用白名单中的系统调用。Shell、文件工具和新用户程序运行在此策略下。

受保护只读 boot 节点的加载器在解析后移除所有能力位，不区分大小写或别名路径。

## 常用系统调用

### 查询

- `QuerySystemInfo(SystemInfo*)`：只读系统信息（48 字节），含 features 位
- `QueryStartInfo(StartInfo*)`：只读启动策略（24 字节）
- `UiLanguage()`：当前 UI 语言（0 中文，1 英文）
- `Time(TimeOfDay*)`：缓存的本地 RTC 时间（12 字节，时/分/秒）
- `CalendarTime(CalendarTime*)`：完整日期时间（32 字节）

### 进程

- `SpawnTracked(path)`：兼容启动者可用，启动严格孩子并获得 WAIT/CANCEL 句柄
- `GetProcessResult(handle)`：非阻塞查询进程结果（正常退出/故障/取消/外部终止）
- `CancelProcess(handle)`：取消子进程

### 文件

严格进程使用显式目录授权。

- `GrantDirectory(path, rights)`：兼容启动者获取目录句柄
- `OpenAt(dir, name)`：相对打开文件
- `FileReadAt(file, offset, buf, len)` / `FileWriteAt(...)`：区间读写
- `FileStage(dir, name)` / `Publish(staged)` / `Commit(dir)`：暂存、发布、提交

### 管道

- `PipeCreate(capacity)`：创建管道（1–4096 字节）
- `StreamRead(handle, buf, len)` / `StreamWrite(...)`：读写管道端点
- `StreamReady(handle)`：查询就绪状态

### 终端会话

- `SYS_TERMINAL`：控制端和程序端操作，支持行/raw 模式、回显、Ctrl+C 等

### 交互启动

原子绑定 CLI 参数、目录授权、定向标准流与会话。详见 [interactive-runs.md](interactive-runs.md)。

### 写工作区

- `SYS_WORKSPACE`：由内核启动的可信 conhost 获取工作区授权后，按交互协议委托给 Shell 和写工具
- `SYS_DIRECTORY_COMMIT`：提交整代 home 快照

### 行编辑

- `TERM_EDIT` 等 4 项 SYS_TERMINAL 操作（17–20）

## 用户态运行库

`user/stream.h` 提供有截止时间的等待辅助函数：

- `StreamReadFor` / `StreamWriteFor` / `StreamWriteAllFor`：带超时的流操作
- `StreamWaitFor` / `RunWaitFor` / `ProcessWaitFor`：使用单调截止时间和 Yield

这些辅助函数使用 Yield 轮询，不是线程级阻塞等待。`S_TIMEOUT` 不自动取消子进程。

## 示例应用

- `user/console_host.cpp`：受信任的文字控制台宿主
- `user/gsh.cpp`：严格 Ring 3 Shell
- `user/tview.cpp`、`user/write_tool.cpp`：读取和写入文件的工具

## 详细契约文档

- [os-interface.md](os-interface.md) — 系统调用接口
- [bounded-cli.md](bounded-cli.md) — 有限 CLI
- [file-objects.md](file-objects.md) — 文件对象与目录授权
- [stream-pipes.md](stream-pipes.md) — 管道
- [terminal-sessions.md](terminal-sessions.md) — 文本会话后端
- [interactive-runs.md](interactive-runs.md) — 交互启动
- [user-shell.md](user-shell.md) — Shell 命令与契约
- [file-tools.md](file-tools.md) — 只读文件工具
- [shell-tools.md](shell-tools.md) — 日常命令
- [workspace-shell.md](workspace-shell.md) — 写工作区
- [shell-editing.md](shell-editing.md) — 行编辑
- [text-console.md](text-console.md) — 开机文字控制台
- [boot-image.md](boot-image.md) — 启动镜像布局
