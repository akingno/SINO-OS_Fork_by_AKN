# 单核同步边界

当前只有用户态抢占，内核 C++ 调用栈不会在任意位置挂起；IRQ 仍能打断允许中断的内核代码。

| 接口 | 状态与契约 |
| --- | --- |
| IrqGuard / saveAndDisable / restore | 已实现；保存并恢复原 IF，支持嵌套，含编译器屏障；不屏蔽 NMI |
| AtomicU32 | 待实际消费者需要时实现；定义 acquire/release 语义，不使用 volatile 代替，也不引入 libatomic |
| SpinLock | 后续仅提供单核短临界区后端和重入检查，不忙等同核持有者 |
| Mutex | 后续提供所有者、等待与重试状态机；没有可分步消费者前不提供悬停内核栈的 lock() |
| futex / POSIX 用户库 | 后置；Event 和 join 不以这些接口为前置 |

IrqGuard 不可复制，作用域内不分配、不做 I/O、不阻塞或调度，不跨 user_enter 持有。
仅普通内核调用访问且不会挂起、也不被 IRQ 访问的数据，继续利用当前执行模型串行化，不无差别加锁。
恢复只改变 IF，不回放其他 EFLAGS；不能无条件 sti，也不能把此接口当作空闲休眠序列。

类型和方法放在 namespace sync，C++ 成员保持 C++ 链接；只在真实汇编/外部 ABI 边界使用 extern "C"。
不固定 mutex/spinlock 的字节数，不使用 packed 压缩原子状态，不依赖异常、RTTI、STL 或动态初始化。
