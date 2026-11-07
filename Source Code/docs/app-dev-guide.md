# 用户态应用开发指南

用户态应用是独立运行在 Ring 3 的程序，包括 Shell、终端前端、文件管理器等，并非 Shell 的专用名称。

## 基本环境

使用 w64devkit GCC/GNU Binutils 和源码包含的 NASM 3.02 交叉编译。不链接宿主 CRT。应用入口为 `int main(void)`，退出或返回终止进程并释放句柄和地址空间。

`user/sdk.h` 和 `user/runtime.c` 定义 ABI 1 的 UI 命令、文件/进程/消息包装函数。INT 0x80 进入内核，所有用户指针经过页表权限检查。

## 构建流程

1. 在 `user/` 下编写应用源码
2. 在 `tools/build-user.js` 中添加构建元组
3. `dev.ps1 -BuildOnly` 构建内核和所有用户应用

构建检查：每个必需的链接符号、文件不超过 64 KiB、SNX v1 映像/数据不超过 192 KiB。SNX v2 上限 16 MiB。

## SNX 格式

SNX1 在 32 字节头中存储 magic、ABI 版本、映像大小、入口点、数据偏移和内存大小。宿主链接使用 PE/COFF 作中间格式，客户机不加载 PE 映像。SNX v2 使用相同基址 `0x40000000`，支持更大的用户程序。

## ABI 限制

- 仅整数 C（无 FPU/SIMD、libc、异常或动态链接）
- 8 个进程，每进程 4 线程
- 每进程 24 个本地句柄
- 每进程 15 个待处理输入事件
- 8 个桌面窗口
- 每线程 16 KiB 栈
- 线程函数必须调用 `SYS_THREAD_EXIT`，不能直接 return
- Read/Write 返回 `src32/api.h` 中定义的统一负数错误码
- Home 写能力不授权写入 `/boot`、`/etc` 或内核内存

## 窗口与绘图

Window/Scene/Event 提供 UI 展示。内核每画布最多验证复制 256 命令（旧 UiScene 保持 64 命令兼容）。不共享帧缓冲或内核指针。

绘图原语：

| 命令 | 说明 |
| --- | --- |
| UI_PANEL | 填充矩形 |
| UI_TEXT | 文本（Font 0/1/2） |
| UI_CELL_TEXT | 固定网格 UTF-8 文本行（终端用，Font 2） |
| UI_LINE | 有符号线段（±1000 × ±714） |
| UI_ELLIPSE | 有界填充椭圆 |
| UI_GRID | 网格 |
| UI_SELECTION | 选区 |

`ui_canvas_begin/fill/text/line/ellipse` 构建最多 256 命令的 UiCanvas，溢出返回 `S_LIMIT`。Font 2 ASCII 前进 7 像素，非 ASCII 前进 21 像素，每命令字符串上限 79 字节。

窗口策略标志（可组合）：

| 标志 | 值 | 说明 |
| --- | --- | --- |
| WINDOW_CLOSE_REQUEST | 1 | 接收关闭请求消息 |
| WINDOW_APP_KEYS | 2 | F5/F8 交给应用而非全局快捷键 |
| WINDOW_NO_IME | 4 | 非文本应用排除拼音输入 |
| WINDOW_DESKTOP_COMMANDS | 8 | 接收桌面命令消息 |

用户窗口最大 1000×714。

## 权限策略

### 兼容策略

继承原有全局文件和桌面访问。现有应用（文件管理器等）使用此策略。

### 严格策略（caps=0）

`run -s` 启动的进程默认拒绝一切，仅允许白名单中的系统调用。Shell、文件工具和新用户程序运行在此策略下。

受保护只读 boot 节点（`/boot/files.snx`、`/boot/term.snx` 等）的加载器在解析后移除所有能力位，不区分大小写或别名路径。

## 常用系统调用

### 查询

- `QuerySystemInfo(SystemInfo*)`：只读系统信息（48 字节），含 features 位
- `QueryStartInfo(StartInfo*)`：只读启动策略（24 字节）
- `UiLanguage()`：当前 UI 语言（0 中文，1 英文）
- `Time(TimeOfDay*)`：缓存的本地 RTC 时间（12 字节，时/分/秒）
- `CalendarTime(CalendarTime*)`：完整日期时间（32 字节）

### 窗口

- `WindowTitle(handle, title)`：设置窗口标题，最多 63 UTF-8 字节
- `GetWindowSize(handle, WindowSize*)`：获取逻辑客户区宽高

### 进程

- `SpawnTracked(path)`：兼容启动者可用，启动严格孩子并获得 WAIT/CANCEL 句柄
- `GetProcessResult(handle)`：非阻塞查询进程结果（正常退出/故障/取消/外部终止）
- `CancelProcess(handle)`：取消子进程

### 文件（Stage D，严格进程需显式目录授权）

- `GrantDirectory(path, rights)`：获取目录句柄
- `OpenAt(dir, name)`：相对打开文件
- `FileReadAt(file, offset, buf, len)` / `FileWriteAt(...)`：区间读写
- `FileStage(dir, name)` / `Publish(staged)` / `Commit(dir)`：暂存、发布、提交

### 管道（Stage E）

- `PipeCreate(capacity)`：创建管道（1–4096 字节）
- `StreamRead(handle, buf, len)` / `StreamWrite(...)`：读写管道端点
- `StreamReady(handle)`：查询就绪状态

### 终端会话（Stage F）

- `SYS_TERMINAL`：控制端和程序端操作，支持行/raw 模式、回显、Ctrl+C 等

### 交互启动（Stage G2）

原子绑定 CLI 参数、目录授权、定向标准流与会话。详见 [interactive-runs.md](interactive-runs.md)。

### 写工作区（Stage G5b）

- `SYS_WORKSPACE`：仅限由只读 boot/term.snx 发起的可信来源
- `SYS_DIRECTORY_COMMIT`：提交整代 home 快照

### 行编辑（Stage G5c）

- `TERM_EDIT` 等 4 项 SYS_TERMINAL 操作（17–20）

## 用户态运行库

`user/stream.h` 提供有截止时间的等待辅助函数：

- `StreamReadFor` / `StreamWriteFor` / `StreamWriteAllFor`：带超时的流操作
- `StreamWaitFor` / `RunWaitFor` / `ProcessWaitFor`：使用单调截止时间和 Yield

这些辅助函数使用 Yield 轮询，不是线程级阻塞等待。`S_TIMEOUT` 不自动取消子进程。

## 示例应用

- `user/hello.c`：最简示例
- `user/files.c`：文件管理器（CAP_HOME，桌面集成）
- `user/gsh.c`：SI-SHELL（caps=0，120×25 终端）

## 详细契约文档

- [os-interface.md](os-interface.md) — 系统调用接口（Stage A–F）
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
- [desktop-shell.md](desktop-shell.md) — 桌面终端入口
- [boot-image.md](boot-image.md) — 启动镜像布局
