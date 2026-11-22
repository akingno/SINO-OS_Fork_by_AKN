# 当前临时测试

仅登记仍在开发或调查的功能。AI 根据本次模块读取相应条目，不扫描或执行其他目录；完整规则见 [README.md](README.md)。

| 模块 / 功能 | 功能 README | 预览 / 执行命令 | 用例数 | 启用条件 | 收尾条件 |
| --- | --- | --- | ---: | --- | --- |
| mm/page — 高页分配 | [README](mm/highmem/README.md) | `node 'Source Code/tests/development/mm/highmem/test-highmem.js' '<ShellBridge 构建目录>'` | 5 | Phase 4（高页 > 1 GiB）开发 | Phase 4 完成后迁入 test-memory.js 或删除 |
