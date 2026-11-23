# 开机文字 Shell

普通镜像使用 BIOS VGA mode 3（80×25），内核启动唯一受信任的 `conhost.snx`。
conhost 创建文本会话、取得 `/home/user` 工作区授权并启动 PROCESS_STRICT、caps=0 的 `gsh.snx`。
命令继续通过现有交互协议启动 gcmd、tview、writer，完成后恢复 Shell 前台。
退出或 Ctrl+D 结束 Shell 时，conhost 关闭旧会话、清理观察句柄，再启动新会话。

SYS_CONSOLE 只允许内核专门启动的 conhost 使用。程序路径相同、从可写文件复制或严格启动均不能获得屏幕/键盘权限。
conhost 可读取解码按键、提交固定尺寸文字帧，并阻塞等候终端工作。用户页校验与进程隔离继续有效。
ShellBridge 测试镜像不自动启动 conhost/gsh，保持原有进程槽、句柄和终端资源基线。

键盘使用 US 布局，提供 ASCII 输入。
方向键、历史、Tab、退格、Ctrl+C 取消、Ctrl+D EOF 使用现有 Shell/终端机制。
文字屏幕仅支持 ASCII，其他 Unicode 字符显示为问号；文件内容和 UTF-8 文件名不会被改写。
输入背压时保留当前按键，暂停继续取键；不存在无限输入队列。

`SYS_CONSOLE` 的操作 2 接收 24 字节请求，六个 `u32` 依次为控制句柄、兴趣掩码、
根 Shell 和前台命令的完成句柄、待投递的输入类型和值。位 0 至 5 分别等待键盘、
待投递输入可重试、输出或编辑帧、Shell 请求、根 Shell 完成和前台命令完成。
其余位及未用字段填零。根 Shell 句柄必填，前台命令句柄仅在选择位 5 时填写。
位 0 和位 1 互斥，位 1 使用 `TERM_TEXT`、`TERM_KEY`、`TERM_EOF` 或 `TERM_CANCEL`。

就绪返回 `S_OK`，调用方继续使用原接口消费。非法参数、无效句柄、权限拒绝、
会话关闭和已有等待者分别返回 `S_INVALID`、`S_BADHANDLE`、`S_DENIED`、
`S_CLOSED`、`S_BUSY`。每个会话允许一个代理等待，PS/2 键盘由一个代理订阅。
内核在同一关中断边界检查条件和入队，重试使用入队时的请求快照。
`gsh` 发布编辑帧后阻塞等候 ACK，`conhost` 按输出、编辑帧、显示确认的顺序处理。

可写 Shell 的写入、创建、改名和删除命令完成后，Shell 使用自己持有的 COMMIT 权限提交用户快照，成功落盘后才返回下一次提示符；不扩大写工具的权限。正常操作无需手动 sync。保存失败会显示 save failed，并令命令状态失败；RAM 中的改动可用 sync 重试。进程中途被取消、强制关闭或磁盘写入失败的未完成操作不作成功保存承诺。

## 验证

先 `dev.ps1 -BuildOnly`，然后：

```powershell
node 'Source Code/tests/host/test-console.js' --list
node 'Source Code/tests/host/test-console.js' 'Build Output/dev/runs/<普通构建编号>'
```

追加 `--persistence`（预览为 `--list --persistence`）运行 3 项：console.persist-create、console.persist-update、console.persist-delete。专项不发送 sync，在创建/追加/改名/删除后直接停止 QEMU，再使用同一私有盘启动，核对字节、路径及删除结果。

9 项固定 ID：console.autostart、console.edit-history、console.workspace、console.cancel、console.ascii-input、console.restart、console.scrollback、console.scroll-input、console.scroll-output。
专项使用独立无窗口 VM 和空白私有盘，通过 QMP 发送 PS/2 按键、读取 0xb8000 的文字字节。
报告与日志在构建目录 console-* 下。该专项独立于统一注册表的 267 项。

## 键盘滚屏与单色显示

Shift+PageUp / Shift+PageDown 每次移动 23 行（历史区域为 24 行，保留一行重叠），最多保留 128 行。
普通 PageUp/PageDown 继续传给前台程序。浏览时新输出保持原视口；最早历史被覆盖时夹紧到仍保留的内容。
输入行平常保持自然位置，不足 25 行时不强制置底；浏览历史时将当前输入光标所在的那一行固定到第 25 行，光标继续可见。
长输入的其他折行保留在历史中。普通输入或编辑键先返回当前输入位置，再交给 Shell；Ctrl+C / Ctrl+D 的原有行为不变。
清屏和会话重启清空历史与滚动位置。conhost 管理历史和输入视图，内核只负责提交字符帧和固定灰白字黑底属性；panic 同样单色。
