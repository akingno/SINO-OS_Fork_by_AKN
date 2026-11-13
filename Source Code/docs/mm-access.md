# 内存范围、保护页与用户缓冲区契约

## 物理范围与地址对齐

E820 可用记录只发布完全位于区间内的页（起点向上、终点向下取整）；非可用记录排除任何被触及的页（起点向下、终点向上）。
重叠记录保留优先，与条目顺序无关。e820.h 统一范围转换与页可用性判断，metadata 和后续页发布使用同一规则。
长度在相加前截断到 4 GiB 上界；4 GiB 排他结束端点使用宽整数或 PFN 表示，不存入 32 位字节地址。

`page_ceil_checked(value, &result)` 返回 1 表示成功；无法表示对齐结果或输出指针为空时返回 0，不修改 result。
零长度可对齐为零；AnonMap 仍独立拒绝零长度。物理内存区域端点转换不使用这个 u32 字节接口。

## 物理页初始化顺序

page_init 只组织启动阶段的初始化：read_boot_memory_map 解析 E820 并确定最高可用 PFN，
据此计算 metadata 大小和启动池末端，再由 init_page_metadata 在启动映射内寻找并保留 metadata，
publish_boot_pages 发布初始可用页，最后记录诊断信息。find_boot_metadata 只搜索完整可用页，禁止借助尚未可用的分配器放置自身 metadata。
扩展物理页仍由 page_publish_extended 在正式页表建立后单独发布。
分页表构造辅助函数名为 init_identity_table；活动页目录查询沿用 paging_is_active，供分页和 uaccess 共用。

## 连续物理页搜索

alloc_from 以 next-fit 顺序搜索候选起点：先搜索 [hint, end)，再搜索 [start, hint)。
hint 只划分候选起点，连续空闲块允许跨 hint；块的终点始终不能超过 end，物理池末端不能与起点拼接。
无效 hint 归一到 start。失败不修改页状态、空闲计数或调用方 hint；成功后扣减页数，并将 hint 更新到分配块末端。
next-fit 最坏仍为 O(N) 页状态扫描；是否引入索引应由实际扫描开销决定。

## 内核保护页

`paging_kernel_guard(page, &owner)` / `paging_kernel_unguard(&owner)` 仅在正式页表建立后使用。
page 必须是调用方持有的、直接映射范围内的活动 kpage 分配；不能传用户后备页、保留页或已释放页。
调用方负责选择安全的页面，不能保护正在执行的栈、内核代码、页目录或页表等仍需访问的结构。
owner 是初始化为零的 KernelGuard，必须存放在被保护页之外，并在恢复前保持地址及生命周期稳定。

guard 保存完整原始 PTE，并清除映射；unguard 精确恢复原 PTE，分别执行 invlpg。共享内核 PDE 结构保持冻结。
记录绑定 owner 地址和不复用的序号；复制 owner 不转移权利。重复操作、错误 owner、越界或不对齐页面返回 S_INVALID；
16 个记录用完或序号耗尽返回 S_LIMIT；恢复时映射已被其他代码改变返回 S_CHANGED，不覆盖新映射。
失败不改变原状态。不得手动修改活动 owner；内核单核且这些操作不可抢占。

分配器区分用户物理页、普通内核页和受保护内核页。保护期间两类释放入口均拒绝回收该页；连续分配中有受保护页时整次释放失败。
恢复后才能释放或复用。进程内核栈用独立 owner 保存生命周期，分配失败清理和正常退出都按“恢复、释放”的顺序执行。
这不是一般页权限修改 API；只读页恢复后仍只读，不会升级为可写。

## 用户复制接口

- copy_from_user(pd, kernel_destination, user_address, bytes)
- copy_to_user(pd, user_address, kernel_source, bytes)
- copy_string_from_user(pd, kernel_destination, user_address, capacity)
- uaccess_prepare(pd, user_address, bytes, USER_READ / USER_WRITE)

所有接口要求 pd 属于当前进程且对应当前活动 CR3；不能用其他进程或非活动页目录校验后再解引用。
process_user_limit 检查当前所有者及虚拟范围，paging_is_active 检查硬件页目录。范围与权限校验不能相互代替。
内核侧缓冲区由调用方保证有效、容量充分且不与用户区混叠。READ 要求 PRESENT|USER，WRITE 还要求 WRITE；未知访问模式拒绝。

