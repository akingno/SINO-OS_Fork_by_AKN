# Contributing to SINO / 参与 SINO

SINO is an experimental C/x86 assembly operating system and BASIC-subset development environment. Start with a release in QEMU and read the [scope and limitations](<Documentation/Release Notes.md>) before proposing a change.

SINO 是实验性操作系统与 BASIC 子集开发环境。建议先在 QEMU 中体验，再阅读范围与限制。

## Useful starting points / 从哪里开始

- Clarify English/Chinese documentation and the first-program tutorial.
- Add small, runnable BASIC examples using the supported language subset.
- Add regression cases for the keyboard decoder, parser or package validator.
- Improve UI layout within the existing memory/drawing limits.

文档与翻译、小型可运行示例、键盘/解释器/安装包边界测试，以及有明确范围的界面改进，都是适合开始的任务。

## Report a bug / 反馈问题

Use [this fork's Issues](https://github.com/akingno/SINO-OS_Fork_by_AKN/issues). Include the release or commit, QEMU version, guest resolution/scaling, exact steps, expected/actual behavior, and a screenshot or relevant error log when available. Say whether you used the preinstalled disk or installed from ISO. Identify upstream-only issues separately before reporting them to the original project.

反馈时请注明版本、QEMU 版本、分辨率与缩放、操作步骤、预期与实际结果，以及截图或相关错误日志；说明使用预装盘还是 ISO 安装。

## Make a change / 提交改动

Keep each pull request focused. Preserve the BIOS boot path and existing GUI. Document new limits instead of presenting unsupported features as complete. Follow the existing toolchain and keep third-party license notices.

Build steps are in the root README; [testing.md](<Source Code/docs/testing.md>) describes the regression tools. Run checks relevant to the change. Documentation-only changes need link/image checks; kernel or application changes need a build and applicable guest regression tests. Use private test disks.

请小步提交，保留现有启动流程与 GUI，使用原工具链并保留第三方许可。文档改动检查链接与图片；内核或应用改动应构建并运行相关回归，测试使用独立虚拟盘。

## Licensing and provenance / 许可与来源

Keep the upstream copyright and MIT permission text. Contributions to this fork's project code use the same MIT license; third-party components keep their own terms. When adding externally sourced code, fonts, data or tools, record the source, version and applicable license, and retain the required notices. See [fork provenance](Documentation/Licenses/Fork-Provenance.md). Studying Linux is a design reference, not permission to copy its implementation into MIT-licensed files; review the specific file's license before importing code.

本 fork 的项目代码继续采用 MIT，保留上游版权与完整许可。新增第三方组件需记录来源、版本和许可，并随分发保留必要声明。个人学习资料 `akn_learning/`、开发磁盘、编译产物及解压发行包不提交到仓库。
