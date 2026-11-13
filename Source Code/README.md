# SINO OS 源码 — AKN Fork

本目录是 SINO OS AKN fork 的唯一开发源码目录。基于上游 [SINO OS 0.10 / SysBASIC 0.3.0](https://github.com/SkysoftStudio-Felix/SINO-OS)（`bf0cad1`），主要改动集中在内核内存管理、进程隔离和用户态 Shell。

## 目录结构

```
src32/                内核（C++17 + x86 汇编）
  mm/                 内存管理（page.cpp, paging.cpp, uaccess.cpp, kmap.cpp）
  fs/                 文件系统（boot_fat12.cpp）
  lib/                内核工具库（kstring.cpp）
  loader.asm          引导加载器
  kernel.cpp          内核主入口
  process.cpp         进程管理、系统调用分发、权限策略
  panic.cpp           panic 与停机处理
  api.h               系统调用编号与用户态共享结构
  boot-layout.h       2.88 MB 启动镜像布局（NASM/C/JS 共用）
user/                 Ring 3 用户态程序
  console_host.cpp    VGA 文字控制台前端（conhost.snx）
  gsh.cpp             SI-SHELL 主程序
  gcmd.cpp            Shell 外部命令工具
  tview.cpp           只读文件查看器
  write_tool.cpp      写工作区工具（writer.snx）
  sdk.h               用户态 SDK（与 api.h 同步）
  runtime.cpp         用户态运行库与匿名映射堆
  linker.ld           用户态链接脚本（基址 0x40000000）
disk32/               启动盘内容与字体资源
tools/                构建、发布与共享开发工具
  build-user.js       用户态程序构建
  build-locale.py     语言资源生成与校验
  vm-test.js          QEMU 测试基础设施
  shell-test.js       Shell 命令通道测试
tests/                宿主套件、Ring 3 测试探针与测试辅助模块
  host/               宿主测试脚本（test-core.js、test-*-waits.js 等）
  guest/              Ring 3 客户机测试程序（编译为 .snx）
  support/            共享夹具与用例数据
docs/                 技术契约文档
  architecture.md     系统架构
  os-interface.md     系统调用接口（Stage A–F）
  boot-image.md       启动镜像布局与扩展
  user-shell.md       Shell 契约
  file-objects.md     文件对象与目录授权
  stream-pipes.md     管道接口
  terminal-sessions.md  文本会话后端
  app-dev-guide.md    应用开发指南
  testing.md          构建与测试说明
```

默认运行配置为 **4 GiB RAM、单核**，目标为约 1–2 GHz 的单核 PC。32 位非 PAE 根据 E820 管理 4 GiB 以下的可用页，不保证全部安装内存可用；64 MiB 运行兼容不再作为目标。

## 构建

日常开发使用仓库根目录的 `启动开发版.cmd`，它调用 `dev.ps1` 完成编译、生成 ISO 并启动 QEMU。

只编译不启动：

    powershell -NoProfile -ExecutionPolicy Bypass -File ..\dev.ps1 -BuildOnly

需要：Node.js、能生成 i386 目标的 G++ / GNU Binutils、QEMU、源码中包含的 NASM 3.02。工具链不在 PATH 时设置 `SINO_TOOLCHAIN` 环境变量；QEMU 不在 PATH 时设置 `SINO_QEMU_DIR`。内核与用户程序使用 freestanding C++17，关闭异常和 RTTI，不依赖宿主 C++ 运行库。

构建产物输出到 `Build Output/dev/runs/<构建编号>/`。

## 当前运行方式与内存管理

普通镜像自动进入 VGA 80×25 单色文字控制台，由 Ring 3 的 conhost 和 gsh 提供交互。Shift+PageUp / Shift+PageDown 浏览历史，浏览期间保留视口和底行输入。旧 GUI、桌面应用和图形验收入口已删除。

内存子系统通过 E820 解析、动态页状态元数据、分阶段页发布和 kmap 管理 4 GiB 地址范围内的可用物理页。内核直接映射上限为 1 GiB，用户页优先从高页分配；每进程独立页表、栈保护页和用户复制校验提供隔离。匿名区按需分配零页，SDK malloc/free 使用匿名映射；SNX v2 映像内存上限为 16 MiB。

当前低端直接映射仍为 VA == PA，尚未实现 buddy / slab、COW 与用户态小对象堆。接口边界见 [mm-access.md](docs/mm-access.md)。

## 测试

先读 [模块选测指南](tests/TESTING.md)，按改动选择相关测试。以下命令从仓库根目录执行：

```powershell
.\regression.ps1 -Suite process -List
.\regression.ps1 -Suite process
.\regression.ps1 -Suite persistence   # 4 项独立持久化检查
```

可用 `regression.ps1 -Suite process`、`-Suite files` 或 `-Suite 'policy,files'` 独立选择与组合功能。

文字控制台先运行 `dev.ps1 -BuildOnly`，再将本次普通构建目录传给 `node 'Source Code/tests/host/test-console.js'`（9 项）。ShellBridge 镜像用于独立命令与接口验证，不自动启动控制台 Shell。

运行前可用 `regression.ps1 -Suite process -List` 查看固定 ID 和数量；重复选择自动去重。压力恢复循环移到 `-Suite stress`（9 项）。完整入口清单见 [当前测试目录](docs/test-catalog.md)。

详细说明见 [docs/testing.md](docs/testing.md)。

## 技术参考

- [系统架构](docs/architecture.md)
- [系统调用接口](docs/os-interface.md)
- [内存访问契约](docs/mm-access.md)
- [文字控制台](docs/text-console.md)
- [启动镜像布局](docs/boot-image.md)
- [应用开发指南](docs/app-dev-guide.md)
- [Shell 契约](docs/user-shell.md)
- [网络](docs/network.md)

## 许可

见 [LICENSE](LICENSE)、[SOURCES.md](SOURCES.md) 和 [Documentation/Licenses](../Documentation/Licenses)。
