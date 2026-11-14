#include "../klog.h"
/* Preemption only at user boundaries. Kernel remains non-preemptible. */
#include "internal.h"
#include "../mm/paging.h"
#include "../mm/kmap.h"
#include "../mm/uaccess.h"
#include "../object.h"
#include "../vfs.h"
#include "../storage.h"
#include "../net.h"
#include "../rtc.h"
#include "../filesys.h"
#include "../pipe.h"
#include "../terminal.h"
#include "../locale.h"
#include "../console.h"
#include "../lib/kstring.h"
#include "../mm/page.h"
#include "../mm/test_fault.h"
#include "../panic.h"

namespace task {
  /* 【简化实现：调度容量】进程和线程使用固定数组，当前上限为 8 x 4。
  * 替换方向：将执行实体的存储与就绪/等待队列分开，再按资源预算动态分配；
  * 不应只放大常量，因为扫描、句柄预算及相关 ABI/测试都依赖现有限制。 */
  typedef struct {
    CliInfo cli;
    char path[128];
    u8 *buffer[3];
    u32 input_bytes, input_position, length[2], deadline, timed, incomplete;
    u32 pending_directory, directory_rights;
    u32 pending_pipe[3]; /* Object index + 1, owned until transferred to child. */
  } RunData;
  static_assert(sizeof(RunData) <= PAGE_SIZE, "run metadata fits one page");
  static u32 run_count;
  static void run_free(RunData* run) {
    if (!run) {
      return;
    }
    if (run->pending_directory) {
      object_unref((int)run->pending_directory - 1);
    }
    for (int i = 0; i < 3; i++) {
      if (run->pending_pipe[i]) {
        object_unref((int)run->pending_pipe[i] - 1);
      }
    }
    for (int i = 0; i < 3; i++) {
      if (run->buffer[i]) {
        kpage_free(run->buffer[i]);
      }
    }
    kpage_free(run);
    run_count--;
  }
  static struct {
    int used, complete;
    ProcessCompletion result;
    RunData* run;
  } completions[PROCESS_COMPLETIONS];
  void completionRelease(int index) {
    if (index >= 0 && index < PROCESS_COMPLETIONS) {
      run_free(completions[index].run);
      memset(&completions[index], 0, sizeof(completions[index]));
    }
  }
  void streamRelease(int value) {
    object_unref(value / 3);
  }
  Process processes[MaxProcesses];
  Process* current;
  int currentThread = -1;
  static u32 next_pid = 1;
  static struct {
    u32 pid;
    char name[64];
  } leases[8];
  static u32 gdt[12], tss[26];
  static void descriptor(int i, u32 base, u32 limit, u32 access, u32 flags) {
    gdt[i * 2] = (limit & 65535) | (base << 16);
    gdt[i * 2 + 1] = (base & 0xff000000) | ((base >> 16) & 255) | (access << 8) |
    (limit & 0xf0000) | (flags << 20);
  }
  void init(void) {
    initScheduler();
    KernelStack::selfTest();
    memset(gdt, 0, sizeof(gdt));
    descriptor(1, 0, 0xfffff, 0x9a, 12);
    descriptor(2, 0, 0xfffff, 0x92, 12);
    descriptor(3, 0, 0xfffff, 0xfa, 12);
    descriptor(4, 0, 0xfffff, 0xf2, 12);
    tss[2] = 16;
    tss[25] = sizeof(tss) << 16;
    descriptor(5, (u32)tss, sizeof(tss) - 1, 0x89, 0);
    struct PACKED {
      u16 limit;
      u32 base;
    } p = {sizeof(gdt) - 1, (u32)gdt};
    __asm__ volatile("lgdt %0; mov $0x28,%%ax; ltr %%ax" ::"m"(p)
      : "eax", "memory");
    object_tests();
    pr_debug("USER GDT/TSS ready, CPL3 selectors 1B/23, I/O denied\n");
  }
  static void release(Process* p) {
    detachProcess(*p);
    for (int i = 0; i < 8; i++) {
      if (leases[i].pid == p->pid) {
        leases[i].pid = 0;
      }
    }
    handles_close(p->handles);
    if (p->interactive) {
      kpage_free(p->interactive);
    }
    if (p->pd) {
#ifdef SINO_SHELL_BRIDGE
      mmtest::clear(p->pd);
#endif
      paging_user_destroy(p->pd);
    }
    p->kernelStack.release();
    /* Publish only after execution resources and owned handles are gone. */
    if (p->completion_object) {
      int object = (int)p->completion_object - 1;
      int slot = objects[object].value;
      completions[slot].result = (ProcessCompletion){sizeof(ProcessCompletion), 1,
        p->pid, p->reason, p->reason == PROCESS_EXITED ? p->code : 0,
        p->fault_vector, {0, 0}};
      completions[slot].complete = 1;
      if (completions[slot].run && p->reason != PROCESS_EXITED) {
        completions[slot].run->incomplete = 1;
      }
      notify(p->completion_object, WaitReason::COMPLETION, true);
      object_unref(object);
    }
    // 仅在所有资源释放后清零；不能复制或移动带活动 KernelGuard 的槽位。
    memset((void*)p, 0, sizeof(*p));
  }
  static phys_t alloc_user_page(Process* p, u32 va, int write) {
#ifdef SINO_SHELL_BRIDGE
    if (mmtest::fail(p->pd, mmtest::Stage::Image)) {
      return PHYS_NULL;
    }
#endif
    phys_t page = page_alloc();
    if (page && !paging_user_map(p->pd, va, page, write)) {
      page_free(page);
      return PHYS_NULL;
    }
    return page;
  }
  static void free_user_pages(Process* p, u32 start, u32 end) {
    for (u32 va = start; va < end; va += PAGE_SIZE) {
      phys_t page = paging_user_unmap(p->pd, va);
      if (page) {
        page_free(page);
      }
    }
  }
  /* 【简化实现：虚拟区间】用 32 项数组保存匿名预留区，线性避开已映射页和
  * 尚未落地的预留区，总量限制 16 MiB；物理页由缺页路径按需补齐。
  * 替换方向：统一管理映像、栈和匿名区的虚拟区间，记录权限与后备类型，
  * 支持查找、合并和拆分；保留地址预留与物理页分配的分离。 */
  static int anon_map(Process* p, u32 bytes) {
    if (!bytes || !page_ceil_checked(bytes, &bytes)) {
      return S_INVALID;
    }
    AnonRegion* slot = 0;
    u32 used = 0;
    for (u32 i = 0; i < ANON_MAX_REGIONS; i++) {
      AnonRegion* region = &p->anon[i];
      used += region->bytes;
      if (!region->base && !slot) {
        slot = region;
      }
    }
    if (!slot || bytes > ANON_MAX_BYTES - used) {
      return S_LIMIT;
    }
    /* Reservations occupy virtual space even before there is a PTE. */
    u32 start = USER_DYNAMIC_BASE;
    for (;;) {
      u32 base = paging_user_find_gap(p->pd, start, USER_DYNAMIC_END, bytes);
      if (!base) {
        return S_LIMIT;
      }
      u32 next = base;
      for (u32 i = 0; i < ANON_MAX_REGIONS; i++) {
        const AnonRegion* region = &p->anon[i];
        u32 end = region->base + region->bytes;
        if (region->base < base + bytes && end > next) {
          next = end;
        }
      }
      if (next == base) {
        *slot = (AnonRegion){base, bytes};
        return (int)base;
      }
      start = next;
    }
  }
  /* 【简化实现：解除映射】只接受区域起点并释放整区，不能解除中间或部分范围。
  * 替换方向：在统一虚拟区间管理中拆分/缩短区域，再回收实际存在的页；
  * 同时处理等待中的用户缓冲区失效及页表、TLB 的生命周期。 */
  static int anon_unmap(Process* p, u32 base) {
    if (base < USER_DYNAMIC_BASE || base >= USER_DYNAMIC_END || !page_aligned(base)) {
      return S_INVALID;
    }
    for (u32 i = 0; i < ANON_MAX_REGIONS; i++) {
      AnonRegion* region = &p->anon[i];
      if (region->base == base) {
        free_user_pages(p, base, base + region->bytes);
        *region = (AnonRegion){0, 0};
        return S_OK;
      }
    }
    return S_INVALID;
  }

  int anonPage(u32* pd, u32 page, int populate) {
    if (!current || current->pd != pd || !page_aligned(page) ||
      page < USER_DYNAMIC_BASE || page >= USER_DYNAMIC_END) {
      return S_INVALID;
    }
    for (u32 i = 0; i < ANON_MAX_REGIONS; i++) {
      const AnonRegion* region = &current->anon[i];
      if (page >= region->base && page - region->base < region->bytes) {
        if (!populate) {
          return S_OK;
        }
        if (paging_user_flags(pd, page) & PAGE_PRESENT) {
          return S_INVALID;
        }
#ifdef SINO_SHELL_BRIDGE
        if (mmtest::fail(current->pd, mmtest::Stage::Anonymous)) {
          return S_NOMEM;
        }
#endif
        return alloc_user_page(current, page, 1) ? S_OK : S_NOMEM;
      }
    }
    return S_INVALID;
  }

