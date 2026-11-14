# task 子系统与单核同步边界

`src32/task/` 持有进程、线程和调度实现；`task.h` 是其他内核子系统使用的接口。
用户系统调用编号、SDK 数据结构和实际 C 链接符号不因目录迁移而改变。

| 文件 | 职责 |
| --- | --- |
| task/task.h | 内核跨模块接口及必要 C ABI 声明，不暴露 Thread/Process 布局 |
| task/entry.cpp | 32 个旧入口的薄 C ABI 桥接；汇编/宿主观察所需的切出标志与统计符号 |
| task/internal.h | 仅 task 内部共享的类型、状态声明及服务接口 |
| task/process.cpp | 创建/回收、映像加载、权限与系统调用；耦合的完成记录和有限 CLI 暂留此处 |
| task/thread.cpp | Thread 初始化、等待期限判断、join 占用和结果 Object 释放 |
| task/collections.h | 不分配内存的侵入式 FIFO 与可撤销定时小根堆，内核/宿主检查共用 |
| task/wait.h/.cpp | 就绪/对象等待/回收队列、通知交接、定时到期与 Thread/Process 状态转换 |
| task/kernel_stack.h/.cpp | 每进程入口栈的大小、实际分配、保护页、栈顶和显式回收；启动自检 |
| task/schedule.h、scheduler.cpp | 有界轮转派发、用户中断处理、到期/就绪查询及空闲契约；每次至多运行一个线程 |
| work.h | RTC、网络和 ShellBridge 的有界待办查询，供主循环空闲复查 |
| sync/irq.h/.cpp | 短临界区中断状态保护及嵌套恢复自检 |

内部实现使用 namespace task / sync；必要 C ABI 由 entry.cpp 转接，不维护第二份状态。
UserFrame 保持 60 字节，用户 ESP 位于偏移 52，Frame.cs 位于偏移 44，编译时核对汇编依赖。
当前每次至多派发一个用户线程；线程数、2 tick 时间片和等待重试语义保持。

## 调度与空闲

主循环每轮推进 RTC、ShellBridge（启用时）和网络，再调用 task::step()。
一次 step 从就绪队列取队头，候选预算为本轮队列长度快照，至多进入一次 user_enter，不扫描全部线程槽或在内部无限轮转。
返回的 StepResult::Ran / NoDispatch 只记录本轮是否派发，不能决定能否休眠。

派发入口要求 IF=1、内核 CR3 且无当前线程绑定；每次候选尝试先关闭中断，再绑定进程/线程和切换用户页目录。
重试在所属用户地址空间执行；若资源被其他等待者消耗而再次返回 S_AGAIN，候选重新阻塞，继续在候选预算内取队头。
重试阻塞、错误结果及正常用户切出共用收尾：恢复内核 CR3，清空 current，将 currentThread 设为 -1，再开启 IF。
运行时校验入口、候选收尾及函数返回的 CR3/绑定/IF，防止漏掉某一分支。

派发后主循环在 IF=0 下重新查询就绪线程、到期等待、待回收进程/有限运行超时，以及后台当前待办。
有工作就开启中断并进入下一轮，让后台服务再次获得执行机会；没有工作才进入 idle_halt。
Yield(0) 仍可参与轮转，Yield(n>0) 等待其期限；IRQ 只按现有边界更新状态，不引入任意内核位置的抢占。
idle_halt 为独立 C ABI 汇编函数，调用前 IF 必须为 0，指令严格为 sti、hlt、ret；IRQ 返回后 IF=1。
关中断后的复查和相邻 sti/hlt 避免检查与休眠之间丢失可屏蔽 IRQ 唤醒。

后台查询只检查可立即推进的状态：RTC 采样到期、网络包/到期重传及关闭收尾、ShellBridge 软件 RX/可写 TX/租约或半帧超时。
UART LSR 读取有清除错误位的副作用，TX 就绪检查会保存错误标记供下一轮处理；TX 未就绪时不会因积压 RX 忙等。
保留周期 PIT 和各服务原有预算；尚未准备好的硬件及未来期限继续由 IRQ/轮询推进，不做 tickless。
就绪、到期和待回收查询分别读取 FIFO、定时堆根和回收队列，均无需遍历进程/线程槽。主动 Yield(0) 或时间片切出后进入就绪队尾。