prepare 先验证整个范围，不因后半段非法而提前落地前半段；确认预留合法后再分配缺失匿名页。
若落页中途 OOM，已分配零页仍属于该进程，保留在原匿名区，计入页数；随后可访问、整区 unmap 或退出统一回收。
这类部分驻留不承诺回滚，但准备失败前不进行数据复制，也不能消费管道字节或修改文件。

固定长度复制完整 prepare 成功后才执行 memcpy，准备失败不更新目标缓冲区（允许上述匿名页驻留变化）。
字符串复制逐页检查，在 NUL 处立即结束；NUL 后下一页无需映射。字符串复制失败可留下已复制前缀；
调用方必须仅在返回 S_OK 后使用整个字符串，不承诺失败时 NUL 结尾。capacity 为零或用尽而无 NUL 返回 S_INVALID。
页面循环以最后一个页地址为终止条件，不依赖越过最后页后加法不溢出。

零字节范围不触页、不分配，但仍检查地址归属、边界、活动 CR3 和访问模式，不能用零长度绕过接口约束。

## 调用路径审计

| 路径 | 使用方式与顺序 |
| --- | --- |
| filesys.c 请求结构、文件路径 | copy_from_user / copy_string_from_user 到内核局部变量，成功后才执行对象操作 |
| SYS_SYSTEM_INFO / SYS_START_INFO | 构造内核局部结果，用 copy_to_user 完整校验后输出 |
| 文件范围读写、目录枚举、暂存提交 | 先 prepare 完整用户输出/输入范围；随后在当前系统调用内借用指针；有副作用的 finish 前已准备输出 |
| 管道和 CLI 流 | 先 prepare 后传输；保留已有部分传输语义；阻塞重试重新校验缓冲区 |
| terminal.c 会话、编辑、交互协议 | 所有读写缓冲区先 prepare；等待重试由 retry_wait 在切换到所属 pd 后重新校验 |
| 网络、旧式文件/进程查询、日志、控制台帧 | 先 prepare，使用期间保持所属地址空间活动，不保存裸指针跨调度使用 |

直接借用保留为有明确准备阶段的接口，不把所有流 I/O 改成额外拷贝。任何未来内核抢占、共享页 COW 或部分 unmap 机制都需重新审查指针有效期。

## 验证入口

`node 'Source Code/tests/host/test-mm-boundaries.js' --list` 预览，去掉 --list 执行 10 项宿主边界检查。
该专项使用真实 E820 范围转换与 uaccess 实现，仅替代硬件页表查询、所属进程范围和匿名落页接口；不把宿主模拟当作客户机回收验证。
报告在 Build Output/dev/runs/mm-boundaries-*。该独立专项不自动加入 core。

普通启动自检检查 guard 缺页、恢复、只读 PTE 保留、重复操作、错误 owner、释放拒绝和资源计数；
分配器自检在隔离的 8 页窗口中检查跨 hint 连续分配、池边界碎片拒绝、过大申请、hint 回绕及释放后计数恢复。
test-memory.js images 验证映像及栈生命周期，anonymous 验证真实跨页 I/O、OOM 不消费管道数据和进程回收。
文件和终端等待路径按 TESTING.md 的追加条件选测，不默认全量回归。

默认 VM 为 4096 MiB、单核，报告通过 QMP 核对配置；images 11 项、anonymous 7 项。
images 从客户机读取原始 E820，按保留优先规则独立核对 4 GiB 以下发布页数与 1 GiB 直接映射边界。
安装 RAM 与实际可管理页数不同，4 GiB 以上的重映射 RAM 在非 PAE 模式下不纳入物理池。

## 测试构建的分配失败注入

mm/test_fault.cpp 的后端仅在 SINO_SHELL_BRIDGE 构建中存在，不新增系统调用或普通用户控制入口。
启动路径核对 PROCESS_STRICT、只读 /boot 节点身份及精确参数后绑定目标页目录；可写副本不能启用。
映像模式在部分数据页分配后失败一次；页表模式让真实数据页分配成功后使新页表分配失败，检查数据页回滚。
匿名模式允许首次零页落地，再拒绝该页目录后续匿名页分配，以验证跨页复制失败不消费管道数据、首次触页故障被隔离。
注入按页目录匹配，不限制其他进程或全局分配器；页目录销毁前清理记录，启动失败和正常/故障退出均走同一清理路径。
这些是指定分配点返回失败的回归检查，不等价于整机物理内存耗尽压力测试。