  u32 userLimit(const u32* pd, u32 address) {
    if (!current || current->pd != pd || address < USER_BASE) {
      return 0;
    }
    if (current->abi == 2) {
      if (address < USER_DYNAMIC_END) {
        return USER_DYNAMIC_END;
      }
      return address < USER_V2_END ? USER_V2_END : 0;
    }
    if (address <= USER_V1_END) {
      return USER_V1_END;
    }
    return address >= USER_DYNAMIC_BASE && address < USER_DYNAMIC_END ? USER_DYNAMIC_END : 0;
  }

  static u32 user_stack_top(const Process* p, int thread) {
    return p->abi == 2 ? USER_V2_STACK(thread) : USER_V1_STACK(thread);
  }
  static int alloc_user_stack(Process* p, int thread) {
    u32 top = user_stack_top(p, thread), start = top - UserStackBytes;
    for (u32 va = start; va < top; va += PAGE_SIZE) {
      if (!alloc_user_page(p, va, 1)) {
        free_user_pages(p, start, va);
        return 0;
      }
    }
    return 1;
  }
#ifdef SINO_SHELL_BRIDGE
  /* Test-only mappings for the strict paging probe on the read-only boot disk. */
  static int paging_probe_setup(Process* p, int exhaust) {
    u32 cross = USER_DYNAMIC_BASE + TABLE_BYTES - PAGE_SIZE;
    u32 high = USER_DYNAMIC_END - PAGE_SIZE;
    if (paging_user_find_gap(p->pd, cross, cross + 2 * PAGE_SIZE, 2 * PAGE_SIZE) != cross ||
      !alloc_user_page(p, cross, 1) || !alloc_user_page(p, cross + PAGE_SIZE, 1) ||
      paging_user_find_gap(p->pd, cross, cross + 3 * PAGE_SIZE, PAGE_SIZE) != cross + 2 * PAGE_SIZE) {
      return S_INVALID;
    }

    u32 before = free_pages;
    if (alloc_user_page(p, cross, 0) || free_pages != before) {
      return S_INVALID;
    }
    if (!alloc_user_page(p, high, 0)) {
      return S_NOMEM;
    }
    free_user_pages(p, high, high + PAGE_SIZE);
    if (free_pages != before) {
      return S_INVALID;
    }
    if (!exhaust) {
      return alloc_user_page(p, high, 0) ? S_OK : S_NOMEM;
    }

    // 数据页实际分配成功，仅使此页目录的下一次页表分配失败，检查回滚。
    mmtest::arm(p->pd, mmtest::Stage::Table, 0);
    int failed_cleanly = !alloc_user_page(p, high, 0) &&
    !paging_user_flags(p->pd, high);
    if (!failed_cleanly || free_pages != before) {
      return S_INVALID;
    }
    pr_debug("PAGING probe table OOM rollback passed\n");
    return S_NOMEM;
  }
#endif
  void pipeWake(const Object* endpoint) {
    const u32 object = (u32)(endpoint - objects) + 1;
    const WaitReason reason = endpoint->type == ObjType::PIPE_WRITE
      ? WaitReason::PIPE_READ : WaitReason::PIPE_WRITE;
    if (!endpoint->refs) {
      notify(object, reason, true);
      return;
    }
    u32 peer = pipe_peer(endpoint);
    if (peer) {
      const u32 flags = (u32)pipe_ready(&objects[peer - 1]);
      if (flags & (STREAM_READABLE | STREAM_WRITABLE)) {
        notify(object, reason);
      }
    }
  }
  void terminalWake(u32 object) {
    u32 flags = terminal::readState(object);
    if (flags & (STREAM_HANGUP | STREAM_EOF)) {
      notify(object, WaitReason::TERM_READ, true);
    } else if (flags & STREAM_READABLE) {
      notify(object, WaitReason::TERM_READ);
    }
  }
  void interactiveWake(u32 object) {
    notify(object, WaitReason::IX_RESULT, true);
  }
  int launch(const char* path, u32 caps) {
    return launchArgs(path, caps, "");
  }
  static int launch_policy(const char*, u32, const char*, u32, int);
  int launchArgs(const char* path, u32 caps, const char* arguments) {
    return launch_policy(path, caps, arguments, PROCESS_COMPAT, -1);
  }
  int launchStrict(const char* path, const char* arguments) {
    return launch_policy(path, 0, arguments, PROCESS_STRICT, -1);
  }
  int workspaceFrontend(u32 pid) {
    for (int i = 0; i < MaxProcesses; i++) {
      if (processes[i].state && processes[i].pid == pid) {
        return processes[i].workspace_frontend;
      }
    }
    return 0;
  }
  static int desktop_run(u32 operation, const DesktopRun* request) {
    if (request->path[sizeof(request->path) - 1] ||
      request->argument[sizeof(request->argument) - 1]) {
      return S_INVALID;
    }
    if (operation == DESKTOP_NETTEST) {
      return launch("/boot/nettest.snx", CAP_HOME | CAP_NETWORK);
    }
    if (operation != DESKTOP_RUN_APP && operation != DESKTOP_RUN_STRICT) {
      return S_INVALID;
    }

    char path[VFS_PATH];
    if (request->path[0] != '/' || vfs_canonical(request->path, path) < 0) {
      return S_INVALID;
    }
    int node = vfs_lookup(path);
    if (node < 0) {
      return S_NOTFOUND;
    }
    int home = vfs_lookup("/home/user"), boot = vfs_lookup("/boot");
    int parent = nodes[node].parent;
    while (parent > 0 && parent != home && parent != boot) {
      parent = nodes[parent].parent;
    }
    u32 len = strlen(nodes[node].name);
    if (parent <= 0 || nodes[node].dir || len < 5 ||
      !streq(nodes[node].name + len - 4, ".snx") ||
      (parent == boot && (!nodes[node].readonly || nodes[node].boot < 0))) {
      return S_DENIED;
    }
    if (operation == DESKTOP_RUN_STRICT) {
      return launchStrict(path, request->argument);
    }
    return launchArgs(path, CAP_HOME, request->argument);
  }
  int desktopRun(u32 shell, u32 operation, const DesktopRun* request) {
    if (operation == DESKTOP_KILL) {
      u32 target;
      memcpy(&target, request->path, sizeof(target));
      for (int i = 0; i < MaxProcesses; i++) {
        if (processes[i].state && processes[i].pid == target) {
          return processes[i].desktop_owner == shell
          ? kill(target, 0) : S_DENIED;
        }
      }
      return S_NOTFOUND;
    }
    int pid = desktop_run(operation, request);
    if (pid > 0) {
      for (int i = 0; i < MaxProcesses; i++) {
        if (processes[i].state && processes[i].pid == (u32)pid) {
          processes[i].desktop_owner = shell;
          break;
        }
      }
    }
    return pid;
  }
  // SNX 加载工作区仅归创建路径所有。依赖单核、内核不可抢占、装载不挂起且不递归
  // 进入来保证串行，禁止 IRQ/回调重入。读取、校验和全部映像复制完成前不能复用。
  // 若增加内核线程、异步 I/O 或可挂起加载，应先改成每次加载独占并负责回收的缓冲。
  static u8 image_scratch[MAX_FILE];
  static int launch_policy(const char* path, u32 caps, const char* arguments,
    u32 policy, int completion) {
    int n = vfs_lookup(path);
    if (n < 0) {
      return S_NOTFOUND;
    }
    if (nodes[n].dir || nodes[n].size < sizeof(NativeHeader) ||
      nodes[n].size > MAX_FILE) {
      return S_INVALID;
    }
    u8* bytes = image_scratch;
    if (vfs_read(n, bytes, MAX_FILE) < 0) {
      return S_IO;
    }
    NativeHeader* h = (NativeHeader*)bytes;
    if (!native_header_valid(h, nodes[n].size)) {
      return S_INVALID;
    }
    Process* p = 0;
    for (int i = 0; i < MaxProcesses; i++) {
      if (!processes[i].state) {
        p = &processes[i];
        break;
      }
    }
    if (!p || next_pid >= 0x7fffffu || strlen(arguments) > 255) {
      return S_LIMIT;
    }
    memset((void*)p, 0, sizeof(*p));
    p->abi = h->abi;
    p->code_end = USER_BASE + h->data_offset;
    p->pd = paging_user_create();
    if (!p->pd || p->kernelStack.allocate() != S_OK) {
      release(p);
      return S_NOMEM;
    }
    RunData* run = completion >= 0 ? completions[objects[completion].value].run : 0;
#ifdef SINO_SHELL_BRIDGE
    // 仅按只读 /boot 节点身份和精确模式启用；可写副本、普通镜像无此能力。
    const char* probe_mode = arguments;
    if (run) {
      probe_mode = run->cli.argc == 1 ? run->cli.bytes : "";
    }
    if (policy == PROCESS_STRICT && n == vfs_lookup("/boot/memv2.snx") &&
      streq(probe_mode, "oom-image")) {
      mmtest::arm(p->pd, mmtest::Stage::Image, 8);
    }
    if (policy == PROCESS_STRICT && n == vfs_lookup("/boot/fstest.snx") &&
      (streq(probe_mode, "anon-oom") || streq(probe_mode, "anon-fault-oom"))) {
      mmtest::arm(p->pd, mmtest::Stage::Anonymous, 1, true);
    }
#endif
    for (u32 offset = 0; offset < h->memory_bytes; offset += PAGE_SIZE) {
      phys_t page = alloc_user_page(p, USER_BASE + offset, offset >= h->data_offset);
      if (!page) {
        release(p);
        return S_NOMEM;
      }
      u32 length = h->image_bytes > offset ? h->image_bytes - offset : 0;
      if (length > PAGE_SIZE) {
        length = PAGE_SIZE;
      }
      if (length) {
        void* mapped = kmap(page);
        memcpy(mapped, bytes + offset, length);
        kunmap(mapped);
      }
    }
    phys_t params_page = alloc_user_page(p, p->abi == 2 ? USER_V2_ARGUMENTS : USER_V1_ARGUMENTS, 0);
    if (!params_page) {
      release(p);
      return S_NOMEM;
    }
    char executable[128];
    vfs_path(n, executable);
    u8* params = (u8*)kmap(params_page);
    memcpy(params, arguments, strlen(arguments) + 1);
    memcpy(params + 256, executable, strlen(executable) + 1);
    kunmap(params);
    if (!alloc_user_stack(p, 0)) {
      release(p);
      return S_NOMEM;
    }
#ifdef SINO_SHELL_BRIDGE
    if (policy == PROCESS_STRICT && n == vfs_lookup("/boot/poltest.snx") &&
      (streq(arguments, "paging-c") || streq(arguments, "paging-c-fault") ||
      streq(arguments, "paging-c-oom"))) {
      int status = paging_probe_setup(p, streq(arguments, "paging-c-oom"));
      if (status != S_OK) {
        release(p);
        return status;
      }
    }
#endif
    if (run) {
      if (run->pending_directory) {
        int h = handle_open(p->handles, (int)run->pending_directory - 1, run->directory_rights);
        if (h < 0) { release(p); return h; }
        p->directory = (u32)h;
        run->pending_directory = 0; /* Transfer the pending reference to the child. */
      }
      u32* handles[3] = {&run->cli.in, &run->cli.out, &run->cli.err};
      for (int i = 0; i < 3; i++) {
        int bound = run->pending_pipe[i] != 0;
        int stream = bound ? (int)run->pending_pipe[i] - 1
        : object_new(ObjType::STREAM, completion * 3 + i);
        if (stream < 0) { release(p); return stream; }
        if (!bound) {
          objects[completion].refs++;
        } /* Finite streams pin the run. */
        int handle = handle_open(p->handles, stream, i == 0 ? 1 : 2);
        if (handle < 0) {
          if (!bound) {
            object_unref(stream);
          }
          release(p); return handle;
        }
        if (bound) {
          run->pending_pipe[i] = 0;
        } /* Reference now belongs to child. */
        *handles[i] = (u32)handle;
      }
    }
    p->pid = next_pid++;
    p->completion_object = (u32)(completion + 1);
    if (completion >= 0) {
      completions[objects[completion].value].result.pid = p->pid;
    }
    p->policy = policy;
    p->input_frontend = p->workspace_frontend = 0;
    if (!next_pid) {
      next_pid = 1;
    }
    p->state = 1;
    p->caps = caps & 7; /* Boot utilities have no write, network or management
    grant, regardless of launcher. */
    if (p->input_frontend) {
      p->caps = 0;
    }
    u32 j = 0;
    while (nodes[n].name[j] && j < 31) {
      p->name[j] = nodes[n].name[j];
      j++;
    }
    p->thread[0].initialize(p, 0, USER_BASE + h->entry, user_stack_top(p, 0));
    if (run && run->timed) {
      armTimer(p->timer, run->deadline, TimerKind::Run, p);
    }
    pr_debug("PROCESS start ");
    pr_debug(p->name);
    pr_debug(" pid=");
    pr_debug_hex(p->pid);
    pr_debug("\n");
    return (int)p->pid;
  }
  /* Both old tracked launches and bounded runs use the same startup transaction. */
  int launchConsole(void) {
    int pid = launch_policy("/boot/conhost.snx", 0, "", PROCESS_COMPAT, -1);
    if (pid > 0) {
      for (u32 i = 0; i < MaxProcesses; i++) {
        if (processes[i].pid == (u32)pid && processes[i].state == 1) {
          processes[i].input_frontend = processes[i].workspace_frontend = 1;
        }
      }
    }
    return pid;
  }
  static int tracked_launch(Handle* owner, const char* path, const char* args, RunData* run) {
    int slot;
    for (slot = 0; slot < PROCESS_COMPLETIONS && completions[slot].used; slot++){
      ;
    }
    if (slot == PROCESS_COMPLETIONS) { run_free(run); return S_LIMIT; }
    completions[slot].used = 1;
    completions[slot].run = run;
    int object = object_new(ObjType::PROCESS, slot);
    if (object < 0) { completionRelease(slot); return object; }
    int handle = handle_open(owner, object, PROCESS_WAIT | PROCESS_CANCEL);
    if (handle < 0) { object_unref(object); return handle; }
    objects[object].refs++;
    int pid = launch_policy(path, 0, args, PROCESS_STRICT, object);
    if (pid < 0) {
      handle_close(owner, (u32)handle);
      object_unref(object);
      return pid;
    }
    return handle;
  }
  int interactiveResult(Handle* owner, u32 handle,
    ProcessCompletion* out) {
    Object* o = handle_get(owner, handle, ObjType::PROCESS, PROCESS_WAIT);
    if (!o) {
      return S_BADHANDLE;
    }
    if (!completions[o->value].complete) {
      return S_AGAIN;
    }
    *out = completions[o->value].result;
    return 0;
  }
  int interactiveStart(Handle* owner, const InteractiveSpec* spec,
    int directory, void* session, u32* pid) {
    if (spec->size != sizeof(*spec) || spec->version != 1 ||
      spec->argc > RUN_ARGC || spec->bytes > RUN_ARGS ||
      (spec->rights & ~(FS_RIGHTS & ~FS_DELEGATE)) ||
      (!directory && spec->rights)) {
      return S_INVALID;
    }
    u32 next = 0;
    for (u32 i = 0; i < spec->argc; i++) {
      if (spec->offsets[i] != next || next >= spec->bytes) {
        return S_INVALID;
      }
      while (next < spec->bytes && spec->argv[next]) {
        next++;
      }
      if (next == spec->bytes) {
        return S_INVALID;
      }
      next++;
    }
    if (next != spec->bytes) {
      return S_INVALID;
    }
    int reader = spec->program == IX_READTOOLS || spec->program == IX_BOOTTOOLS;
    int boot_copy = spec->program == IX_BOOTCOPY;
    int writer = spec->program == IX_WRITETOOLS || boot_copy;
    const char* path = spec->program==IX_SHELL ? "/boot/gsh.snx" : spec->program==IX_COMMAND ? "/boot/gcmd.snx" : writer ? "/boot/writer.snx" : reader ? "/boot/tview.snx" : "/boot/brokerqa.snx";
    if (spec->program != 1 && spec->program != 2 && spec->program!=IX_SHELL && spec->program!=IX_COMMAND && !reader && !writer) {
      return S_DENIED;
    }
    if (spec->program != IX_SHELL && !writer &&
      (spec->rights & ~(FS_READ|FS_LOOKUP|FS_ENUM))) {
      return S_DENIED;
    }
    if (boot_copy &&
      spec->rights != (FS_READ | FS_LOOKUP | FS_WRITE | FS_CREATE)) {
      return S_DENIED;
    }
    if (spec->program == IX_COMMAND && spec->rights) {
      return S_DENIED;
    }
    int n = vfs_lookup(path);
    if (n < 0 || !nodes[n].readonly || nodes[n].boot < 0) {
      return S_DENIED;
    }
    int grant = -1;
    if (spec->rights) {
      grant = filesys_acquire_directory(owner, directory, spec->rights);
      if (grant < 0) {
        return grant;
      }
    }
    CliInfo* cli = (CliInfo*)kpage_alloc();
    if (!cli) {
      if (grant >= 0) {
        object_unref(grant);
      }
      return S_NOMEM;
    }
    cli->size = sizeof(*cli);
    cli->version = 1;
    cli->argc = spec->argc;
    cli->argv_bytes = spec->bytes;
    memcpy(cli->offsets, spec->offsets, sizeof(cli->offsets));
    memcpy(cli->bytes, spec->argv, spec->bytes);
    int h = tracked_launch(owner, path, "", 0);
    if (h < 0) {
      kpage_free(cli);
      if (grant >= 0) {
        object_unref(grant);
      }
      return h;
    }
    Handle* handles = terminalTarget(owner, h, pid);
    Process* child = 0;
    for (int i = 0; i < MaxProcesses; i++) {
      if (handles == processes[i].handles) {
        child = &processes[i];
        break;
      }
    }
    if (!child) {
      kpage_free(cli);
      if (grant >= 0) {
        object_unref(grant);
      }
      handle_close(owner, h);
      return S_IO;
    }
    child->interactive = cli;
    int status = 0;
    if (grant >= 0) {
      status = handle_open(handles, grant, spec->rights);
      if (status >= 0) {
        child->directory = status;
      }
      else {
        object_unref(grant);
      }
    }
    if (status >= 0 && boot_copy) {
      status = filesys_boot_reader(handles);
      if (status >= 0) {
        child->boot_directory = status;
      }
    }
    if (status >= 0) {
      status = terminal_bind_interactive(session, handles, *pid, cli);
    }
    if (status < 0) {
      release(child);
      handle_close(owner, h);
      return status;
    }
    return h;
  }
  void runLimits(RunLimits* out) {
    *out = (RunLimits){
      sizeof(RunLimits), 1,         RUN_COUNT, RUN_ARGC,   RUN_ARGS,
      RUN_BYTES,         RUN_BYTES, RUN_BYTES, RUN_IO_MAX, RUN_TIMEOUT_MAX};
  }
  int runStart(Handle* owner, const RunSpec* spec) {
    return runStartDirectory(owner, spec, 0, 0);
  }
  int runStartDirectory(Handle* owner, const RunSpec* spec, u32 directory, u32 rights) {
    return runStartBound(owner, spec, directory, rights, 0);
  }
  /* 【简化实现：运行记录与捕获耦合】即使三个标准流均绑定管道，仍占 RUN_COUNT
  * （当前 2）额度并分配三页有限输入/捕获缓冲，限制了多阶段管线。
  * 替换方向：将进程启动、完成观察与有限捕获分开，仅为使用捕获的角色分配
  * 缓冲；管线成员先全部启动，再统一等待，启动失败时关闭端点并回收已启动成员。 */
  int runStartBound(Handle* owner, const RunSpec* spec, u32 directory, u32 rights,
    const u32* streams) {
    if (spec->size != sizeof(RunSpec) || spec->version != 1 || spec->flags ||
      spec->reserved || spec->argc > RUN_ARGC || spec->argv_bytes > RUN_ARGS ||
      spec->input_bytes > RUN_BYTES || spec->timeout_ticks > RUN_TIMEOUT_MAX ||
      (!spec->argc && spec->argv_bytes)) {
      return S_INVALID;
    }
    if ((!directory && rights) || (directory && !rights)) {
      return S_INVALID;
    }
    if (streams && streams[0] && spec->input_bytes) {
      return S_INVALID;
    }
    if (run_count >= RUN_COUNT) {
      return S_LIMIT;
    }
    RunData* run = (RunData*)kpage_alloc();
    if (!run) {
      return S_NOMEM;
    }
    run_count++;
    for (int i = 0; i < 3; i++) {
      run->buffer[i] = (u8*)kpage_alloc();
      if (!run->buffer[i]) { run_free(run); return S_NOMEM; }
    }
    /* Caller has bounded and validated these kernel/user input ranges. */
    const char* path = (const char*)spec->path;
    u32 path_size = 0;
    while (path_size < sizeof(run->path) && path[path_size]) {
      path_size++;
    }
    if (path_size == sizeof(run->path) || path[0] != '/' ||
      vfs_canonical(path, run->path) < 0) { run_free(run); return S_INVALID; }
    run->cli.size = sizeof(CliInfo); run->cli.version = 1;
    run->cli.argc = spec->argc; run->cli.argv_bytes = spec->argv_bytes;
    if (spec->argc) {
      memcpy(run->cli.offsets, (void*)spec->offsets, spec->argc * 4);
    }
    if (spec->argv_bytes) {
      memcpy(run->cli.bytes, (void*)spec->argv, spec->argv_bytes);
    }
    u32 next = 0;
    for (u32 i = 0; i < spec->argc; i++) {
      if (run->cli.offsets[i] != next || next >= spec->argv_bytes) { run_free(run); return S_INVALID; }
      while (next < spec->argv_bytes && run->cli.bytes[next]) {
        next++;
      }
      if (next == spec->argv_bytes) { run_free(run); return S_INVALID; }
      next++;
    }
    if (next != spec->argv_bytes) { run_free(run); return S_INVALID; }
    run->input_bytes = spec->input_bytes;
    if (spec->input_bytes) {
      memcpy(run->buffer[0], (void*)spec->input, spec->input_bytes);
    }
    run->timed = spec->timeout_ticks != 0;
    run->deadline = ticks + spec->timeout_ticks;
    if (directory) {
      int object = filesys_acquire_directory(owner, directory, rights);
      if (object < 0) { run_free(run); return object; }
      run->pending_directory = (u32)object + 1;
      run->directory_rights = rights;
    }
    if (streams) {
      for (int i = 0; i < 3; i++) {
        if (streams[i]) {
          int object = pipe_acquire(owner, streams[i], i != 0);
          if (object < 0) { run_free(run); return object; }
          run->pending_pipe[i] = (u32)object + 1;
        }
      }
    }
    return tracked_launch(owner, run->path, "", run);
  }
  static int run_result(const Object* object, RunResult* out) {
    int slot = object->value;
    RunData* run = completions[slot].run;
    if (!run) {
      return S_INVALID;
    }
    if (!completions[slot].complete) {
      return S_AGAIN;
    }
    *out = (RunResult){completions[slot].result, run->input_position,
      run->length[0], run->length[1], run->incomplete};
    return 0;
  }
  int runResult(Handle* owner, u32 handle, RunResult* out) {
    Object* object = handle_get(owner, handle, ObjType::PROCESS, PROCESS_WAIT);
    return object ? run_result(object, out) : S_BADHANDLE;
  }
  int runRead(Handle* owner, u32 handle, u32 role, u32 offset, void* out, u32 cap) {
    RunResult result;
    int status = runResult(owner, handle, &result);
    if (status) {
      return status;
    }
    if (role < 1 || role > 2 || cap > RUN_IO_MAX) {
      return S_INVALID;
    }
    Object* object = handle_get(owner, handle, ObjType::PROCESS, PROCESS_WAIT);
    RunData* run = completions[object->value].run;
    if (offset > run->length[role - 1]) {
      return S_INVALID;
    }
    u32 bytes = run->length[role - 1] - offset;
    if (bytes > cap) {
      bytes = cap;
    }
    if (bytes) {
      memcpy(out, run->buffer[role] + offset, bytes);
    }
    return (int)bytes;
  }
  int cancelOwned(Handle* owner, u32 handle) {
    Object* object = handle_get(owner, handle, ObjType::PROCESS, PROCESS_CANCEL);
    if (!object) {
      return S_BADHANDLE;
    }
    if (completions[object->value].complete) {
      return 0;
    }
    for (int i = 0; i < MaxProcesses; i++) {
      Process* p = &processes[i];
      if (p->state && p->pid == completions[object->value].result.pid &&
        p->completion_object == (u32)(object - objects) + 1) {
        p->retire(PROCESS_CANCELED, 0);
        return 0;
      }
    }
    return S_NOTFOUND;
  }
  Handle* terminalTarget(Handle* owner,u32 h,u32* pid) {
    Object* o=handle_get(owner,h,ObjType::PROCESS,PROCESS_WAIT|PROCESS_CANCEL);
    if(!o||completions[o->value].complete){
      return 0;
    }
    for(int i=0;i<MaxProcesses;i++){
      if(processes[i].state==1 && processes[i].policy==PROCESS_STRICT &&
        processes[i].completion_object==(u32)(o-objects)+1){*pid=processes[i].pid;return processes[i].handles;}
    }
    return 0;
  }
  Handle* interactiveHandles(u32 pid) {
    for(int i=0;i<MaxProcesses;i++){
      if(processes[i].state==1&&processes[i].pid==pid&&processes[i].interactive){
        return processes[i].handles;
      }
    }
    return 0;
  }
  int terminalCancel(u32 pid) {
    for(int i=0;i<MaxProcesses;i++){
      if(pid&&processes[i].pid==pid&&processes[i].state){
        processes[i].retire(PROCESS_CANCELED, 0);return 0;
      }
    }
    return S_NOTFOUND;
  }
  int kill(u32 pid, i32 code) {
    for (int i = 0; i < MaxProcesses; i++) {
      if (processes[i].state && processes[i].pid == pid) {
        if (processes[i].state == 2) {
          return 0;
        } /* First terminal event wins. */
        processes[i].retire(PROCESS_KILLED, code);
        return 0;
      }
    }
    return S_NOTFOUND;
  }
  static int homepath(u32 address, char* path, int writing) {
    char raw[VFS_PATH];
    u32* user_pd = current->pd;
    UACCESS_TRY(copy_string_from_user(user_pd, raw, address, sizeof(raw)));
    if (raw[0] != '/') {
      return S_INVALID;
    }
    if (vfs_canonical(raw, path) < 0) {
      return S_INVALID;
    }
    if (!writing) {
      return 0;
    }
    if (!(current->caps & CAP_HOME)) {
      return S_DENIED;
    }
    const char* home = "/home/user/";
    for (u32 i = 0; home[i]; i++) {
      if (path[i] != home[i]) {
        return S_DENIED;
      }
    }
    return 0;
  }
  void leave(Frame* f) {
    Thread* t = &current->thread[currentThread];
    memcpy(&t->saved, f, sizeof(UserFrame));
    user_switch_out = 1;
    paging_activate_kernel();
  }
  static Object* pipe_handle(u32 handle, int reading) {
    return handle_get(current->handles, handle, reading ? ObjType::PIPE_READ : ObjType::PIPE_WRITE,
      reading ? 1 : 2);
  }
  static int transfer_pipe(Object* endpoint, int reading, u32 buffer, u32 bytes) {
    if (bytes > RUN_IO_MAX) {
      return S_INVALID;
    }
    if (bytes) {
      UACCESS_TRY(uaccess_prepare(current->pd, buffer, bytes, reading ? UserAccess::WRITE : UserAccess::READ));
    }
    int result = pipe_transfer(endpoint, reading, (void*)buffer, bytes);
    if (result > 0) {
      pipeWake(endpoint);
      u32 peer = pipe_peer(endpoint);
      if (peer) { pipeWake(&objects[peer - 1]); }
    }
    return result;
  }
  static int user_completion_result(Frame* f, const Object* object) {
    int run_query = f->eax == SYS_RUN_RESULT;
    if (!run_query && !object) {
      return S_BADHANDLE;
    }
    u32 bytes = run_query ? sizeof(RunResult) : sizeof(ProcessCompletion);
    if (f->edx == RESULT_BLOCK_FLAG ? f->esi > STREAM_WAIT_MAX : f->edx < bytes) {
      return S_INVALID;
    }
    UACCESS_TRY(uaccess_prepare(current->pd, f->ecx, bytes, UserAccess::WRITE));
    if (!object) {
      return S_BADHANDLE;
    }
    if (run_query) {
      RunResult result;
      int status = run_result(object, &result);
      if (status == S_OK) {
        memcpy((void*)f->ecx, &result, bytes);
      }
      return status;
    }
    if (!completions[object->value].complete) {
      return S_AGAIN;
    }
    memcpy((void*)f->ecx, &completions[object->value].result, bytes);
    return S_OK;
  }
  int retryWait(Thread* t) {
    u32 now = ticks;
    int result = S_TIMEOUT;
    if (!t->waitExpired(now)) {
      Frame* f = &t->saved.frame;
      if (t->waitOp == WaitReason::COMPLETION) {
        Object* object = handle_get(current->handles, f->ebx, ObjType::PROCESS, PROCESS_WAIT);
        result = user_completion_result(f, object);
      } else {
        if (t->waitOp == WaitReason::TERM_READ || t->waitOp == WaitReason::IX_RESULT) {
          result = terminal_call(current->handles, current->pid, current->policy,
            &t->waitTerminal, current->pd);
        } else {
          int reading = t->waitOp == WaitReason::PIPE_READ;
          Object* endpoint = pipe_handle(f->ebx, reading);
          result = endpoint ? transfer_pipe(endpoint, reading, f->ecx, f->edx & ~STREAM_BLOCK_FLAG)
          : S_BADHANDLE;
        }
      }
    }
    if (result == S_AGAIN) {
      /* Another waiter may have transferred first. Keep the original deadline. */
      t->reblock();
      return 0;
    }
    t->finishWait(result, now);
    return 1;
  }
  /* Allow local mechanisms, public queries and explicitly authorized objects.
  * New calls are denied until reviewed; old ambient paths cannot bypass this. */
  static int strict_call(u32 call) {
    switch (call) {
    case SYS_EXIT: case SYS_YIELD: case SYS_LOG: case SYS_TICKS:
    case SYS_CLOSE: case SYS_DUP: case SYS_THREAD: case SYS_THREAD_EXIT:
    case SYS_EVENT_CREATE: case SYS_EVENT_SET: case SYS_EVENT_WAIT:
    case SYS_DIRECTORY_COMMIT: case SYS_TIME: case SYS_CALENDAR: case SYS_UI_LANGUAGE:
    case SYS_SYSTEM_INFO: case SYS_START_INFO:
    case SYS_PROCESS_RESULT: case SYS_PROCESS_CANCEL:
    case SYS_CLI_INFO: case SYS_STREAM_READ: case SYS_STREAM_WRITE:
    case SYS_RUN_RESULT: case SYS_RUN_READ: case SYS_RUN_LIMITS:
    case SYS_START_DIRECTORY: case SYS_OPEN_AT:
    case SYS_FILE_READ_AT: case SYS_FILE_WRITE_AT: case SYS_FILE_STAT:
    case SYS_DIR_READ: case SYS_REMOVE_AT: case SYS_RENAME_AT:
    case SYS_FILE_STAGE: case SYS_FILE_PUBLISH: case SYS_FILE_COMMIT:
    case SYS_PIPE_CREATE: case SYS_STREAM_READY: case SYS_PIPE_LIMITS:
    case SYS_TERMINAL:
    case SYS_ANON_MAP: case SYS_ANON_UNMAP:
      return 1;
    default: return 0;
    }
  }
  int dispatchSyscall(Frame* f) {
    u32 a = f->ebx, b = f->ecx, c = f->edx;
    u32* user_pd = current->pd;
    char path[VFS_PATH];
    int n, r;
    Object* o;
    if (current->policy == PROCESS_STRICT && !strict_call(f->eax)) {
      return f->eax >= SYS_EXIT && f->eax < SYS_CALL_END ? S_DENIED : S_INVALID;
    }
    if (f->eax >= SYS_SOCKET && f->eax <= SYS_RECVFROM &&
      !(current->caps & CAP_NETWORK)) {
      return S_DENIED;
    }
    if ((f->eax >= SYS_DIR_GRANT && f->eax <= SYS_FILE_COMMIT && f->eax != SYS_START_DIRECTORY) || f->eax==SYS_DIRECTORY_COMMIT) {
      return filesys_call(current->handles, current->caps, f->eax, a, b, c,
        user_pd);
    }
    switch (f->eax) {
    case SYS_CONSOLE: {
        if (!current->input_frontend) {
          return S_DENIED;
        }
        if (!a) {
          return b || c ? S_INVALID : key_read();
        }
        if (a != 1 || c != sizeof(ConsoleFrame)) {
          return S_INVALID;
        }
        UACCESS_TRY(uaccess_prepare(user_pd, b, c, UserAccess::READ));
        const ConsoleFrame* frame = (const ConsoleFrame*)b;
        if (frame->cursor > CONSOLE_COLUMNS * CONSOLE_ROWS) {
          return S_INVALID;
        }
        console_present(frame->cells, frame->cursor);
        return S_OK;
      }
    case SYS_ANON_MAP:
      return b || c ? S_INVALID : anon_map(current, a);
    case SYS_ANON_UNMAP:
      return b || c ? S_INVALID : anon_unmap(current, a);
    case SYS_WORKSPACE:
      if(!current->workspace_frontend || a || b || c){
        return S_DENIED;
      }
      return filesys_workspace(current->handles);
    case SYS_TERMINAL: {
        if (b!=sizeof(TerminalRequest) || c) {
          return S_INVALID;
        }
        TerminalRequest request;
        UACCESS_TRY(copy_from_user(user_pd, &request, a, sizeof(request)));
        WaitReason waitOp = WaitReason::NONE; u32 timeout = request.extra;
        if (request.op == TERM_READ && request.value == TERM_READ_BLOCK) {
          waitOp = WaitReason::TERM_READ;
          request.value = 0;
        } else {
          if (request.op == IX_RESULT && (request.extra & IX_RESULT_BLOCK)) {
            waitOp = WaitReason::IX_RESULT;
            timeout &= ~IX_RESULT_BLOCK;
          }
        }
        if (waitOp != WaitReason::NONE) {
          if (timeout > STREAM_WAIT_MAX) {
            return S_INVALID;
          }
          request.extra = 0;
        }
        r = terminal_call(current->handles, current->pid, current->policy, &request, user_pd);
        if (waitOp != WaitReason::NONE && r == S_AGAIN) {
          o = handle_get(current->handles, request.handle, ObjType::TERM_CLIENT, 0);
          Thread* t = &current->thread[currentThread];
          t->waitTerminal = request;
          t->block((u32)(o - objects) + 1, waitOp, timeout);
          leave(f);
        }
        return r;
      }
    case SYS_PIPE_CREATE: {
        if (c < sizeof(PipePair)) {
          return S_INVALID;
        }
        UACCESS_TRY(uaccess_prepare(user_pd, b, sizeof(PipePair), UserAccess::WRITE));
        PipePair pair; r = pipe_create(current->handles, a, &pair);
        if (!r) {
          memcpy((void*)b, &pair, sizeof(pair));
        }
        return r;
      }
    case SYS_PIPE_LIMITS: {
        if (c || b < sizeof(PipeLimits)) {
          return S_INVALID;
        }
        UACCESS_TRY(uaccess_prepare(user_pd, a, sizeof(PipeLimits), UserAccess::WRITE));
        PipeLimits limits; pipe_limits(&limits); memcpy((void*)a, &limits, sizeof(limits)); return 0;
      }
    case SYS_STREAM_READY: {
        if (b || c) {
          return S_INVALID;
        }
        if(handle_get(current->handles,a,ObjType::TERM_CLIENT,0)){
          TerminalRequest q={TERM_READY,a,0,0,0,0};
          return terminal_call(current->handles,current->pid,current->policy,&q,user_pd);
        }
        o = handle_get(current->handles, a, ObjType::PIPE_READ, 1);
        if (!o) {
          o = handle_get(current->handles, a, ObjType::PIPE_WRITE, 2);
        }
        if (o) {
          return pipe_ready(o);
        }
        o = handle_get(current->handles, a, ObjType::STREAM, 0);
        if (!o || current->completion_object != (u32)(o->value / 3) + 1) {
          return S_BADHANDLE;
        }
        u32 role = (u32)o->value % 3;
        if (!handle_get(current->handles, a, ObjType::STREAM, role ? 2 : 1)) {
          return S_BADHANDLE;
        }
        RunData* run = completions[objects[o->value / 3].value].run;
        if (!role) {
          return STREAM_READABLE | (run->input_position == run->input_bytes ? STREAM_EOF : 0);
        }
        return run->length[role - 1] < RUN_BYTES ? STREAM_WRITABLE : STREAM_LIMIT;
      }
    case SYS_START_DIRECTORY: {
        if (a > 1 || b || c) {
          return S_INVALID;
        }
        u32 directory = a ? current->boot_directory : current->directory;
        if (!directory) {
          return S_NOTFOUND;
        }
        return handle_get(current->handles, directory, ObjType::DIRECTORY, 0)
        ? (int)directory : S_BADHANDLE;
      }
    case SYS_RUN_START: {
        if (c || (b != sizeof(RunSpec) && b != sizeof(RunSpecV2) && b != sizeof(RunSpecV3))) {
          return S_INVALID;
        }
        UACCESS_TRY(uaccess_prepare(user_pd, a, b, UserAccess::READ));
        RunSpec spec; memcpy(&spec, (void*)a, sizeof(spec));
        u32 directory = 0, rights = 0, streams[3] = {0, 0, 0};
        if (b >= sizeof(RunSpecV2)) {
          RunSpecV2 extended; memcpy(&extended, (void*)a, sizeof(extended));
          if (spec.size != b || spec.version != (b == sizeof(RunSpecV2) ? 2u : 3u) ||
            extended.reserved[0] || extended.reserved[1]) {
            return S_INVALID;
          }
          directory = extended.directory; rights = extended.directory_rights;
          spec.size = sizeof(spec); spec.version = 1;
        }
        if (b == sizeof(RunSpecV3)) {
          RunSpecV3 extended; memcpy(&extended, (void*)a, sizeof(extended));
          if (extended.reserved) {
            return S_INVALID;
          }
          streams[0] = extended.in; streams[1] = extended.out; streams[2] = extended.err;
        }
        if (spec.argc > RUN_ARGC || spec.argv_bytes > RUN_ARGS || spec.input_bytes > RUN_BYTES) {
          return S_INVALID;
        }
        UACCESS_TRY(copy_string_from_user(user_pd, path, spec.path, sizeof(path)));
        if (spec.argc) {
          UACCESS_TRY(uaccess_prepare(user_pd, spec.offsets, spec.argc * 4, UserAccess::READ));
        }
        if (spec.argv_bytes) {
          UACCESS_TRY(uaccess_prepare(user_pd, spec.argv, spec.argv_bytes, UserAccess::READ));
        }
        if (spec.input_bytes) {
          UACCESS_TRY(uaccess_prepare(user_pd, spec.input, spec.input_bytes, UserAccess::READ));
        }
        spec.path = (u32)path;
        return runStartBound(current->handles, &spec, directory, rights, streams);
      }
    case SYS_RUN_LIMITS: {
        if (c || b < sizeof(RunLimits)) {
          return S_INVALID;
        }
        UACCESS_TRY(uaccess_prepare(user_pd, a, sizeof(RunLimits), UserAccess::WRITE));
        RunLimits limits; runLimits(&limits); memcpy((void*)a, &limits, sizeof(limits)); return 0;
      }
    case SYS_CLI_INFO: {
        if (c || b < sizeof(CliInfo)) {
          return S_INVALID;
        }
        UACCESS_TRY(uaccess_prepare(user_pd, a, sizeof(CliInfo), UserAccess::WRITE));
        if (current->interactive) { memcpy((void*)a,current->interactive,sizeof(CliInfo)); return 0; }
        if (!current->completion_object) {
          return S_NOTFOUND;
        }
        RunData* run = completions[objects[current->completion_object - 1].value].run;
        if (!run) {
          return S_NOTFOUND;
        }
        memcpy((void*)a, &run->cli, sizeof(CliInfo)); return 0;
      }
    case SYS_STREAM_READ:
    case SYS_STREAM_WRITE: {
        int reading = f->eax == SYS_STREAM_READ;
        int blocking = (c & STREAM_BLOCK_FLAG) != 0;
        if (!blocking && handle_get(current->handles, a, ObjType::TERM_CLIENT, 0)) {
          TerminalRequest q={reading?TERM_READ:TERM_WRITE,a,b,c,0,0};
          return terminal_call(current->handles,current->pid,current->policy,&q,user_pd);
        }
        o = pipe_handle(a, reading);
        if (o) {
          if (blocking && f->esi > STREAM_WAIT_MAX) {
            return S_INVALID;
          }
          r = transfer_pipe(o, reading, b, c & ~STREAM_BLOCK_FLAG);
          if (blocking && r == S_AGAIN) {
            current->thread[currentThread].block(pipe_peer(o),
              reading ? WaitReason::PIPE_READ : WaitReason::PIPE_WRITE, f->esi);
            leave(f);
          }
          return r;
        }
        if (blocking) {
          return S_BADHANDLE;
        }
        o = handle_get(current->handles, a, ObjType::STREAM, reading ? 1 : 2);
        if (!o || current->completion_object != (u32)(o->value / 3) + 1) {
          return S_BADHANDLE;
        }
        u32 role = (u32)o->value % 3;
        if ((reading && role != 0) || (!reading && role == 0)) {
          return S_BADHANDLE;
        }
        if (!c) {
          return 0;
        }
        if (c > RUN_IO_MAX) {
          return S_INVALID;
        }
        UACCESS_TRY(uaccess_prepare(user_pd, b, c, (reading) ? UserAccess::WRITE : UserAccess::READ));
        RunData* run = completions[objects[o->value / 3].value].run;
        u32 bytes;
        if (reading) {
          bytes = run->input_bytes - run->input_position;
          if (bytes > c) {
            bytes = c;
          }
          if (bytes) {
            memcpy((void*)b, run->buffer[0] + run->input_position, bytes);
          }
          run->input_position += bytes;
        } else {
          bytes = RUN_BYTES - run->length[role - 1];
          if (bytes > c) {
            bytes = c;
          }
          if (bytes) {
            memcpy(run->buffer[role] + run->length[role - 1], (void*)b, bytes);
          }
          run->length[role - 1] += bytes;
          if (bytes < c) {
            run->incomplete = 1;
            current->retire(PROCESS_OUTPUT_LIMIT, 0);
            leave(f);
            return bytes ? (int)bytes : S_LIMIT;
          }
        }
        return (int)bytes;
      }
    case SYS_RUN_RESULT:
    case SYS_PROCESS_RESULT: {
        o = handle_get(current->handles, a, ObjType::PROCESS, PROCESS_WAIT);
        r = user_completion_result(f, o);
        if (c == RESULT_BLOCK_FLAG && r == S_AGAIN) {
          current->thread[currentThread].block((u32)(o - objects) + 1,
            WaitReason::COMPLETION, f->esi);
          leave(f);
        }
        return r;
      }
    case SYS_RUN_READ: {
        if (c != sizeof(RunRead)) {
          return S_INVALID;
        }
        RunRead read;
        UACCESS_TRY(copy_from_user(user_pd, &read, b, sizeof(read)));
        if (read.capacity > RUN_IO_MAX) {
          return S_INVALID;
        }
        if (read.capacity) {
          UACCESS_TRY(uaccess_prepare(user_pd, read.buffer, read.capacity, UserAccess::WRITE));
        }
        return runRead(current->handles, a, read.role, read.offset, (void*)read.buffer, read.capacity);
      }
    case SYS_SPAWN_TRACKED: {
        char args[256];
        if (c) {
          return S_INVALID;
        }
        r = homepath(a, path, 0);
        if (r) {
          return r;
        }
        if (b) {
          UACCESS_TRY(copy_string_from_user(user_pd, args, b, sizeof(args)));
        } else {
          args[0] = 0;
        }
        return tracked_launch(current->handles, path, args, 0);
      }
    case SYS_PROCESS_CANCEL:
      o = handle_get(current->handles, a, ObjType::PROCESS, PROCESS_CANCEL);
      if (!o) {
        return S_BADHANDLE;
      }
      if (b || c) {
        return S_INVALID;
      }
      return cancelOwned(current->handles, a);
    case SYS_SYSTEM_INFO: {
        if (c || b < sizeof(SystemInfo)) {
          return S_INVALID;
        }
        SystemInfo info = {sizeof(SystemInfo), 1, SYSTEM_FEATURES, 100,
          MaxProcesses, MaxThreads, HANDLES, OBJECTS, MAX_FILE, SNX_V2_IMAGE_BYTES, 16384, 0};
        return copy_to_user(user_pd, a, &info, sizeof(info));
      }
    case SYS_START_INFO: {
        if (c || b < sizeof(StartInfo)) {
          return S_INVALID;
        }
        StartInfo info = {sizeof(StartInfo), 1, current->policy, current->caps,
          SYSTEM_FEATURES, 0};
        return copy_to_user(user_pd, a, &info, sizeof(info));
      }
    case SYS_LEASE: {
        char name[64];
        UACCESS_TRY(copy_string_from_user(user_pd, name, a, sizeof(name)));
        if (!name[0]) {
          return S_INVALID;
        }
        int free = -1;
        for (int i = 0; i < 8; i++) {
          if (!leases[i].pid) {
            free = i;
          }
          else {
            if (streq(leases[i].name, name)) {
              if (leases[i].pid != current->pid) {
                return S_BUSY;
              }
              if (b) {
                leases[i].pid = 0;
              }
              return 0;
            }
          }
        }
        if (b) {
          return S_NOTFOUND;
        }
        if (free < 0) {
          return S_LIMIT;
        }
        leases[free].pid = current->pid;
        memcpy(leases[free].name, name, strlen(name) + 1);
        return 0;
      }
    case SYS_SENDTO:
    case SYS_RECVFROM:
      UACCESS_TRY(uaccess_prepare(user_pd, b, sizeof(SockMessage), (f->eax == SYS_RECVFROM) ? UserAccess::WRITE : UserAccess::READ));
      o = handle_get(current->handles, a, ObjType::SOCKET,
        f->eax == SYS_SENDTO ? 2 : 1);
      if (!o) {
        return S_BADHANDLE;
      }
      {
        SockMessage* m = (SockMessage*)b;
        if (m->size > 1024) {
          return S_INVALID;
        }
        UACCESS_TRY(uaccess_prepare(user_pd, m->buffer, m->size, (f->eax == SYS_RECVFROM) ? UserAccess::WRITE : UserAccess::READ));
        return f->eax == SYS_SENDTO ? net_sendto(o->value, (const u8*)m->buffer,
          m->size, &m->address)
        : net_recvfrom(o->value, (u8*)m->buffer,
          m->size, &m->address);
      }
    case SYS_SOCKET:
      n = net_socket((int)a);
      if (n < 0) {
        return n;
      }
      r = object_new(ObjType::SOCKET, n);
      if (r < 0) {
        net_close(n);
        return r;
      }
      n = handle_open(current->handles, r, 3);
      if (n < 0) {
        net_close(objects[r].value);
        objects[r].refs = 0;
      }
      return n;
    case SYS_BIND:
    case SYS_CONNECT:
      o = handle_get(current->handles, a, ObjType::SOCKET, 2);
      if (!o) {
        return S_BADHANDLE;
      }
      UACCESS_TRY(uaccess_prepare(user_pd, b, sizeof(SockAddress), UserAccess::READ));
      return f->eax == SYS_BIND ? net_bind(o->value, (const SockAddress*)b)
      : net_connect(o->value, (const SockAddress*)b);
    case SYS_LISTEN:
      o = handle_get(current->handles, a, ObjType::SOCKET, 2);
      return o ? net_listen(o->value) : S_BADHANDLE;
    case SYS_ACCEPT:
      o = handle_get(current->handles, a, ObjType::SOCKET, 1);
      if (!o) {
        return S_BADHANDLE;
      }
      n = net_accept(o->value);
      if (n < 0) {
        return n;
      }
      r = object_new(ObjType::SOCKET, n);
      if (r < 0) {
        net_close(n);
        return r;
      }
      n = handle_open(current->handles, r, 3);
      if (n < 0) {
        net_close(objects[r].value);
        objects[r].refs = 0;
      }
      return n;
    case SYS_SEND:
    case SYS_RECV:
      o = handle_get(current->handles, a, ObjType::SOCKET,
        f->eax == SYS_SEND ? 2 : 1);
      if (!o) {
        return S_BADHANDLE;
      }
      if (c > 1024) {
        return S_INVALID;
      }
      UACCESS_TRY(uaccess_prepare(user_pd, b, c, (f->eax == SYS_RECV) ? UserAccess::WRITE : UserAccess::READ));
      return f->eax == SYS_SEND ? net_send(o->value, (const u8*)b, c)
      : net_recv(o->value, (u8*)b, c);
    case SYS_NET_READY:
      o = handle_get(current->handles, a, ObjType::SOCKET, 1);
      return o ? net_ready(o->value) : S_BADHANDLE;
    case SYS_NET_INFO:
      UACCESS_TRY(uaccess_prepare(user_pd, a, sizeof(NetStats), UserAccess::WRITE));
      *(NetStats*)a = net_loop.stats;
      return 0;
    case SYS_PING:
      return net_ping(a, b);
    case SYS_CALENDAR: {
        if (c || b < sizeof(CalendarTime)) {
          return S_INVALID;
        }
        UACCESS_TRY(uaccess_prepare(user_pd, a, sizeof(CalendarTime), UserAccess::WRITE));
        RtcDateTime date; int result=rtc_read(&date);
        if(!result){CalendarTime out={sizeof(out),1,date.year,date.month,date.day,date.hour,date.minute,date.second};memcpy((void*)a,&out,sizeof(out));}
        return result;
      }
    case SYS_TIME:
      if (b < sizeof(TimeOfDay)) {
        return S_INVALID;
      }
      UACCESS_TRY(uaccess_prepare(user_pd, a, sizeof(TimeOfDay), UserAccess::WRITE));
      {
        TimeOfDay value;
        int result = rtc_time(&value);
        if (!result) {
          *(TimeOfDay*)a = value;
        }
        return result;
      }
    case SYS_OPEN_PATH:
      return S_INVALID;
    case SYS_FILE_CLIP:
      return S_INVALID;
    case SYS_DROP_ZONES:
      return S_INVALID;
    case SYS_WINDOW_SIZE:
      return S_BADHANDLE;
    case SYS_UI_LANGUAGE:
      return settings_language;
    case SYS_WINDOW_TITLE:
      return S_BADHANDLE;
    case SYS_DUP:
      return handle_duplicate(current->handles, a, b);
    case SYS_WINDOW_POLICY:
      return S_BADHANDLE;
    case SYS_MOVE:
      r = homepath(a, path, 1);
      if (r) {
        return r;
      }
      {
        char dest[VFS_PATH];
        r = homepath(b, dest, 1);
        if (r) {
          return r;
        }
        r = vfs_move(path, dest);
        if (r < 0) {
          return r == S_BUSY ? S_BUSY : S_IO;
        }
        return 0;
      }
    case SYS_CLIP_GET:
      return 0;
    case SYS_CLIP_SET:
      return 0;
    case SYS_EXIT:
      current->retire(PROCESS_EXITED, (int)a);
      process_exits++;
      leave(f);
      return 0;
    case SYS_YIELD:
      current->thread[currentThread].sleep(a > 1000 ? 1000 : a);
      leave(f);
      return 0;
    case SYS_TICKS:
      return (int)ticks;
    case SYS_LOG:
      if (c > 1024) {
        return S_INVALID;
      }
      UACCESS_TRY(uaccess_prepare(user_pd, a, c, UserAccess::READ));
      klog_write_bytes((const void*)a, c);
      return (int)c;
    case SYS_WINDOW:
      return S_INVALID;
    case SYS_SCENE:
      return S_BADHANDLE;
    case SYS_EVENT:
      return S_INVALID; /* Reserved GUI event call. */
    case SYS_CLOSE:
      r = handle_close(current->handles, a);
      if (r == S_OK) {
        for (int i = 0; i < MaxThreads; i++) {
          Thread* t = &current->thread[i];
          u32 handle = (t->waitOp == WaitReason::TERM_READ || t->waitOp == WaitReason::IX_RESULT)
          ? t->waitTerminal.handle : t->saved.frame.ebx;
          if (t->waitOp != WaitReason::NONE && handle == a) {
            t->finishWait(S_BADHANDLE, ticks);
          }
        }
      }
      return r;
    case SYS_READ:
      r = homepath(a, path, 0);
      if (r) {
        return r;
      }
      n = vfs_lookup(path);
      if (n < 0) {
        return S_NOTFOUND;
      }
      if (c > MAX_FILE) {
        return S_INVALID;
      }
      UACCESS_TRY(uaccess_prepare(user_pd, b, c, UserAccess::WRITE));
      if (nodes[n].size > c) {
        return S_LIMIT;
      }
      return vfs_read(n, (u8*)b, c) < 0 ? S_IO : (int)nodes[n].size;
    case SYS_WRITE: {
        r = homepath(a, path, 1);
        if (r) {
          return r;
        }
        if (c > MAX_FILE) {
          return S_INVALID;
        }
        UACCESS_TRY(uaccess_prepare(user_pd, b, c, UserAccess::READ));
        n = vfs_lookup(path);
        if (n >= 0 && nodes[n].dir) {
          return S_INVALID;
        }
        int created = n < 0;
        if (created) {
          n = vfs_create(path, 0, 0);
        }
        if (n < 0) {
          return S_IO;
        }
        if (vfs_write(n, (const u8*)b, c, 0) < 0) {
          if (created) {
            vfs_remove(path, 0);
          }
          return S_IO;
        }
        return 0;
      }
    case SYS_MKDIR:
      r = homepath(a, path, 1);
      if (r) {
        return r;
      }
      return vfs_create(path, 1, 1) < 0 ? S_IO : 0;
    case SYS_REMOVE:
      r = homepath(a, path, 1);
      if (r) {
        return r;
      }
      r = vfs_remove(path, (int)b);
      return r < 0 ? (r == S_BUSY ? S_BUSY : S_IO) : 0;
    case SYS_LIST:
      r = homepath(a, path, 0);
      if (r) {
        return r;
      }
      n = vfs_lookup(path);
      if (n < 0 || !nodes[n].dir) {
        return S_NOTFOUND;
      }
      if (c > VFS_NODES) {
        return S_INVALID;
      }
      UACCESS_TRY(uaccess_prepare(user_pd, b, c * sizeof(FileInfo), UserAccess::WRITE));
      {
        u32 count = 0;
        FileInfo* out = (FileInfo*)b;
        for (int i = 1; i < VFS_NODES && count < c; i++) {
          if (nodes[i].used && nodes[i].parent == n) {
            memcpy(out[count].name, nodes[i].name, 32);
            out[count].size = nodes[i].size;
            out[count++].dir = (u32)nodes[i].dir;
          }
        }
        return (int)count;
      }
    case SYS_SPAWN:
      r = homepath(a, path, 0);
      if (r) {
        return r;
      }
      char args[256];
      if (b) {
        UACCESS_TRY(copy_string_from_user(user_pd, args, b, sizeof(args)));
      } else
      {
        args[0] = 0;
      }
      return launchArgs(path, current->caps & CAP_HOME,
        args); /* no implicit network grant */
    case SYS_KILL:
      if (a != current->pid && !(current->caps & CAP_MANAGE)) {
        return S_DENIED;
      }
      return kill(a, (int)b);
    case SYS_PROCESSES:
      if (c > MaxProcesses) {
        return S_INVALID;
      }
      UACCESS_TRY(uaccess_prepare(user_pd, b, c * sizeof(ProcessInfo), UserAccess::WRITE));
      return list((ProcessInfo*)b, c);
    case SYS_SYNC:
      if (storage_sync()) {
        return 0;
      }
      pr_debug("SYS_SYNC failed: ");
      pr_debug(storage_error);
      pr_debug("\n");
      return S_IO;
    case SYS_THREAD:
      if (a < USER_BASE + sizeof(NativeHeader) || a >= current->code_end ||
        paging_user_flags(current->pd, a) != (PAGE_PRESENT | PAGE_USER)) {
        return S_INVALID;
      }
      for (int i = 1; i < MaxThreads; i++) {
        if (current->thread[i].state == ThreadState::UNUSED) {
          if (!alloc_user_stack(current, i)) {
            return S_NOMEM;
          }
          current->thread[i].initialize(current, (u32)i, a, user_stack_top(current, i));
          return i;
        }
      }
      return S_LIMIT;
    case SYS_THREAD_EXIT: {
        current->thread[currentThread].cancel();
        int live = 0;
        for (int i = 0; i < MaxThreads; i++) {
          if (current->thread[i].state != ThreadState::UNUSED) {
            live = 1;
          }
        }
        if (!live) {
          current->retire(PROCESS_EXITED, 0);
        }
        u32 top = user_stack_top(current, currentThread);
        free_user_pages(current, top - UserStackBytes, top);
        leave(f);
        return 0;
      }
    case SYS_EVENT_CREATE:
      n = object_new(ObjType::EVENT, 0);
      if (n < 0) {
        return n;
      }
      r = handle_open(current->handles, n, 3);
      if (r < 0) {
        objects[n].refs = 0;
      }
      return r;
    case SYS_EVENT_SET:
      o = handle_get(current->handles, a, ObjType::EVENT, 2);
      if (!o) {
        return S_BADHANDLE;
      }
      o->value = !!b;
      return 0;
      /* 【简化实现：事件等待】只读取事件值，未置位便返回 S_AGAIN，调用方需重试。
      * 替换方向：接入公共线程等待机制，由 Set/关闭/超时唤醒，明确手动或自动
      * 复位语义；不能仅将这里改成循环，否则会阻塞整个不可抢占内核。 */
    case SYS_EVENT_WAIT:
      o = handle_get(current->handles, a, ObjType::EVENT, 1);
      if (!o) {
        return S_BADHANDLE;
      }
      return o->value ? 0 : S_AGAIN;
    default:
      return S_INVALID;
    }
  }
  void tests(void) {
    pr_debug("PROCESS address spaces initialized (8 processes x 4 threads)\n");
  }

  int list(ProcessInfo* out, u32 cap) {
    u32 count = 0;
    for (int i = 0; i < MaxProcesses && count < cap; i++) {
      if (processes[i].state) {
        Process* p = &processes[i];
        ProcessInfo* v = &out[count++];
        memset(v, 0, sizeof(*v));
        v->pid = p->pid;
        v->state = p->state;
        v->exit_code = p->code;
        memcpy(v->name, p->name, 32);
        for (int j = 0; j < MaxThreads; j++) {
          if (p->thread[j].state != ThreadState::UNUSED) {
            v->threads++;
          }
        }
        v->pages = paging_user_count(p->pd);
      }
    }
    return (int)count;
  }

  int alive(u32 pid) {
    for (int i = 0; i < MaxProcesses; i++) {
      if (processes[i].pid == pid && processes[i].state == 1) {
        return 1;
      }
    }
    return 0;
  }

  void reapAndExpireRuns() {
    expireTimers(ticks);
    while (Process* p = takeReap()) {
      release(p);
    }
  }

  void setEntryStack(const Process& process) {
    tss[1] = process.kernelStack.top();
  }

}