task_schedule_stats 是宿主观察符号，记录派发、重试阻塞、公共收尾、继续推进、实际休眠及违规休眠次数，不提供用户系统调用。
启动自检覆盖空/睡眠/阻塞/期限回绕/待回收状态；现有 process 验收读取实际 idle_halt 的三个机器码并运行 probe.snx /schedule，覆盖竞争读重试、零 tick 让出、有限睡眠及空闲统计。

## 内核入口栈

每进程一个 KernelStack，供进程内所有线程在系统调用、中断或异常进入内核时使用。
当前内核不可抢占，不跨调度保留该 C++ 调用栈；此阶段不为每个线程另配入口栈。

| 范围 | 大小与用途 |
| --- | --- |
| 分配基址至基址 + 4 KiB | 未映射保护页；仍占一页物理分配 |
| 基址 + 4 KiB 至基址 + 36 KiB | 32 KiB 可用栈，向低地址增长 |
| TSS.esp0 | 基址 + 36 KiB，即可用栈的上界，首个压栈写入其下方 |

当前连续分配 9 页，每进程 36 KiB，8 个槽全占用共 288 KiB，按实际创建分配。
KernelStack 单独记录 allocationPages，释放使用实际分配描述；未来 buddy 若返回更大有序块，必须同步修改分配描述，不能按可用页数推断释放大小。
对象必须先零初始化，活动时地址稳定且不可复制/移动；KernelGuard 以 token 地址校验所有权。
分配失败保持空状态；保护失败立即回收分配；进程退出或后续创建失败时先恢复保护页，再释放实际分配。
release 可重复调用，但必须在切回主循环栈后释放，禁止在入口栈上回收自身。

