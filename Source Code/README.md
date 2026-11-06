# SINO OS 源码 — AKN Fork

本目录是 SINO OS AKN fork 的唯一开发源码目录。基于上游 [SINO OS 0.10 / SysBASIC 0.3.0](https://github.com/SkysoftStudio-Felix/SINO-OS)（`bf0cad1`），主要改动集中在内核内存管理、进程隔离和用户态 Shell。

## 目录结构

```
src32/                内核（C + x86 汇编）
  mm/                 独立内存管理子系统（page.c, paging.c, uaccess.c）
  loader.asm          引导加载器
  kernel.c            内核主入口
  process.c           进程管理、系统调用分发、权限策略
  api.h               系统调用编号与用户态共享结构
  boot-layout.h       2.88 MB 启动镜像布局（NASM/C/JS 共用）
user/                 Ring 3 用户态程序
  gsh.c               SI-SHELL 主程序
  gcmd.c              Shell 外部命令工具
  tview.c             只读文件查看器
  write_tool.c        写工作区工具
  files.c             文件管理器
  calculator.c        计算器
  clock.c             时钟
  sdk.h               用户态 SDK（与 api.h 同步）
  runtime.c           用户态运行库
  linker.ld           用户态链接脚本（基址 0x40000000）
disk32/               启动盘内容与字体资源
tools/                构建、发布与共享开发工具
tests/                宿主测试、客户机探针与测试辅助代码
  dev-run.js          开发构建与 QEMU 启动
  shell-test.js       Shell 命令通道测试
  vm-test.js          QEMU 测试基础设施
  stage2-test.js      Stage 2 全量门禁
  build-user.js       用户态程序构建
  build-locale.py     双语 UI 资源生成与校验
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

## 构建

日常开发使用仓库根目录的 `启动开发版.cmd`，它调用 `dev.ps1` 完成编译、生成 ISO 并启动 QEMU。

只编译不启动：

    powershell -NoProfile -ExecutionPolicy Bypass -File ..\dev.ps1 -BuildOnly

需要：Node.js、能生成 i386 目标的 GCC/GNU Binutils、源码中包含的 NASM 3.02。工具链不在 PATH 时设置 `SINO_TOOLCHAIN` 环境变量。修改字体资源另需 Python 与 Pillow（`python tools\build-ui-assets.py`）；普通构建使用现有资源。

构建产物输出到 `Build Output/dev/runs/<构建编号>/`。

## 测试

Shell 命令通道测试：

    powershell -NoProfile -ExecutionPolicy Bypass -File ..\test.ps1 -SelfTest

Stage 2 全量门禁（8 组，含原生权限、资源回收、冷启动恢复等）：

    powershell -NoProfile -ExecutionPolicy Bypass -File ..\stage2-test.ps1

详细说明见 [docs/testing.md](docs/testing.md)。

## 技术参考

- [系统架构](docs/architecture.md)
- [系统调用接口](docs/os-interface.md)
- [启动镜像布局](docs/boot-image.md)
- [应用开发指南](docs/app-dev-guide.md)
- [Shell 契约](docs/user-shell.md)
- [SysBASIC 参考](docs/ide.md)
- [网络](docs/network.md)

## 许可

见 [LICENSE](LICENSE)、[SOURCES.md](SOURCES.md) 和 [Documentation/Licenses](../Documentation/Licenses)。
