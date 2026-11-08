# 开机文字 Shell

普通镜像使用 BIOS VGA mode 3（80×25），内核启动唯一受信任的 `conhost.snx`。
conhost 创建文本会话、取得 `/home/user` 工作区授权并启动 PROCESS_STRICT、caps=0 的 `gsh.snx`。
命令继续通过现有交互协议启动 gcmd、tview、writer，完成后恢复 Shell 前台。
退出或 Ctrl+D 结束 Shell 时，conhost 关闭旧会话、清理观察句柄，再启动新会话。

SYS_CONSOLE 只允许内核专门启动的 conhost 使用。程序路径相同、从可写文件复制或严格启动均不能获得屏幕/键盘权限。
conhost 只能读取解码按键和提交固定尺寸文字帧；用户页校验和进程隔离继续有效。
ShellBridge 测试镜像不自动启动 conhost/gsh，保持原有进程槽、句柄和终端资源基线。

输入为 US ASCII 键盘，无输入法切换、拼音组合或候选框。Ctrl+Space 不触发中文输入。
方向键、历史、Tab、退格、Ctrl+C 取消、Ctrl+D EOF 使用现有 Shell/终端机制。
文字屏幕仅支持 ASCII，其他 Unicode 字符显示为问号；文件内容和 UTF-8 文件名不会被改写。
输入背压时保留当前按键，暂停继续取键；不存在无限输入队列。

可写 Shell 的写入、创建、改名和删除命令完成后，Shell 使用自己持有的 COMMIT 权限提交用户快照，成功落盘后才返回下一次提示符；不扩大写工具的权限。正常操作无需手动 sync。保存失败会显示 save failed，并令命令状态失败；RAM 中的改动可用 sync 重试。进程中途被取消、强制关闭或磁盘写入失败的未完成操作不作成功保存承诺。

## 验证

先 `dev.ps1 -BuildOnly`，然后：

```powershell
node 'Source Code/tests/host/test-console.js' --list
node 'Source Code/tests/host/test-console.js' 'Build Output/dev/runs/<普通构建编号>'
```

追加 `--persistence`（预览为 `--list --persistence`）运行 3 项：console.persist-create、console.persist-update、console.persist-delete。专项不发送 sync，在创建/追加/改名/删除后直接停止 QEMU，再使用同一私有盘启动，核对字节、路径及删除结果。

9 项固定 ID：console.autostart、console.edit-history、console.workspace、console.cancel、console.ascii-input、console.restart、console.scrollback、console.scroll-input、console.scroll-output。
专项使用独立无窗口 VM 和空白私有盘，通过 QMP 发送 PS/2 按键、读取 0xb8000 的文字字节，不调用桌面、窗口、截图或鼠标接口。
报告与日志在构建目录 console-* 下。临时开发覆盖已提升为该稳定入口，不计入统一注册表的 267 项。

## 键盘滚屏与单色显示

Shift+PageUp / Shift+PageDown 每次移动 23 行（历史区域为 24 行，保留一行重叠），最多保留 128 行。
普通 PageUp/PageDown 继续传给前台程序。浏览时新输出保持原视口；最早历史被覆盖时夹紧到仍保留的内容。
输入行平常保持自然位置，不足 25 行时不强制置底；浏览历史时将当前输入光标所在的那一行固定到第 25 行，光标继续可见。
长输入的其他折行保留在历史中。普通输入或编辑键先返回当前输入位置，再交给 Shell；Ctrl+C / Ctrl+D 的原有行为不变。
清屏和会话重启清空历史与滚动位置。conhost 管理历史和输入视图，内核只负责提交字符帧和固定灰白字黑底属性；panic 同样单色。
旧 GUI 事件队列、场景绘图 SDK、Forms 声明和 GUI 专用语言资源已删除，相关系统调用编号继续保留并返回错误。
