# 高页分配验证（2 GiB）

## 模块 / 功能

`mm/page.c`、`mm/paging.c` — 64 位 E820 解析、分区分配策略、高页首次分配日志。对应 plan_stage5 Phase 4。

## 启用条件

内存管理 Phase 4（高页 > 1 GiB）开发与验证期间执行。

## 命令

预览：

```powershell
node 'Source Code/tests/development/mm/highmem/test-highmem.js' --help
```

执行（从仓库根目录，需要已有 ShellBridge 构建目录）：

```powershell
node 'Source Code/tests/development/mm/highmem/test-highmem.js' 'Build Output/dev/runs/<ShellBridge 构建目录>'
```

## 用例数

5 项：

| ID | 验证内容 |
| --- | --- |
| highmem.nr-pages-reflects-2-gib | nr_pages 反映 2 GiB 物理内存 |
| highmem.direct-map-capped-at-user-base | 直接映射上限锁在 USER_BASE |
| highmem.extended-pages-published | 扩展页发布数量合理 |
| highmem.first-high-allocation-above-1-gib | 用户页首次分配在 1 GiB 以上 |
| highmem.mem-reports-2-gib-scale | Shell mem 命令报告 2 GiB 级别总量 |

## 构建 / 产物

需要 ShellBridge 镜像（`dev.ps1 -BuildOnly` 后使用同一构建目录）。报告在 `Build Output/dev/runs/<构建目录>/highmem-*/highmem-report.json`。

## 收尾条件

Phase 4 完成并合入后：将启动日志和分配行为的长期契约迁入 `test-memory.js images` 的 2 GiB 分支；删除本临时验证。
