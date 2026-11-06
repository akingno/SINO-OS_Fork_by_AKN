#include "klog.h"
/* Preemption only at user boundaries. Kernel/desktop remain non-preemptible. */
#include "process.h"
#include "mm/paging.h"
#include "mm/uaccess.h"
#include "object.h"
#include "vfs.h"
#include "gui.h"
#include "storage.h"
#include "net.h"
#include "settings.h"
#include "rtc.h"
#include "filesys.h"
#include "pipe.h"
#include "terminal.h"
#include "ui_queue.h"
#define NP 8
#define NT 4
#define USER_STACK_BYTES 16384u
typedef struct {
  Frame frame;
  u32 useresp, ss;
} UserFrame;
enum { THREAD_UNUSED, THREAD_ACTIVE, THREAD_BLOCKED };
enum { WAIT_PIPE_READ = 1, WAIT_PIPE_WRITE, WAIT_COMPLETION, WAIT_TERM_READ, WAIT_IX_RESULT };
typedef struct {
  int state;
  u32 wake;
  u32 wait_object; /* Object index + 1; zero when not waiting. */
  u32 wait_op;
  u32 wait_deadline;
  int wait_timed; /* A finite deadline can wrap to zero. */
  TerminalRequest wait_terminal;
  UserFrame saved;
} Thread;
typedef struct {
  u32 base, bytes;
} AnonRegion;
typedef struct {
  u32 pid, state, caps;
  int code;
  u32* pd;
  u32 abi, code_end;
  void* kernel_stack;
  Thread thread[NT];
  Handle handles[HANDLES];
  UiQueue input_queue;
  char name[32];
  u32 policy, workspace_frontend, input_frontend;

  u32 completion_object; /* Object index + 1; zero for old launch paths. */
  u32 reason, fault_vector;
  CliInfo* interactive;
  u32 directory; /* Explicit startup grant; never a global working directory. */
  u32 boot_directory;
  u32 desktop_owner;
  AnonRegion anon[ANON_MAX_REGIONS];
} Process;
typedef struct {
  CliInfo cli;
  char path[128];
  u8 *buffer[3];
  u32 input_bytes, input_position, length[2], deadline, timed, incomplete;
  u32 pending_directory, directory_rights;
  u32 pending_pipe[3]; /* Object index + 1, owned until transferred to child. */
} RunData;
_Static_assert(sizeof(RunData) <= 4096, "run metadata fits one page");
static u32 run_count;
static void run_free(RunData* run) {
  if (!run) return;
  if (run->pending_directory) object_unref((int)run->pending_directory - 1);
  for (int i = 0; i < 3; i++)
    if (run->pending_pipe[i]) object_unref((int)run->pending_pipe[i] - 1);
  for (int i = 0; i < 3; i++) if (run->buffer[i]) page_free(run->buffer[i]);
  page_free(run);
  run_count--;
}
static struct {
  int used, complete;
  ProcessCompletion result;
  RunData* run;
} completions[PROCESS_COMPLETIONS];
void process_completion_release(int index) {
  if (index >= 0 && index < PROCESS_COMPLETIONS) {
    run_free(completions[index].run);
    memset(&completions[index], 0, sizeof(completions[index]));
  }
}
void process_stream_release(int value) {
  object_unref(value / 3);
}
static Process processes[NP];
static Process* current;
static int current_thread;
static u32 cursor;
static u32 next_pid = 1, quantum_end;
static struct {
  u32 pid;
  char name[64];
} leases[8];
volatile u32 user_switch_out;
u32 process_faults, process_switches, process_exits;
static u32 gdt[12], tss[26];
extern void user_enter(UserFrame*);
static void descriptor(int i, u32 base, u32 limit, u32 access, u32 flags) {
  gdt[i * 2] = (limit & 65535) | (base << 16);
  gdt[i * 2 + 1] = (base & 0xff000000) | ((base >> 16) & 255) | (access << 8) |
                   (limit & 0xf0000) | (flags << 20);
}
void process_init(void) {
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
static void thread_unblock(u32 object, u32 op);
static void release(Process* p) {
  for (int i = 0; i < 8; i++)
    if (leases[i].pid == p->pid)
      leases[i].pid = 0;
  handles_close(p->handles);
  if (p->interactive) page_free(p->interactive);
  if (p->pd)
    paging_user_destroy(p->pd);
  if (p->kernel_stack)
    page_free(p->kernel_stack);
  /* Publish only after execution resources and owned handles are gone. */
  if (p->completion_object) {
    int object = (int)p->completion_object - 1;
    int slot = objects[object].value;
    completions[slot].result = (ProcessCompletion){sizeof(ProcessCompletion), 1,
        p->pid, p->reason, p->reason == PROCESS_EXITED ? p->code : 0,
        p->fault_vector, {0, 0}};
    completions[slot].complete = 1;
    if (completions[slot].run && p->reason != PROCESS_EXITED)
      completions[slot].run->incomplete = 1;
    thread_unblock(p->completion_object, WAIT_COMPLETION);
    object_unref(object);
  }
  memset(p, 0, sizeof(*p));
}
static void* alloc_user_page(Process* p, u32 va, int write) {
  void* page = page_alloc();
  if (page && !paging_user_map(p->pd, va, page, write)) {
    page_free(page);
    return 0;
  }
  return page;
}
static void free_user_pages(Process* p, u32 start, u32 end) {
  for (u32 va = start; va < end; va += 4096) {
    void* page = paging_user_unmap(p->pd, va);
    if (page)
      page_free(page);
  }
}
static int anon_map(Process* p, u32 bytes) {
  if (!bytes || bytes > 0xffffffffu - 4095u)
    return S_INVALID;
  bytes = (bytes + 4095u) & ~4095u;
  AnonRegion* slot = 0;
  u32 used = 0;
  for (u32 i = 0; i < ANON_MAX_REGIONS; i++) {
    AnonRegion* region = &p->anon[i];
    used += region->bytes;
    if (!region->base && !slot)
      slot = region;
  }
  if (!slot || bytes > ANON_MAX_BYTES - used)
    return S_LIMIT;
  /* Reservations occupy virtual space even before there is a PTE. */
  u32 start = USER_DYNAMIC_BASE;
  for (;;) {
    u32 base = paging_user_find_gap(p->pd, start, USER_DYNAMIC_END, bytes);
    if (!base)
      return S_LIMIT;
    u32 next = base;
    for (u32 i = 0; i < ANON_MAX_REGIONS; i++) {
      const AnonRegion* region = &p->anon[i];
      u32 end = region->base + region->bytes;
      if (region->base < base + bytes && end > next)
        next = end;
    }
    if (next == base) {
      *slot = (AnonRegion){base, bytes};
      return (int)base;
    }
    start = next;
  }
}
static int anon_unmap(Process* p, u32 base) {
  if (base < USER_DYNAMIC_BASE || base >= USER_DYNAMIC_END || (base & 4095u))
    return S_INVALID;
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

int process_anon_page(u32* pd, u32 page, int populate) {
  if (!current || current->pd != pd || (page & 4095u) ||
      page < USER_DYNAMIC_BASE || page >= USER_DYNAMIC_END)
    return S_INVALID;
  for (u32 i = 0; i < ANON_MAX_REGIONS; i++) {
    const AnonRegion* region = &current->anon[i];
    if (page >= region->base && page - region->base < region->bytes) {
      if (!populate)
        return S_OK;
      if (paging_user_flags(pd, page) & PAGE_PRESENT)
        return S_INVALID;
      return alloc_user_page(current, page, 1) ? S_OK : S_NOMEM;
    }
  }
  return S_INVALID;
}

u32 process_user_limit(const u32* pd, u32 address) {
  if (!current || current->pd != pd || address < USER_BASE)
    return 0;
  if (current->abi == 2) {
    if (address < USER_DYNAMIC_END)
      return USER_DYNAMIC_END;
    return address < USER_V2_END ? USER_V2_END : 0;
  }
  if (address <= USER_V1_END)
    return USER_V1_END;
  return address >= USER_DYNAMIC_BASE && address < USER_DYNAMIC_END ? USER_DYNAMIC_END : 0;
}

static u32 user_stack_top(const Process* p, int thread) {
  return p->abi == 2 ? USER_V2_STACK(thread) : USER_V1_STACK(thread);
}
static int alloc_user_stack(Process* p, int thread) {
  u32 top = user_stack_top(p, thread), start = top - USER_STACK_BYTES;
  for (u32 va = start; va < top; va += 4096)
    if (!alloc_user_page(p, va, 1)) {
      free_user_pages(p, start, va);
      return 0;
    }
  return 1;
}
#ifdef SINO_SHELL_BRIDGE
/* Test-only mappings for the strict paging probe on the read-only boot disk. */
static int paging_probe_setup(Process* p, int exhaust) {
  u32 cross = USER_DYNAMIC_BASE + 0x3ff000u;
  u32 high = USER_DYNAMIC_END - 4096;
  if (paging_user_find_gap(p->pd, cross, cross + 8192, 8192) != cross ||
      !alloc_user_page(p, cross, 1) || !alloc_user_page(p, cross + 4096, 1) ||
      paging_user_find_gap(p->pd, cross, cross + 12288, 4096) != cross + 8192)
    return S_INVALID;

  u32 before = free_pages;
  if (alloc_user_page(p, cross, 0) || free_pages != before)
    return S_INVALID;
  if (!alloc_user_page(p, high, 0))
    return S_NOMEM;
  free_user_pages(p, high, high + 4096);
  if (free_pages != before)
    return S_INVALID;
  if (!exhaust)
    return alloc_user_page(p, high, 0) ? S_OK : S_NOMEM;

  /* Leave exactly one free page: the data allocation can succeed, but the
     new page table cannot. Keep the earlier mappings for launch rollback. */
  void** held = 0;
  while (free_pages > 1) {
    void** page = page_alloc();
    if (!page)
      break;
    *page = held;
    held = page;
  }
  int failed_cleanly = free_pages == 1 && !alloc_user_page(p, high, 0) &&
                       free_pages == 1 && !paging_user_flags(p->pd, high);
  while (held) {
    void** next = *held;
    page_free(held);
    held = next;
  }
  if (!failed_cleanly || free_pages != before)
    return S_INVALID;
  pr_debug("PAGING probe table OOM rollback passed\n");
  return S_NOMEM;
}
#endif
static void initial(Thread* t, u32 entry, u32 stack) {
  memset(t, 0, sizeof(*t));
  t->state = THREAD_ACTIVE;
  t->wake = ticks;
  t->saved.frame.eip = entry;
  t->saved.frame.cs = 0x1b;
  t->saved.frame.eflags = 0x202;
  t->saved.useresp = stack;
  t->saved.ss = 0x23;
}
/* A zero timeout waits indefinitely. Finite waits use signed tick comparisons. */
static inline void thread_block(Thread* t, u32 object, u32 op, u32 timeout) {
  if (timeout > 0x7fffffffu)
    timeout = 0x7fffffffu;
  t->wait_object = object;
  t->wait_op = op;
  t->wait_deadline = timeout ? ticks + timeout : 0;
  t->wait_timed = timeout != 0;
  t->state = THREAD_BLOCKED;
}
static void thread_finish_wait(Thread* t, int result, u32 now) {
  t->saved.frame.eax = (u32)result;
  t->wait_object = 0;
  t->wait_op = 0;
  t->wait_deadline = 0;
  t->wait_timed = 0;
  t->wake = now;
  t->state = THREAD_ACTIVE;
}
static int thread_wait_expired(const Thread* t, u32 now) {
  return t->wait_timed && (i32)(now - t->wait_deadline) >= 0;
}
/* Readiness allows a retry. The wait record survives until the call finishes. */
static void thread_unblock(u32 object, u32 op) {
  u32 now = ticks;
  for (int i = 0; i < NP; i++) {
    Process* p = &processes[i];
    if (p->state != 1)
      continue;
    for (int j = 0; j < NT; j++) {
      Thread* t = &p->thread[j];
      if (t->state == THREAD_BLOCKED && t->wait_object == object && t->wait_op == op) {
        t->wake = now;
        t->state = THREAD_ACTIVE;
      }
    }
  }
}
void process_pipe_wake(const Object* endpoint) {
  thread_unblock((u32)(endpoint - objects) + 1,
                 endpoint->type == OBJ_PIPE_WRITE ? WAIT_PIPE_READ : WAIT_PIPE_WRITE);
}
void process_terminal_wake(u32 object) {
  thread_unblock(object, WAIT_TERM_READ);
}
void process_interactive_wake(u32 object) {
  thread_unblock(object, WAIT_IX_RESULT);
}
int process_launch(const char* path, u32 caps) {
  return process_launch_args(path, caps, "");
}
static int launch_policy(const char*, u32, const char*, u32, int);
int process_launch_args(const char* path, u32 caps, const char* arguments) {
  return launch_policy(path, caps, arguments, PROCESS_COMPAT, -1);
}
int process_launch_strict(const char* path, const char* arguments) {
  return launch_policy(path, 0, arguments, PROCESS_STRICT, -1);
}
int process_workspace_frontend(u32 pid) {
  for (int i = 0; i < NP; i++)
    if (processes[i].state && processes[i].pid == pid)
      return processes[i].workspace_frontend;
  return 0;
}
static int desktop_run(u32 operation, const DesktopRun* request) {
  if (request->path[sizeof(request->path) - 1] ||
      request->argument[sizeof(request->argument) - 1])
    return S_INVALID;
  if (operation == DESKTOP_INSTALL)
    return process_launch_args("/boot/setup.snx", CAP_HOME, request->argument);
  if (operation == DESKTOP_NETTEST)
    return process_launch("/boot/nettest.snx", CAP_HOME | CAP_NETWORK);
  if (operation != DESKTOP_RUN_APP && operation != DESKTOP_RUN_STRICT)
    return S_INVALID;

  char path[VFS_PATH];
  if (request->path[0] != '/' || vfs_canonical(request->path, path) < 0)
    return S_INVALID;
  int node = vfs_lookup(path);
  if (node < 0)
    return S_NOTFOUND;
  int home = vfs_lookup("/home/user"), boot = vfs_lookup("/boot");
  int parent = nodes[node].parent;
  while (parent > 0 && parent != home && parent != boot)
    parent = nodes[parent].parent;
  u32 len = strlen(nodes[node].name);
  if (parent <= 0 || nodes[node].dir || len < 5 ||
      !streq(nodes[node].name + len - 4, ".snx") ||
      (parent == boot && (!nodes[node].readonly || nodes[node].boot < 0)))
    return S_DENIED;
  if (operation == DESKTOP_RUN_STRICT)
    return process_launch_strict(path, request->argument);
  return process_launch_args(path, CAP_HOME, request->argument);
}
int process_desktop_run(u32 shell, u32 operation, const DesktopRun* request) {
  if (operation == DESKTOP_KILL) {
    u32 target;
    memcpy(&target, request->path, sizeof(target));
    for (int i = 0; i < NP; i++)
      if (processes[i].state && processes[i].pid == target)
        return processes[i].desktop_owner == shell
                   ? process_kill(target, 0) : S_DENIED;
    return S_NOTFOUND;
  }
  int pid = desktop_run(operation, request);
  if (pid > 0)
    for (int i = 0; i < NP; i++)
      if (processes[i].state && processes[i].pid == (u32)pid) {
        processes[i].desktop_owner = shell;
        break;
      }
  return pid;
}
static int launch_policy(const char* path, u32 caps, const char* arguments,
                         u32 policy, int completion) {
  int n = vfs_lookup(path);
  if (n < 0)
    return S_NOTFOUND;
  if (nodes[n].dir || nodes[n].size < sizeof(NativeHeader) ||
      nodes[n].size > MAX_FILE)
    return S_INVALID; /* Scratch is reserved; this call never yields. */
  u8* bytes = (u8*)0xf00000;
  if (vfs_read(n, bytes, MAX_FILE) < 0)
    return S_IO;
  NativeHeader* h = (NativeHeader*)bytes;
  if (!native_header_valid(h, nodes[n].size))
    return S_INVALID;
  Process* p = 0;
  for (int i = 0; i < NP; i++)
    if (!processes[i].state) {
      p = &processes[i];
      break;
    }
  if (!p || next_pid >= 0x7fffffu || strlen(arguments) > 255)
    return S_LIMIT;
  memset(p, 0, sizeof(*p));
  p->abi = h->abi;
  p->code_end = USER_BASE + h->data_offset;
  p->pd = paging_user_create();
  p->kernel_stack = page_alloc();
  if (!p->pd || !p->kernel_stack) {
    release(p);
    return S_NOMEM;
  }
  for (u32 offset = 0; offset < h->memory_bytes; offset += 4096) {
    void* page = alloc_user_page(p, USER_BASE + offset, offset >= h->data_offset);
    if (!page) {
      release(p);
      return S_NOMEM;
    }
    u32 length = h->image_bytes > offset ? h->image_bytes - offset : 0;
    if (length > 4096)
      length = 4096;
    if (length)
      memcpy(page, bytes + offset, length);
  }
  u8* params = alloc_user_page(p, p->abi == 2 ? USER_V2_ARGUMENTS : USER_V1_ARGUMENTS, 0);
  if (!params) {
    release(p);
    return S_NOMEM;
  }
  memcpy(params, arguments, strlen(arguments) + 1);
  char executable[128];
  vfs_path(n, executable);
  memcpy(params + 256, executable, strlen(executable) + 1);
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
  RunData* run = completion >= 0 ? completions[objects[completion].value].run : 0;
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
                          : object_new(OBJ_STREAM, completion * 3 + i);
      if (stream < 0) { release(p); return stream; }
      if (!bound) objects[completion].refs++; /* Finite streams pin the run. */
      int handle = handle_open(p->handles, stream, i == 0 ? 1 : 2);
      if (handle < 0) {
        if (!bound) object_unref(stream);
        release(p); return handle;
      }
      if (bound) run->pending_pipe[i] = 0; /* Reference now belongs to child. */
      *handles[i] = (u32)handle;
    }
  }
  p->pid = next_pid++;
  p->completion_object = (u32)(completion + 1);
  if (completion >= 0)
    completions[objects[completion].value].result.pid = p->pid;
  p->policy = policy;
  p->input_frontend=policy==PROCESS_COMPAT && nodes[n].readonly && nodes[n].boot>=0 && (streq(executable,"/boot/term.snx")||streq(executable,"/boot/termqa.snx"));
  p->workspace_frontend=policy==PROCESS_COMPAT && nodes[n].readonly && nodes[n].boot>=0 && streq(executable,"/boot/term.snx");
  if (!next_pid)
    next_pid = 1;
  p->state = 1;
  p->caps = caps & 7; /* Boot utilities have no write, network or management
                         grant, regardless of launcher. */
  if (nodes[n].readonly && nodes[n].boot >= 0 &&
      (streq(executable, "/boot/calc.snx") ||
       streq(executable, "/boot/clock.snx") ||
       streq(executable, "/boot/term.snx")||streq(executable,"/boot/termqa.snx")))
    p->caps = 0;
  if (nodes[n].readonly && nodes[n].boot >= 0 &&
      streq(executable, "/boot/files.snx"))
    p->caps &= CAP_HOME;
  u32 j = 0;
  while (nodes[n].name[j] && j < 31) {
    p->name[j] = nodes[n].name[j];
    j++;
  }
  initial(&p->thread[0], USER_BASE + h->entry, user_stack_top(p, 0));
  pr_debug("PROCESS start ");
  pr_debug(p->name);
  pr_debug(" pid=");
  pr_debug_hex(p->pid);
  pr_debug("\n");
  return (int)p->pid;
}
/* Both old tracked launches and bounded runs use the same startup transaction. */
static int tracked_launch(Handle* owner, const char* path, const char* args, RunData* run) {
  int slot;
  for (slot = 0; slot < PROCESS_COMPLETIONS && completions[slot].used; slot++);
  if (slot == PROCESS_COMPLETIONS) { run_free(run); return S_LIMIT; }
  completions[slot].used = 1;
  completions[slot].run = run;
  int object = object_new(OBJ_PROCESS, slot);
  if (object < 0) { process_completion_release(slot); return object; }
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
int process_interactive_result(Handle* owner, u32 handle,
                               ProcessCompletion* out) {
  Object* o = handle_get(owner, handle, OBJ_PROCESS, PROCESS_WAIT);
  if (!o)
    return S_BADHANDLE;
  if (!completions[o->value].complete)
    return S_AGAIN;
  *out = completions[o->value].result;
  return 0;
}
int process_interactive_start(Handle* owner, const InteractiveSpec* spec,
                              int directory, void* session, u32* pid) {
  if (spec->size != sizeof(*spec) || spec->version != 1 ||
      spec->argc > RUN_ARGC || spec->bytes > RUN_ARGS ||
      (spec->rights & ~(FS_RIGHTS & ~FS_DELEGATE)) ||
      (!directory && spec->rights))
    return S_INVALID;
  u32 next = 0;
  for (u32 i = 0; i < spec->argc; i++) {
    if (spec->offsets[i] != next || next >= spec->bytes)
      return S_INVALID;
    while (next < spec->bytes && spec->argv[next])
      next++;
    if (next == spec->bytes)
      return S_INVALID;
    next++;
  }
  if (next != spec->bytes)
    return S_INVALID;
  int reader = spec->program == IX_READTOOLS || spec->program == IX_BOOTTOOLS;
  int boot_copy = spec->program == IX_BOOTCOPY;
  int writer = spec->program == IX_WRITETOOLS || boot_copy;
  const char* path = spec->program==IX_SHELL ? "/boot/gsh.snx" : spec->program==IX_COMMAND ? "/boot/gcmd.snx" : writer ? "/boot/writer.snx" : reader ? "/boot/tview.snx" : "/boot/brokerqa.snx";
  if (spec->program != 1 && spec->program != 2 && spec->program!=IX_SHELL && spec->program!=IX_COMMAND && !reader && !writer)
    return S_DENIED;
  if (spec->program != IX_SHELL && !writer &&
      (spec->rights & ~(FS_READ|FS_LOOKUP|FS_ENUM))) return S_DENIED;
  if (boot_copy &&
      spec->rights != (FS_READ | FS_LOOKUP | FS_WRITE | FS_CREATE))
    return S_DENIED;
  if (spec->program == IX_COMMAND && spec->rights)
    return S_DENIED;
  int n = vfs_lookup(path);
  if (n < 0 || !nodes[n].readonly || nodes[n].boot < 0)
    return S_DENIED;
  int grant = -1;
  if (spec->rights) {
    grant = filesys_acquire_directory(owner, directory, spec->rights);
    if (grant < 0)
      return grant;
  }
  CliInfo* cli = page_alloc();
  if (!cli) {
    if (grant >= 0)
      object_unref(grant);
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
    page_free(cli);
    if (grant >= 0)
      object_unref(grant);
    return h;
  }
  Handle* handles = process_terminal_target(owner, h, pid);
  Process* child = 0;
  for (int i = 0; i < NP; i++)
    if (handles == processes[i].handles) {
      child = &processes[i];
      break;
    }
  if (!child) {
    page_free(cli);
    if (grant >= 0)
      object_unref(grant);
    handle_close(owner, h);
    return S_IO;
  }
  child->interactive = cli;
  int status = 0;
  if (grant >= 0) {
    status = handle_open(handles, grant, spec->rights);
    if (status >= 0)
      child->directory = status;
    else
      object_unref(grant);
  }
  if (status >= 0 && boot_copy) {
    status = filesys_boot_reader(handles);
    if (status >= 0)
      child->boot_directory = status;
  }
  if (status >= 0)
    status = terminal_bind_interactive(session, handles, *pid, cli);
  if (status < 0) {
    release(child);
    handle_close(owner, h);
    return status;
  }
  return h;
}
void process_run_limits(RunLimits* out) {
  *out = (RunLimits){
      sizeof(RunLimits), 1,         RUN_COUNT, RUN_ARGC,   RUN_ARGS,
      RUN_BYTES,         RUN_BYTES, RUN_BYTES, RUN_IO_MAX, RUN_TIMEOUT_MAX};
}
int process_run_start(Handle* owner, const RunSpec* spec) {
  return process_run_start_directory(owner, spec, 0, 0);
}
int process_run_start_directory(Handle* owner, const RunSpec* spec, u32 directory, u32 rights) {
  return process_run_start_bound(owner, spec, directory, rights, 0);
}
int process_run_start_bound(Handle* owner, const RunSpec* spec, u32 directory, u32 rights,
                            const u32* streams) {
  if (spec->size != sizeof(RunSpec) || spec->version != 1 || spec->flags ||
      spec->reserved || spec->argc > RUN_ARGC || spec->argv_bytes > RUN_ARGS ||
      spec->input_bytes > RUN_BYTES || spec->timeout_ticks > RUN_TIMEOUT_MAX ||
      (!spec->argc && spec->argv_bytes)) return S_INVALID;
  if ((!directory && rights) || (directory && !rights)) return S_INVALID;
  if (streams && streams[0] && spec->input_bytes) return S_INVALID;
  if (run_count >= RUN_COUNT) return S_LIMIT;
  RunData* run = page_alloc();
  if (!run) return S_NOMEM;
  run_count++;
  for (int i = 0; i < 3; i++) {
    run->buffer[i] = page_alloc();
    if (!run->buffer[i]) { run_free(run); return S_NOMEM; }
  }
  /* Caller has bounded and validated these kernel/user input ranges. */
  const char* path = (const char*)spec->path;
  u32 path_size = 0;
  while (path_size < sizeof(run->path) && path[path_size]) path_size++;
  if (path_size == sizeof(run->path) || path[0] != '/' ||
      vfs_canonical(path, run->path) < 0) { run_free(run); return S_INVALID; }
  run->cli.size = sizeof(CliInfo); run->cli.version = 1;
  run->cli.argc = spec->argc; run->cli.argv_bytes = spec->argv_bytes;
  if (spec->argc) memcpy(run->cli.offsets, (void*)spec->offsets, spec->argc * 4);
  if (spec->argv_bytes) memcpy(run->cli.bytes, (void*)spec->argv, spec->argv_bytes);
  u32 next = 0;
  for (u32 i = 0; i < spec->argc; i++) {
    if (run->cli.offsets[i] != next || next >= spec->argv_bytes) { run_free(run); return S_INVALID; }
    while (next < spec->argv_bytes && run->cli.bytes[next]) next++;
    if (next == spec->argv_bytes) { run_free(run); return S_INVALID; }
    next++;
  }
  if (next != spec->argv_bytes) { run_free(run); return S_INVALID; }
  run->input_bytes = spec->input_bytes;
  if (spec->input_bytes) memcpy(run->buffer[0], (void*)spec->input, spec->input_bytes);
  run->timed = spec->timeout_ticks != 0;
  run->deadline = ticks + spec->timeout_ticks;
  if (directory) {
    int object = filesys_acquire_directory(owner, directory, rights);
    if (object < 0) { run_free(run); return object; }
    run->pending_directory = (u32)object + 1;
    run->directory_rights = rights;
  }
  if (streams) for (int i = 0; i < 3; i++) if (streams[i]) {
    int object = pipe_acquire(owner, streams[i], i != 0);
    if (object < 0) { run_free(run); return object; }
    run->pending_pipe[i] = (u32)object + 1;
  }
  return tracked_launch(owner, run->path, "", run);
}
static int run_result(const Object* object, RunResult* out) {
  int slot = object->value;
  RunData* run = completions[slot].run;
  if (!run) return S_INVALID;
  if (!completions[slot].complete) return S_AGAIN;
  *out = (RunResult){completions[slot].result, run->input_position,
                     run->length[0], run->length[1], run->incomplete};
  return 0;
}
int process_run_result(Handle* owner, u32 handle, RunResult* out) {
  Object* object = handle_get(owner, handle, OBJ_PROCESS, PROCESS_WAIT);
  return object ? run_result(object, out) : S_BADHANDLE;
}
int process_run_read(Handle* owner, u32 handle, u32 role, u32 offset, void* out, u32 cap) {
  RunResult result;
  int status = process_run_result(owner, handle, &result);
  if (status) return status;
  if (role < 1 || role > 2 || cap > RUN_IO_MAX) return S_INVALID;
  Object* object = handle_get(owner, handle, OBJ_PROCESS, PROCESS_WAIT);
  RunData* run = completions[object->value].run;
  if (offset > run->length[role - 1]) return S_INVALID;
  u32 bytes = run->length[role - 1] - offset;
  if (bytes > cap) bytes = cap;
  if (bytes) memcpy(out, run->buffer[role] + offset, bytes);
  return (int)bytes;
}
int process_cancel_owned(Handle* owner, u32 handle) {
  Object* object = handle_get(owner, handle, OBJ_PROCESS, PROCESS_CANCEL);
  if (!object) return S_BADHANDLE;
  if (completions[object->value].complete) return 0;
  for (int i = 0; i < NP; i++) {
    Process* p = &processes[i];
    if (p->state && p->pid == completions[object->value].result.pid &&
        p->completion_object == (u32)(object - objects) + 1) {
      if (p->state == 1) { p->state = 2; p->reason = PROCESS_CANCELED; p->code = 0; }
      return 0;
    }
  }
  return S_NOTFOUND;
}
Handle* process_terminal_target(Handle* owner,u32 h,u32* pid) {
  Object* o=handle_get(owner,h,OBJ_PROCESS,PROCESS_WAIT|PROCESS_CANCEL);
  if(!o||completions[o->value].complete)return 0;
  for(int i=0;i<NP;i++)if(processes[i].state==1 && processes[i].policy==PROCESS_STRICT &&
      processes[i].completion_object==(u32)(o-objects)+1){*pid=processes[i].pid;return processes[i].handles;}
  return 0;
}
Handle* process_interactive_handles(u32 pid) {
  for(int i=0;i<NP;i++)if(processes[i].state==1&&processes[i].pid==pid&&processes[i].interactive)return processes[i].handles;
  return 0;
}
int process_terminal_cancel(u32 pid) {
  for(int i=0;i<NP;i++)if(pid&&processes[i].pid==pid&&processes[i].state){
    if(processes[i].state==1){processes[i].state=2;processes[i].reason=PROCESS_CANCELED;processes[i].code=0;}return 0;
  }
  return S_NOTFOUND;
}
int process_kill(u32 pid, i32 code) {
  for (int i = 0; i < NP; i++)
    if (processes[i].state && processes[i].pid == pid) {
      if (processes[i].state == 2) return 0; /* First terminal event wins. */
      processes[i].state = 2;
      processes[i].code = code;
      processes[i].reason = PROCESS_KILLED;
      return 0;
    }
  return S_NOTFOUND;
}
void process_message(u32 pid, UiEvent e) {
  for (int i = 0; i < NP; i++)
    if (processes[i].state == 1 && processes[i].pid == pid) {
      Process* p = &processes[i];
      ui_queue_push(&p->input_queue,e,p->input_frontend);
    }
}
static int homepath(u32 address, char* path, int writing) {
  char raw[VFS_PATH];
  u32* user_pd = current->pd;
  UACCESS_TRY(uaccess_copy_string(user_pd, raw, address, sizeof(raw)));
  if (raw[0] != '/') return S_INVALID;
  if (vfs_canonical(raw, path) < 0)
    return S_INVALID;
  if (!writing)
    return 0;
  if (!(current->caps & CAP_HOME))
    return S_DENIED;
  const char* home = "/home/user/";
  for (u32 i = 0; home[i]; i++)
    if (path[i] != home[i])
      return S_DENIED;
  return 0;
}
static void leave(Frame* f) {
  Thread* t = &current->thread[current_thread];
  memcpy(&t->saved, f, sizeof(UserFrame));
  user_switch_out = 1;
  paging_activate_kernel();
}
static Object* pipe_handle(u32 handle, int reading) {
  return handle_get(current->handles, handle, reading ? OBJ_PIPE_READ : OBJ_PIPE_WRITE,
                    reading ? 1 : 2);
}
static int transfer_pipe(Object* endpoint, int reading, u32 buffer, u32 bytes) {
  if (bytes > RUN_IO_MAX) return S_INVALID;
  if (bytes) UACCESS_TRY(uaccess_prepare(current->pd, buffer, bytes, reading ? USER_WRITE : USER_READ));
  int result = pipe_transfer(endpoint, reading, (void*)buffer, bytes);
  if (result > 0)
    process_pipe_wake(endpoint);
  return result;
}
static int user_completion_result(Frame* f, const Object* object) {
  int run_query = f->eax == SYS_RUN_RESULT;
  if (!run_query && !object) return S_BADHANDLE;
  u32 bytes = run_query ? sizeof(RunResult) : sizeof(ProcessCompletion);
  if (f->edx == RESULT_BLOCK_FLAG ? f->esi > STREAM_WAIT_MAX : f->edx < bytes)
    return S_INVALID;
  UACCESS_TRY(uaccess_prepare(current->pd, f->ecx, bytes, USER_WRITE));
  if (!object) return S_BADHANDLE;
  if (run_query) {
    RunResult result;
    int status = run_result(object, &result);
    if (status == S_OK) memcpy((void*)f->ecx, &result, bytes);
    return status;
  }
  if (!completions[object->value].complete) return S_AGAIN;
  memcpy((void*)f->ecx, &completions[object->value].result, bytes);
  return S_OK;
}
static int retry_wait(Thread* t) {
  u32 now = ticks;
  int result = S_TIMEOUT;
  if (!thread_wait_expired(t, now)) {
    Frame* f = &t->saved.frame;
    if (t->wait_op == WAIT_COMPLETION) {
      Object* object = handle_get(current->handles, f->ebx, OBJ_PROCESS, PROCESS_WAIT);
      result = user_completion_result(f, object);
    } else if (t->wait_op == WAIT_TERM_READ || t->wait_op == WAIT_IX_RESULT) {
      result = terminal_call(current->handles, current->pid, current->policy,
                             &t->wait_terminal, current->pd);
    } else {
      int reading = t->wait_op == WAIT_PIPE_READ;
      Object* endpoint = pipe_handle(f->ebx, reading);
      result = endpoint ? transfer_pipe(endpoint, reading, f->ecx, f->edx & ~STREAM_BLOCK_FLAG)
                        : S_BADHANDLE;
    }
  }
  if (result == S_AGAIN) {
    /* Another waiter may have transferred first. Keep the original deadline. */
    t->state = THREAD_BLOCKED;
    return 0;
  }
  thread_finish_wait(t, result, now);
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
static int syscall(Frame* f) {
  u32 a = f->ebx, b = f->ecx, c = f->edx;
  u32* user_pd = current->pd;
  char path[VFS_PATH];
  int n, r;
  Object* o;
  if (current->policy == PROCESS_STRICT && !strict_call(f->eax))
    return f->eax >= SYS_EXIT && f->eax < SYS_CALL_END ? S_DENIED : S_INVALID;
  if (f->eax >= SYS_SOCKET && f->eax <= SYS_RECVFROM &&
      !(current->caps & CAP_NETWORK))
    return S_DENIED;
  if ((f->eax >= SYS_DIR_GRANT && f->eax <= SYS_FILE_COMMIT && f->eax != SYS_START_DIRECTORY) || f->eax==SYS_DIRECTORY_COMMIT)
    return filesys_call(current->handles, current->caps, f->eax, a, b, c,
                        user_pd);
  switch (f->eax) {
    case SYS_ANON_MAP:
      return b || c ? S_INVALID : anon_map(current, a);
    case SYS_ANON_UNMAP:
      return b || c ? S_INVALID : anon_unmap(current, a);
    case SYS_WORKSPACE:
      if(!current->workspace_frontend || a || b || c)return S_DENIED;
      return filesys_workspace(current->handles);
    case SYS_TERMINAL: {
      if (b!=sizeof(TerminalRequest) || c) return S_INVALID;
      TerminalRequest request;
      UACCESS_TRY(uaccess_copy_in(user_pd, &request, a, sizeof(request)));
      u32 wait_op = 0, timeout = request.extra;
      if (request.op == TERM_READ && request.value == TERM_READ_BLOCK) {
        wait_op = WAIT_TERM_READ;
        request.value = 0;
      } else if (request.op == IX_RESULT && (request.extra & IX_RESULT_BLOCK)) {
        wait_op = WAIT_IX_RESULT;
        timeout &= ~IX_RESULT_BLOCK;
      }
      if (wait_op) {
        if (timeout > STREAM_WAIT_MAX) return S_INVALID;
        request.extra = 0;
      }
      r = terminal_call(current->handles, current->pid, current->policy, &request, user_pd);
      if (wait_op && r == S_AGAIN) {
        o = handle_get(current->handles, request.handle, OBJ_TERM_CLIENT, 0);
        Thread* t = &current->thread[current_thread];
        t->wait_terminal = request;
        thread_block(t, (u32)(o - objects) + 1, wait_op, timeout);
        leave(f);
      }
      return r;
    }
    case SYS_PIPE_CREATE: {
      if (c < sizeof(PipePair)) return S_INVALID;
      UACCESS_TRY(uaccess_prepare(user_pd, b, sizeof(PipePair), USER_WRITE));
      PipePair pair; r = pipe_create(current->handles, a, &pair);
      if (!r) memcpy((void*)b, &pair, sizeof(pair));
      return r;
    }
    case SYS_PIPE_LIMITS: {
      if (c || b < sizeof(PipeLimits)) return S_INVALID;
      UACCESS_TRY(uaccess_prepare(user_pd, a, sizeof(PipeLimits), USER_WRITE));
      PipeLimits limits; pipe_limits(&limits); memcpy((void*)a, &limits, sizeof(limits)); return 0;
    }
    case SYS_STREAM_READY: {
      if (b || c) return S_INVALID;
      if(handle_get(current->handles,a,OBJ_TERM_CLIENT,0)){
        TerminalRequest q={TERM_READY,a,0,0,0,0};
        return terminal_call(current->handles,current->pid,current->policy,&q,user_pd);
      }
      o = handle_get(current->handles, a, OBJ_PIPE_READ, 1);
      if (!o) o = handle_get(current->handles, a, OBJ_PIPE_WRITE, 2);
      if (o) return pipe_ready(o);
      o = handle_get(current->handles, a, OBJ_STREAM, 0);
      if (!o || current->completion_object != (u32)(o->value / 3) + 1) return S_BADHANDLE;
      u32 role = (u32)o->value % 3;
      if (!handle_get(current->handles, a, OBJ_STREAM, role ? 2 : 1)) return S_BADHANDLE;
      RunData* run = completions[objects[o->value / 3].value].run;
      if (!role) return STREAM_READABLE | (run->input_position == run->input_bytes ? STREAM_EOF : 0);
      return run->length[role - 1] < RUN_BYTES ? STREAM_WRITABLE : STREAM_LIMIT;
    }
    case SYS_START_DIRECTORY: {
      if (a > 1 || b || c) return S_INVALID;
      u32 directory = a ? current->boot_directory : current->directory;
      if (!directory) return S_NOTFOUND;
      return handle_get(current->handles, directory, OBJ_DIRECTORY, 0)
                 ? (int)directory : S_BADHANDLE;
    }
    case SYS_RUN_START: {
      if (c || (b != sizeof(RunSpec) && b != sizeof(RunSpecV2) && b != sizeof(RunSpecV3))) return S_INVALID;
      UACCESS_TRY(uaccess_prepare(user_pd, a, b, USER_READ));
      RunSpec spec; memcpy(&spec, (void*)a, sizeof(spec));
      u32 directory = 0, rights = 0, streams[3] = {0, 0, 0};
      if (b >= sizeof(RunSpecV2)) {
        RunSpecV2 extended; memcpy(&extended, (void*)a, sizeof(extended));
        if (spec.size != b || spec.version != (b == sizeof(RunSpecV2) ? 2u : 3u) ||
            extended.reserved[0] || extended.reserved[1]) return S_INVALID;
        directory = extended.directory; rights = extended.directory_rights;
        spec.size = sizeof(spec); spec.version = 1;
      }
      if (b == sizeof(RunSpecV3)) {
        RunSpecV3 extended; memcpy(&extended, (void*)a, sizeof(extended));
        if (extended.reserved) return S_INVALID;
        streams[0] = extended.in; streams[1] = extended.out; streams[2] = extended.err;
      }
      if (spec.argc > RUN_ARGC || spec.argv_bytes > RUN_ARGS || spec.input_bytes > RUN_BYTES) return S_INVALID;
      UACCESS_TRY(uaccess_copy_string(user_pd, path, spec.path, sizeof(path)));
      if (spec.argc) UACCESS_TRY(uaccess_prepare(user_pd, spec.offsets, spec.argc * 4, USER_READ));
      if (spec.argv_bytes) UACCESS_TRY(uaccess_prepare(user_pd, spec.argv, spec.argv_bytes, USER_READ));
      if (spec.input_bytes) UACCESS_TRY(uaccess_prepare(user_pd, spec.input, spec.input_bytes, USER_READ));
      spec.path = (u32)path;
      return process_run_start_bound(current->handles, &spec, directory, rights, streams);
    }
    case SYS_RUN_LIMITS: {
      if (c || b < sizeof(RunLimits)) return S_INVALID;
      UACCESS_TRY(uaccess_prepare(user_pd, a, sizeof(RunLimits), USER_WRITE));
      RunLimits limits; process_run_limits(&limits); memcpy((void*)a, &limits, sizeof(limits)); return 0;
    }
    case SYS_CLI_INFO: {
      if (c || b < sizeof(CliInfo)) return S_INVALID;
      UACCESS_TRY(uaccess_prepare(user_pd, a, sizeof(CliInfo), USER_WRITE));
      if (current->interactive) { memcpy((void*)a,current->interactive,sizeof(CliInfo)); return 0; }
      if (!current->completion_object) return S_NOTFOUND;
      RunData* run = completions[objects[current->completion_object - 1].value].run;
      if (!run) return S_NOTFOUND;
      memcpy((void*)a, &run->cli, sizeof(CliInfo)); return 0;
    }
    case SYS_STREAM_READ:
    case SYS_STREAM_WRITE: {
      int reading = f->eax == SYS_STREAM_READ;
      int blocking = (c & STREAM_BLOCK_FLAG) != 0;
      if (!blocking && handle_get(current->handles, a, OBJ_TERM_CLIENT, 0)) {
        TerminalRequest q={reading?TERM_READ:TERM_WRITE,a,b,c,0,0};
        return terminal_call(current->handles,current->pid,current->policy,&q,user_pd);
      }
      o = pipe_handle(a, reading);
      if (o) {
        if (blocking && f->esi > STREAM_WAIT_MAX) return S_INVALID;
        r = transfer_pipe(o, reading, b, c & ~STREAM_BLOCK_FLAG);
        if (blocking && r == S_AGAIN) {
          thread_block(&current->thread[current_thread], pipe_peer(o),
                       reading ? WAIT_PIPE_READ : WAIT_PIPE_WRITE, f->esi);
          leave(f);
        }
        return r;
      }
      if (blocking) return S_BADHANDLE;
      o = handle_get(current->handles, a, OBJ_STREAM, reading ? 1 : 2);
      if (!o || current->completion_object != (u32)(o->value / 3) + 1) return S_BADHANDLE;
      u32 role = (u32)o->value % 3;
      if ((reading && role != 0) || (!reading && role == 0)) return S_BADHANDLE;
      if (!c) return 0;
      if (c > RUN_IO_MAX) return S_INVALID;
      UACCESS_TRY(uaccess_prepare(user_pd, b, c, (reading) ? USER_WRITE : USER_READ));
      RunData* run = completions[objects[o->value / 3].value].run;
      u32 bytes;
      if (reading) {
        bytes = run->input_bytes - run->input_position;
        if (bytes > c) bytes = c;
        if (bytes) memcpy((void*)b, run->buffer[0] + run->input_position, bytes);
        run->input_position += bytes;
      } else {
        bytes = RUN_BYTES - run->length[role - 1];
        if (bytes > c) bytes = c;
        if (bytes) memcpy(run->buffer[role] + run->length[role - 1], (void*)b, bytes);
        run->length[role - 1] += bytes;
        if (bytes < c) {
          run->incomplete = 1; current->state = 2;
          current->reason = PROCESS_OUTPUT_LIMIT; current->code = 0; leave(f);
          return bytes ? (int)bytes : S_LIMIT;
        }
      }
      return (int)bytes;
    }
    case SYS_RUN_RESULT:
    case SYS_PROCESS_RESULT: {
      o = handle_get(current->handles, a, OBJ_PROCESS, PROCESS_WAIT);
      r = user_completion_result(f, o);
      if (c == RESULT_BLOCK_FLAG && r == S_AGAIN) {
        thread_block(&current->thread[current_thread], (u32)(o - objects) + 1,
                     WAIT_COMPLETION, f->esi);
        leave(f);
      }
      return r;
    }
    case SYS_RUN_READ: {
      if (c != sizeof(RunRead)) return S_INVALID;
      RunRead read;
      UACCESS_TRY(uaccess_copy_in(user_pd, &read, b, sizeof(read)));
      if (read.capacity > RUN_IO_MAX) return S_INVALID;
      if (read.capacity) UACCESS_TRY(uaccess_prepare(user_pd, read.buffer, read.capacity, USER_WRITE));
      return process_run_read(current->handles, a, read.role, read.offset, (void*)read.buffer, read.capacity);
    }
    case SYS_SPAWN_TRACKED: {
      char args[256];
      if (c) return S_INVALID;
      r = homepath(a, path, 0);
      if (r) return r;
      if (b) {
        UACCESS_TRY(uaccess_copy_string(user_pd, args, b, sizeof(args)));
      } else args[0] = 0;
      return tracked_launch(current->handles, path, args, 0);
    }
    case SYS_PROCESS_CANCEL:
      o = handle_get(current->handles, a, OBJ_PROCESS, PROCESS_CANCEL);
      if (!o) return S_BADHANDLE;
      if (b || c) return S_INVALID;
      return process_cancel_owned(current->handles, a);
    case SYS_SYSTEM_INFO: {
      if (c || b < sizeof(SystemInfo)) return S_INVALID;
      UACCESS_TRY(uaccess_prepare(user_pd, a, sizeof(SystemInfo), USER_WRITE));
      SystemInfo info = {sizeof(SystemInfo), 1, SYSTEM_FEATURES, 100,
                         NP, NT, HANDLES, OBJECTS, MAX_FILE, SNX_V2_IMAGE_BYTES, 16384, 0};
      memcpy((void*)a, &info, sizeof(info));
      return 0;
    }
    case SYS_START_INFO: {
      if (c || b < sizeof(StartInfo)) return S_INVALID;
      UACCESS_TRY(uaccess_prepare(user_pd, a, sizeof(StartInfo), USER_WRITE));
      StartInfo info = {sizeof(StartInfo), 1, current->policy, current->caps,
                        SYSTEM_FEATURES, 0};
      memcpy((void*)a, &info, sizeof(info));
      return 0;
    }
    case SYS_LEASE: {
      char name[64];
      UACCESS_TRY(uaccess_copy_string(user_pd, name, a, sizeof(name)));
      if (!name[0]) return S_INVALID;
      int free = -1;
      for (int i = 0; i < 8; i++) {
        if (!leases[i].pid)
          free = i;
        else if (streq(leases[i].name, name)) {
          if (leases[i].pid != current->pid)
            return S_BUSY;
          if (b)
            leases[i].pid = 0;
          return 0;
        }
      }
      if (b)
        return S_NOTFOUND;
      if (free < 0)
        return S_LIMIT;
      leases[free].pid = current->pid;
      memcpy(leases[free].name, name, strlen(name) + 1);
      return 0;
    }
    case SYS_SENDTO:
    case SYS_RECVFROM:
      UACCESS_TRY(uaccess_prepare(user_pd, b, sizeof(SockMessage), (f->eax == SYS_RECVFROM) ? USER_WRITE : USER_READ));
      o = handle_get(current->handles, a, OBJ_SOCKET,
                     f->eax == SYS_SENDTO ? 2 : 1);
      if (!o)
        return S_BADHANDLE;
      {
        SockMessage* m = (SockMessage*)b;
        if (m->size > 1024) return S_INVALID;
        UACCESS_TRY(uaccess_prepare(user_pd, m->buffer, m->size, (f->eax == SYS_RECVFROM) ? USER_WRITE : USER_READ));
        return f->eax == SYS_SENDTO ? net_sendto(o->value, (const u8*)m->buffer,
                                                 m->size, &m->address)
                                    : net_recvfrom(o->value, (u8*)m->buffer,
                                                   m->size, &m->address);
      }
    case SYS_SOCKET:
      n = net_socket((int)a);
      if (n < 0)
        return n;
      r = object_new(OBJ_SOCKET, n);
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
      o = handle_get(current->handles, a, OBJ_SOCKET, 2);
      if (!o)
        return S_BADHANDLE;
      UACCESS_TRY(uaccess_prepare(user_pd, b, sizeof(SockAddress), USER_READ));
      return f->eax == SYS_BIND ? net_bind(o->value, (const SockAddress*)b)
                                : net_connect(o->value, (const SockAddress*)b);
    case SYS_LISTEN:
      o = handle_get(current->handles, a, OBJ_SOCKET, 2);
      return o ? net_listen(o->value) : S_BADHANDLE;
    case SYS_ACCEPT:
      o = handle_get(current->handles, a, OBJ_SOCKET, 1);
      if (!o)
        return S_BADHANDLE;
      n = net_accept(o->value);
      if (n < 0)
        return n;
      r = object_new(OBJ_SOCKET, n);
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
      o = handle_get(current->handles, a, OBJ_SOCKET,
                     f->eax == SYS_SEND ? 2 : 1);
      if (!o)
        return S_BADHANDLE;
      if (c > 1024) return S_INVALID;
      UACCESS_TRY(uaccess_prepare(user_pd, b, c, (f->eax == SYS_RECV) ? USER_WRITE : USER_READ));
      return f->eax == SYS_SEND ? net_send(o->value, (const u8*)b, c)
                                : net_recv(o->value, (u8*)b, c);
    case SYS_NET_READY:
      o = handle_get(current->handles, a, OBJ_SOCKET, 1);
      return o ? net_ready(o->value) : S_BADHANDLE;
    case SYS_NET_INFO:
      UACCESS_TRY(uaccess_prepare(user_pd, a, sizeof(NetStats), USER_WRITE));
      *(NetStats*)a = net_loop.stats;
      return 0;
    case SYS_PING:
      return net_ping(a, b);
    case SYS_CALENDAR: {
      if (c || b < sizeof(CalendarTime)) return S_INVALID;
      UACCESS_TRY(uaccess_prepare(user_pd, a, sizeof(CalendarTime), USER_WRITE));
      RtcDateTime date; int result=rtc_read(&date);
      if(!result){CalendarTime out={sizeof(out),1,date.year,date.month,date.day,date.hour,date.minute,date.second};memcpy((void*)a,&out,sizeof(out));}
      return result;
    }
    case SYS_TIME:
      if (b < sizeof(TimeOfDay)) return S_INVALID;
      UACCESS_TRY(uaccess_prepare(user_pd, a, sizeof(TimeOfDay), USER_WRITE));
      {
        TimeOfDay value;
        int result = rtc_time(&value);
        if (!result)
          *(TimeOfDay*)a = value;
        return result;
      }
    case SYS_OPEN_PATH:
      if (!(current->caps & CAP_HOME))
        return S_DENIED;
      r = homepath(a, path, 0);
      if (r)
        return r;
      return gui_open_path(path);
    case SYS_FILE_CLIP: {
      if (a > 2)
        return S_INVALID;
      if (a && !(current->caps & CAP_HOME))
        return S_DENIED;
      UACCESS_TRY(uaccess_prepare(user_pd, b, sizeof(FileClipboard), (a == 0) ? USER_WRITE : USER_READ));
      FileClipboard clip;
      memset(&clip, 0, sizeof(clip));
      if (a == 1) {
        UACCESS_TRY(uaccess_copy_string(user_pd, clip.path, b, sizeof(clip.path)));
        clip.cut = ((const FileClipboard*)b)->cut;
        if (clip.cut > 1)
          return S_INVALID;
        r = homepath(b, path, clip.cut != 0);
        if (r)
          return r;
        memcpy(clip.path, path, strlen(path) + 1);
      }
      r = gui_file_clip((int)a, &clip);
      if (!r && !a)
        memcpy((void*)b, &clip, sizeof(clip));
      return r;
    }
    case SYS_DROP_ZONES:
      o = handle_get(current->handles, a, OBJ_WINDOW, 2);
      if (!o)
        return S_BADHANDLE;
      if (!(current->caps & CAP_HOME))
        return S_DENIED;
      if (c > DROP_ZONES) return S_INVALID;
      UACCESS_TRY(uaccess_prepare(user_pd, b, c * sizeof(UiDropZone), USER_READ));
      return gui_user_drop_zones(o->value, (const UiDropZone*)b, c);
    case SYS_WINDOW_SIZE:
      o = handle_get(current->handles, a, OBJ_WINDOW, 1);
      if (!o)
        return S_BADHANDLE;
      if (c < sizeof(WindowSize)) return S_INVALID;
      UACCESS_TRY(uaccess_prepare(user_pd, b, sizeof(WindowSize), USER_WRITE));
      return gui_user_size(o->value, (WindowSize*)b);
    case SYS_UI_LANGUAGE:
      return settings_language;
    case SYS_WINDOW_TITLE:
      o = handle_get(current->handles, a, OBJ_WINDOW, 2);
      if (!o)
        return S_BADHANDLE;
      {
        char title[64];
        UACCESS_TRY(uaccess_copy_string(user_pd, title, b, sizeof(title)));
        return gui_user_title(o->value, title);
      }
    case SYS_DUP:
      return handle_duplicate(current->handles, a, b);
    case SYS_WINDOW_POLICY:
      o = handle_get(current->handles, a, OBJ_WINDOW, 2);
      return o ? gui_user_policy(o->value, b) : S_BADHANDLE;
    case SYS_MOVE:
      r = homepath(a, path, 1);
      if (r)
        return r;
      {
        char dest[VFS_PATH];
        r = homepath(b, dest, 1);
        if (r)
          return r;
        r = vfs_move(path, dest);
        if (r < 0)
          return r == S_BUSY ? S_BUSY : S_IO;
        gui_files_changed();
        return 0;
      }
    case SYS_CLIP_GET:
      if (b > 16384) return S_INVALID;
      UACCESS_TRY(uaccess_prepare(user_pd, a, b, USER_WRITE));
      return gui_clip_read((u8*)a, b);
    case SYS_CLIP_SET:
      if (b > 16384) return S_INVALID;
      UACCESS_TRY(uaccess_prepare(user_pd, a, b, USER_READ));
      return gui_clip_write((const u8*)a, b);
    case SYS_EXIT:
      current->state = 2;
      current->reason = PROCESS_EXITED;
      current->code = (int)a;
      process_exits++;
      leave(f);
      return 0;
    case SYS_YIELD:
      current->thread[current_thread].wake = ticks + (a > 1000 ? 1000 : a);
      leave(f);
      return 0;
    case SYS_TICKS:
      return (int)ticks;
    case SYS_LOG:
      if (c > 1024) return S_INVALID;
      UACCESS_TRY(uaccess_prepare(user_pd, a, c, USER_READ));
      klog_write_bytes((const void*)a, c);
      return (int)c;
    case SYS_WINDOW: {
      char title[64];
      UACCESS_TRY(uaccess_copy_string(user_pd, title, a, sizeof(title)));
      if (b < 200 || b > 1000 || c < 120 || c > 714) return S_INVALID;
      int slot = -1;
      for (int i = 0; i < HANDLES; i++)
        if (!current->handles[i].generation) {
          slot = i;
          break;
        }
      if (slot < 0)
        return S_LIMIT;
      int obj = object_new(OBJ_WINDOW, -1);
      if (obj < 0)
        return obj;
      int id = gui_user_open(current->pid, title, (int)b, (int)c);
      if (id < 0) {
        objects[obj].refs = 0;
        return S_LIMIT;
      }
      objects[obj].value = id;
      return handle_open(current->handles, obj, 3);
    }
    case SYS_SCENE:
      o = handle_get(current->handles, a, OBJ_WINDOW, 2);
      if (!o)
        return S_BADHANDLE;
      UACCESS_TRY(uaccess_prepare(user_pd, b, 4, USER_READ));
      {
        u32 count = *(const u32*)b;
        if (count > UI_CANVAS_COMMANDS) return S_INVALID;
        UACCESS_TRY(uaccess_prepare(user_pd, b, 4 + count * sizeof(UiCommand), USER_READ));
      }
      return gui_user_scene(o->value, (const UiScene*)b);
    case SYS_EVENT:
      UACCESS_TRY(uaccess_prepare(user_pd, a, sizeof(UiEvent), USER_WRITE));
      return ui_queue_pop(&current->input_queue,(UiEvent*)a);
    case SYS_CLOSE:
      r = handle_close(current->handles, a);
      if (r == S_OK)
        for (int i = 0; i < NT; i++) {
          Thread* t = &current->thread[i];
          u32 handle = (t->wait_op == WAIT_TERM_READ || t->wait_op == WAIT_IX_RESULT)
                           ? t->wait_terminal.handle : t->saved.frame.ebx;
          if (t->wait_op && handle == a)
            thread_finish_wait(t, S_BADHANDLE, ticks);
        }
      return r;
    case SYS_READ:
      r = homepath(a, path, 0);
      if (r)
        return r;
      n = vfs_lookup(path);
      if (n < 0)
        return S_NOTFOUND;
      if (c > MAX_FILE) return S_INVALID;
      UACCESS_TRY(uaccess_prepare(user_pd, b, c, USER_WRITE));
      if (nodes[n].size > c)
        return S_LIMIT;
      return vfs_read(n, (u8*)b, c) < 0 ? S_IO : (int)nodes[n].size;
    case SYS_WRITE:
      r = homepath(a, path, 1);
      if (r)
        return r;
      if (c > MAX_FILE) return S_INVALID;
      UACCESS_TRY(uaccess_prepare(user_pd, b, c, USER_READ));
      n = vfs_lookup(path);
      if (n >= 0 && nodes[n].dir)
        return S_INVALID;
      int created = n < 0;
      if (created)
        n = vfs_create(path, 0, 0);
      if (n < 0)
        return S_IO;
      if (vfs_write(n, (const u8*)b, c, 0) < 0) {
        if (created)
          vfs_remove(path, 0);
        return S_IO;
      }
      gui_files_changed();
      return 0;
    case SYS_MKDIR:
      r = homepath(a, path, 1);
      if (r)
        return r;
      gui_files_changed();
      return vfs_create(path, 1, 1) < 0 ? S_IO : 0;
    case SYS_REMOVE:
      r = homepath(a, path, 1);
      if (r)
        return r;
      gui_files_changed();
      r = vfs_remove(path, (int)b);
      return r < 0 ? (r == S_BUSY ? S_BUSY : S_IO) : 0;
    case SYS_LIST:
      r = homepath(a, path, 0);
      if (r)
        return r;
      n = vfs_lookup(path);
      if (n < 0 || !nodes[n].dir)
        return S_NOTFOUND;
      if (c > VFS_NODES) return S_INVALID;
      UACCESS_TRY(uaccess_prepare(user_pd, b, c * sizeof(FileInfo), USER_WRITE));
      {
        u32 count = 0;
        FileInfo* out = (FileInfo*)b;
        for (int i = 1; i < VFS_NODES && count < c; i++)
          if (nodes[i].used && nodes[i].parent == n) {
            memcpy(out[count].name, nodes[i].name, 32);
            out[count].size = nodes[i].size;
            out[count++].dir = (u32)nodes[i].dir;
          }
        return (int)count;
      }
    case SYS_SPAWN:
      r = homepath(a, path, 0);
      if (r)
        return r;
      char args[256];
      if (b) {
        UACCESS_TRY(uaccess_copy_string(user_pd, args, b, sizeof(args)));
      } else
        args[0] = 0;
      return process_launch_args(path, current->caps & CAP_HOME,
                                 args); /* no implicit network grant */
    case SYS_KILL:
      if (a != current->pid && !(current->caps & CAP_MANAGE))
        return S_DENIED;
      return process_kill(a, (int)b);
    case SYS_PROCESSES:
      if (c > NP) return S_INVALID;
      UACCESS_TRY(uaccess_prepare(user_pd, b, c * sizeof(ProcessInfo), USER_WRITE));
      return process_list((ProcessInfo*)b, c);
    case SYS_SYNC:
      if (storage_sync())
        return 0;
      pr_debug("SYS_SYNC failed: ");
      pr_debug(storage_error);
      pr_debug("\n");
      return S_IO;
    case SYS_THREAD:
      if (a < USER_BASE + sizeof(NativeHeader) || a >= current->code_end ||
          paging_user_flags(current->pd, a) != (PAGE_PRESENT | PAGE_USER))
        return S_INVALID;
      for (int i = 1; i < NT; i++)
        if (current->thread[i].state == THREAD_UNUSED) {
          if (!alloc_user_stack(current, i))
            return S_NOMEM;
          initial(&current->thread[i], a, user_stack_top(current, i));
          return i;
        }
      return S_LIMIT;
    case SYS_THREAD_EXIT: {
      current->thread[current_thread].state = THREAD_UNUSED;
      int live = 0;
      for (int i = 0; i < NT; i++)
        if (current->thread[i].state != THREAD_UNUSED)
          live = 1;
      if (!live) {
        current->state = 2;
        current->reason = PROCESS_EXITED;
        current->code = 0;
      }
      u32 top = user_stack_top(current, current_thread);
      free_user_pages(current, top - USER_STACK_BYTES, top);
      leave(f);
      return 0;
    }
    case SYS_EVENT_CREATE:
      n = object_new(OBJ_EVENT, 0);
      if (n < 0)
        return n;
      r = handle_open(current->handles, n, 3);
      if (r < 0)
        objects[n].refs = 0;
      return r;
    case SYS_EVENT_SET:
      o = handle_get(current->handles, a, OBJ_EVENT, 2);
      if (!o)
        return S_BADHANDLE;
      o->value = !!b;
      return 0;
    case SYS_EVENT_WAIT:
      o = handle_get(current->handles, a, OBJ_EVENT, 1);
      if (!o)
        return S_BADHANDLE;
      return o->value ? 0 : S_AGAIN;
    default:
      return S_INVALID;
  }
}
int process_interrupt(Frame* f) {
  if ((f->cs & 3) != 3)
    return 0;
  if (!current)
    panic("Orphan user frame", f->eip, 0);
  if (f->vector < 32) {
    if (f->vector == 14 && (f->error & ~2u) == 4u) {
      u32 address;
      __asm__ volatile("mov %%cr2,%0" : "=r"(address));
      int status = process_anon_page(current->pd, address & ~4095u, 1);
      if (status == S_OK)
        return 1;
      if (status == S_NOMEM) {
        pr_debug("USER anonymous page OOM address=");
        pr_debug_hex(address);
        pr_debug("\n");
      }
    }
    process_faults++;
    current->code = -(int)f->vector;
    current->reason = PROCESS_FAULTED;
    current->fault_vector = f->vector;
    current->state = 2;
    pr_debug("USER fault contained pid=");
    pr_debug_hex(current->pid);
    pr_debug(" vector=");
    pr_debug_hex(f->vector);
    pr_debug("\n");
    leave(f);
    return 1;
  }
  if (f->vector == 0x80) {
    if (current->caps & CAP_NETWORK)
      net_poll();
    int r = syscall(f);
    f->eax = (u32)r;
    if (user_switch_out) {
      Thread* t = &current->thread[current_thread];
      /* A parked syscall keeps its number for the retry. */
      if (t->state != THREAD_BLOCKED)
        t->saved.frame.eax = (u32)r;
    }
    return 1;
  }
  if (f->vector == 32 && (i32)(ticks - quantum_end) >= 0)
    leave(f);
  return 0;
}
void process_step(void) {
  for (int i = 0; i < NP; i++) {
    Process* p = &processes[i];
    if (p->state == 1 && p->completion_object) {
      RunData* run = completions[objects[p->completion_object - 1].value].run;
      if (run && run->timed && (i32)(ticks - run->deadline) >= 0) {
        p->state = 2; p->reason = PROCESS_TIMEOUT; p->code = 0;
      }
    }
  }
  for (int i = 0; i < NP; i++)
    if (processes[i].state == 2)
      release(&processes[i]);
  u32 now = ticks;
  for (int i = 0; i < NP; i++) {
    Process* p = &processes[i];
    if (p->state != 1)
      continue;
    for (int j = 0; j < NT; j++) {
      Thread* t = &p->thread[j];
      if (thread_wait_expired(t, now))
        thread_finish_wait(t, S_TIMEOUT, now);
    }
  }
  for (int i = 0; i < NP * NT; i++) {
    int which = cursor++ % (NP * NT);
    Process* p = &processes[which / NT];
    Thread* t = &p->thread[which % NT];
    if (p->state != 1 || t->state != THREAD_ACTIVE || (i32)(ticks - t->wake) < 0)
      continue;
    current = p;
    current_thread = which % NT;
    user_switch_out = 0;
    cli();
    paging_activate(p->pd);
    int ready = !t->wait_op || retry_wait(t);
    if (ready) {
      quantum_end = ticks + 2;
      tss[1] = (u32)p->kernel_stack + 4096;
      process_switches++;
      user_enter(&t->saved);
    }
    paging_activate_kernel();
    current = 0;
    sti();
    if (ready)
      break;
  }
}
void process_tests(void) {
  pr_debug("PROCESS address spaces initialized (8 processes x 4 threads)\n");
}

int process_list(ProcessInfo* out, u32 cap) {
  u32 count = 0;
  for (int i = 0; i < NP && count < cap; i++)
    if (processes[i].state) {
      Process* p = &processes[i];
      ProcessInfo* v = &out[count++];
      memset(v, 0, sizeof(*v));
      v->pid = p->pid;
      v->state = p->state;
      v->exit_code = p->code;
      memcpy(v->name, p->name, 32);
      for (int j = 0; j < NT; j++)
        if (p->thread[j].state != THREAD_UNUSED)
          v->threads++;
      v->pages = paging_user_count(p->pd);
    }
  return (int)count;
}

int process_alive(u32 pid) {
  for (int i = 0; i < NP; i++)
    if (processes[i].pid == pid && processes[i].state == 1)
      return 1;
  return 0;
}