启动/主循环栈仍使用固定低端 KERNEL_STACK_TOP（0x80000），与这些按进程分配的入口栈不同。
保护页能触发越界异常；当前没有独立的溢出异常栈，不能保证溢出后安全恢复。
内核构建生成 build32/*-normal.su 逐函数栈用量，供系统调用、文件与终端路径审计；它不包含完整调用链和汇编入口开销，不能单独证明最坏栈深度。

### 入口栈审计（GCC 15.2.0，i386，-Os）

普通与 ShellBridge 构建的以下逐函数报告一致，单位为字节：

| 函数 | 栈用量 |
| --- | ---: |
| irq_dispatch / task::interrupt / task::dispatchSyscall | 56 / 40 / 520 |
| task::launch_policy | 232 |
| filesys_call / vfs_canonical / vfs_path | 668 / 324 / 532 |
| terminal_call / feed | 128 / 1092 |
| storage_sync | 744 |
| net_send / tcp_emit / ip_output | 552 / 580 / 1556 |

审查了系统调用入口至 filesys_call → vfs_move / vfs_canonical / vfs_path、terminal_call → feed、storage_sync → vfs_path / 磁盘写入，以及进程创建 → launch_policy 的路径。
这些文件/终端路径没有递归遍历：vfs_path 使用有界的 128 项祖先数组，目录操作使用循环；feed 的行、提交和回显缓冲合计 1024 B，受输入长度限制。
SNX 的 64 KiB 加载缓冲和持久化的 64 KiB 文件缓冲均为模块静态存储，不在入口栈上。
较大的网络帧仍需叠加 net_send、tcp_emit、ip_output 等调用帧；回环 transmit 进入有界收包队列，不同步递归处理整个协议栈。

Ring 3 入口另有 CPU 的 20 B 保存区、向量/错误码 8 B、pushad 32 B，共 60 B；随后还有传给 irq_dispatch 的参数与 call 返回地址及 C++ 调用链。
当前中断门关闭 IF，审查的系统调用路径不主动开启中断；未来增加内核抢占、修改中断门或引入嵌套中断时必须重新预算。
报告全部为 static 或 dynamic,bounded，后者不能简单解读为无界 VLA；仍须结合生成代码与调用关系。
本次未发现需要把整文件数组搬出入口栈或移除递归的上述路径；32 KiB 留有余量，但此审计不是所有间接调用、异常和未来代码的最坏深度证明。
ShellBridge 命令执行函数另有 9528 B 报告，运行在启动/主循环栈，未纳入每进程入口栈预算。

## 加载工作区

`image_scratch[MAX_FILE]` 是 task/process.cpp 私有的 64 KiB 静态加载缓冲。
它依赖单核、内核不可抢占、装载不挂起且不递归进入来保证串行，IRQ/回调不得重入。
从读取、头部校验到全部映像复制结束前，该工作区都不能被其他加载覆盖。
未来若加入可挂起或异步加载，应先改成每次加载独占的资源并处理失败回收；不能用长时间关中断或自旋锁包住整个装载。

## 就绪、等待、定时与回收队列

Thread 状态区分 UNUSED、READY、RUNNING、SLEEPING、BLOCKED、EXITED。EXITED 是已释放用户栈、尚待 join/detach 的记录，不在就绪队列，也不计入活动线程。schedLink 含前驱、后继和实际队列 owner 指针；一个节点只能属于就绪或某个对象等待 FIFO，运行中不在这两类链上。取消可直接 O(1) 摘除，不搜索前驱。
Thread 内嵌 TimerNode，slot 为 heap index + 1，零表示未登记；定时节点独立于 schedLink，因此有期限的对象阻塞/待重试就绪线程可以同时保留定时条目。
定时堆插入/撤销为 O(log N)，统一处理 Yield 睡眠、等待超时及有限运行期限；保留有符号 tick 比较、零超时无限等待和到期优先语义。相同期限按登记次序处理。当前堆配额为 32 个线程条目加 8 个进程条目。
Process 单独内嵌回收链和运行期限节点；retire 是唯一进入待回收状态的路径，先撤销线程的就绪/等待/定时资格，再入回收队列。回收在主循环栈执行，关闭句柄/释放内存时不持有 IrqGuard。

等待队列头尾存于按 Object 索引与 WaitReason 直接定位的元数据表，键包含 Object generation。Object 创建时重置对应代数；销毁前广播并摘空其等待队列，已通知的旧代数记录不影响复用后的新对象。
等待记录不增加 Object 引用，避免额外保持管道端点存活而破坏 EOF。管道内保存双方对象索引/代数，查找对端无需扫描对象表。
每个目标队列记录已通知但尚未完成的 inFlight 数量；有限通知存在未完成机会时不重复取下一等待者。通知只摘等待节点并进入就绪 FIFO，保留参数、原期限和定时条目，不替用户搬运数据。
重试在正确地址空间重新校验句柄和用户缓冲；S_AGAIN 撤销通知资格后回到等待队尾，沿用原期限。正数部分 I/O、错误和完成结果各提交一次。
完成、超时、关闭或取消会统一撤销节点、定时条目和通知资格；释放一个通知机会后，若仍有数据/空间则交接下一等待者，保证没有新外部事件时仍能推进。
少量管道数据/空间和终端字节采用 FIFO 有限通知；进程完成、交互结果、EOF、控制器断开、前台变更和对象销毁按语义广播。

队列和堆的关键更新使用 IrqGuard，仅包含登记、撤销和状态修改，不分配、不等待硬件、不切入用户态。普通非抢占内核路径仍负责序列化实际 I/O；未来将 I/O 状态更新移入 IRQ 时必须一起满足条件检查与登记的同步契约。
生产 FIFO/堆由独立宿主检查使用 96 个合成节点验证；运行时容量仍是 8×4。所有入队对象地址必须稳定，不可复制/移动，清零或复用槽位前先撤销所有成员资格。

### Event 阻塞等待

WaitReason::EVENT 复用同一对象代数、通知后重试、到期优先及撤销协议，不另建等待器或轮询循环。
SYS_EVENT_WAIT_BLOCK=0x151 显式阻塞；旧 SYS_EVENT_WAIT=0x115 继续只查询。Set(true) 保持置位并广播，Set(false) 只清除状态；通知不预先认定成功，Reset 先于重试时保留原期限重新挂起。
SYS_CLOSE 通过保存现场的 EBX 匹配精确等待句柄，因此通知后关闭也返回 S_BADHANDLE，关闭其他重复句柄不误取消。取消、故障和退出沿用统一撤销路径。
SDK 使用 sino::event::{create,set,query,wait,available}，FEATURE_EVENT_WAIT 独立标识该能力；严格策略只授权当前进程持有的本地事件句柄。

### 新线程、退出记录与 join

新 create 接收 worker、argument、SDK trampoline 和 flags，校验并准备完整的 T-12 cdecl 栈帧后才发布 TID/进入就绪队列。SDK trampoline 是独立 C ABI 入口，调用 int worker(void*)，把返回值提交给 THREAD_EXIT_CODE；意外返回时调用进程 Exit 错误路径，不从伪返回地址 ret。
TID 使用不回绕的全局 28 位代数及 3 位 slot+1，只在当前进程查找；旧线程创建仍返回原始槽号，不能作为新 join token。主槽保留，三个 worker 槽含未领取退出记录，不扩大运行容量。

joinable 线程持有一个私有 THREAD Object，其 value 指向稳定 Thread；用户不能通过新 API 把这个 Object 当成句柄授权给其他进程。joinObject 的执行/记录引用一直保留到成功领取、detach 或进程回收，不会因为等待登记而增加引用。线程退出立即释放四页用户栈，保留 EXITED/exitCode/TID；detached/legacy 退出直接复用槽位。
只有一个阻塞 joiner 可以占用目标，目标保存 joiner，调用线程保存 joinTarget。该反向联系在统一 clearWait 中撤销，覆盖超时、无效输出、取消和退出；通知仍只允许在原地址空间重试。结果先校验并写入用户输出，成功后再清除占用、释放 Object 并使槽位可用。非阻塞 query 是 try-join，成功也消费结果。
detach 在目标被其他 joiner 占用时返回 S_BUSY；活目标 detach 丢弃未来结果义务，已退出目标 detach 释放记录。退出记录不算活线程，最后一个活动线程退出即结束进程；主循环回收先取消全部节点/占用，再释放所有结果 Object 和进程资源。
旧 ThreadExit 忽略参数并使用零退出码；新 ThreadExitCode 保留 signed int 结果。只检测自身 join，不额外检测用户构造的相互 join 环；超时范围和到期优先契约保持，包括 wait.cpp 的超时语义重审 TODO。

## 同步接口

IrqGuard 保存原 IF、关闭可屏蔽中断，在析构时恢复原 IF，并提供编译器屏障；不回放其他 EFLAGS，也不屏蔽 NMI。
作用域内不得分配、做 I/O、阻塞、调度或进入用户态。普通不可抢占且不与 IRQ 共享的内核数据继续按执行模型串行化。
启动在设备初始化完成、IF 开启后自检嵌套保护：外层关闭、内层保持关闭、退出外层恢复开启。

AtomicU32、单核 SpinLock 后端、Mutex 状态机及其使用上下文见 [sync/README.md](../src32/sync/README.md)。
尚未提供可在任意内核 C++ 调用栈上睡眠的 mutex、futex 或 POSIX 用户库。

## 验证

按 [模块指南](../tests/TESTING.md) 选测：提取的进程/权限入口使用 process / policy；Thread 等待成员迁移使用 shell-waits 代表实际阻塞调用；普通镜像另做启动检查。
IRQ 自检并入原有启动/进程用例，不新增独立固定 ID。不因仅移动共享实现就运行全部等待专项。
入口栈变更选择 process、内存 images 及普通启动。启动自检验证精确页数、保护/可用 PTE 在内核及用户页目录中的权限、首尾读写、重复操作、保护记录耗尽回滚及重试；不以真实触发内核栈溢出来测试恢复。
队列共同迁移选择 process 8、completion-waits 8、pipe-waits 10、terminal-waits 11、shell-waits 5，加普通启动 1、集合宿主 4，共 47 项。新增 4 个独立宿主用例，不改变统一注册表 ID；原有启动/抢占用例追加重复通知、旧对象代数、到期优先、竞争重试和剩余字节交接断言。
Event 接入选择独立 test-event-waits.js 8、process 8、policy 2、普通启动 1，共 19 项；Event 专项使用同次 ShellBridge 构建、共享 VM/内存观察工具和私有盘，固定行为 ID 见 tests/support/event-cases.json，不自动加入 core。
线程参数/join 接入选择独立 test-thread-waits.js 8、process 8、policy 2、images 11、Event 8、completion-waits 8、普通启动 1，共 46 个选定用例。探针读取初始栈帧、验证 v1/v2 返回和负数退出码、立即栈回收、一次领取、超时/解除映射后的再领取、单消费者/自身/外进程拒绝、detach/复用、参数/配额回滚和取消/故障/最后线程清理。需要创建严格子进程的两项使用兼容父进程，不扩大严格委托权限。
