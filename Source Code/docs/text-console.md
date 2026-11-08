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

6 项固定 ID：console.autostart、console.edit-history、console.workspace、console.cancel、console.ascii-input、console.restart。
专项使用独立无窗口 VM 和空白私有盘，通过 QMP 发送 PS/2 按键、读取 0xb8000 的文字字节，不调用桌面、窗口、截图或鼠标接口。
报告与日志在构建目录 console-* 下。临时开发覆盖已提升为该稳定入口，不计入统一注册表的 270 项。
