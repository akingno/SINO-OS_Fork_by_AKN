#include "klog.h"
/* Preemption only at user boundaries. Kernel/desktop remain non-preemptible. */
#include "process.h"
#include "object.h"
#include "vfs.h"
#include "gui.h"
#include "storage.h"
#include "net.h"
#include "settings.h"
#include "rtc.h"
#include "filesys.h"
#include "pipe.h"
#define NP 8
#define NT 4
typedef struct {
  Frame frame;
  u32 useresp, ss;
} UserFrame;
typedef struct {
  int state;
  u32 wake;
  UserFrame saved;
} Thread;
typedef struct {
  u32 pid, state, caps;
  int code;
  u32 *pd, *pt;
  void* kernel_stack;
  Thread thread[NT];
  Handle handles[HANDLES];
  UiEvent queue[16];
  u32 head, tail;
  char name[32];
  u32 policy;
  u32 completion_object; /* Object index + 1; zero for old launch paths. */
  u32 reason, fault_vector;
  u32 directory; /* Explicit startup grant; never a global working directory. */
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
static void cr3(u32 a) {
  __asm__ volatile("mov %0,%%cr3" ::"r"(a) : "memory");
}
static void release(Process* p) {
  for (int i = 0; i < 8; i++)
    if (leases[i].pid == p->pid)
      leases[i].pid = 0;
  handles_close(p->handles);
  if (p->pt) {
    for (u32 i = 0; i < 1024; i++)
      if (p->pt[i] & 1)
        page_free((void*)(p->pt[i] & ~4095u));
    page_free(p->pt);
  }
  if (p->pd)
    page_free(p->pd);
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
    object_unref(object);
  }
  memset(p, 0, sizeof(*p));
}
static int map_page(Process* p, u32 index, u32 flags) {
  void* page = page_alloc();
  if (!page)
    return 0;
  p->pt[index] = (u32)page | flags;
  return 1;
}
static void initial(Thread* t, u32 entry, u32 stack) {
  memset(t, 0, sizeof(*t));
  t->state = 1;
  t->wake = ticks;
  t->saved.frame.eip = entry;
  t->saved.frame.cs = 0x1b;
  t->saved.frame.eflags = 0x202;
  t->saved.useresp = stack;
  t->saved.ss = 0x23;
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
  if (h->magic != NATIVE_MAGIC || h->abi != 1 ||
      h->image_bytes != nodes[n].size || h->entry < 32 ||
      h->entry >= h->data_offset || h->data_offset > h->image_bytes ||
      (h->data_offset & 4095) || h->memory_bytes < h->image_bytes ||
      h->memory_bytes > USER_BYTES || h->reserved[0] || h->reserved[1])
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
  p->pd = page_alloc();
  p->pt = page_alloc();
  p->kernel_stack = page_alloc();
  if (!p->pd || !p->pt || !p->kernel_stack) {
    release(p);
    return S_NOMEM;
  }
  memcpy(p->pd, (void*)0x90000, 4096);
  p->pd[USER_BASE >> 22] = (u32)p->pt | 7;
  for (u32 i = 0; i < (h->memory_bytes + 4095) / 4096; i++) {
    if (!map_page(p, i, i * 4096 < h->data_offset ? 5 : 7)) {
      release(p);
      return S_NOMEM;
    }
    u32 offset = i * 4096,
        length = h->image_bytes > offset ? h->image_bytes - offset : 0;
    if (length > 4096)
      length = 4096;
    if (length)
      memcpy((void*)(p->pt[i] & ~4095u), bytes + offset, length);
  }
  if (!map_page(p, 48, 5)) {
    release(p);
    return S_NOMEM;
  }
  memcpy((void*)(p->pt[48] & ~4095u), arguments, strlen(arguments) + 1);
  char executable[128];
  vfs_path(n, executable);
  memcpy((void*)((p->pt[48] & ~4095u) + 256), executable,
         strlen(executable) + 1);
  for (u32 i = 60; i < 64; i++)
    if (!map_page(p, i, 7)) {
      release(p);
      return S_NOMEM;
    }
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
  if (!next_pid)
    next_pid = 1;
  p->state = 1;
  p->caps = caps & 7; /* Boot utilities have no write, network or management
                         grant, regardless of launcher. */
  if (nodes[n].readonly && nodes[n].boot >= 0 &&
      (streq(executable, "/boot/calc.snx") ||
       streq(executable, "/boot/clock.snx")))
    p->caps = 0;
  if (nodes[n].readonly && nodes[n].boot >= 0 &&
      streq(executable, "/boot/files.snx"))
    p->caps &= CAP_HOME;
  u32 j = 0;
  while (nodes[n].name[j] && j < 31) {
    p->name[j] = nodes[n].name[j];
    j++;
  }
  initial(&p->thread[0], USER_BASE + h->entry, USER_STACK(0));
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
void process_run_limits(RunLimits* out) {
  *out = (RunLimits){sizeof(RunLimits), 1, RUN_COUNT, RUN_ARGC, RUN_ARGS,
                     RUN_BYTES, RUN_BYTES, RUN_BYTES, RUN_IO_MAX, RUN_TIMEOUT_MAX};
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
int process_run_result(Handle* owner, u32 handle, RunResult* out) {
  Object* object = handle_get(owner, handle, OBJ_PROCESS, PROCESS_WAIT);
  if (!object) return S_BADHANDLE;
  int slot = object->value;
  RunData* run = completions[slot].run;
  if (!run) return S_INVALID;
  if (!completions[slot].complete) return S_AGAIN;
  *out = (RunResult){completions[slot].result, run->input_position,
                     run->length[0], run->length[1], run->incomplete};
  return 0;
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
      if (e.type == MSG_FILES_CHANGED) {
        int duplicate = 0;
        for (u32 j = p->tail; j != p->head; j = (j + 1) & 15)
          if (p->queue[j].type == e.type && p->queue[j].window == e.window)
            duplicate = 1;
        if (duplicate)
          return;
      }
      u32 previous = (p->head - 1) & 15;
      if (e.type == MSG_MOUSE_MOVE && p->head != p->tail &&
          p->queue[previous].type == MSG_MOUSE_MOVE) {
        p->queue[previous] = e;
        return;
      }
      u32 next = (p->head + 1) & 15;
      if (next == p->tail)
        return;
      p->queue[p->head] = e;
      p->head = next;
    }
}
/* Checks current user pages only; capability checks remain separate. */
static int user_access_ok(u32 address, u32 length, int write) {
  if (address < USER_BASE || length > 0x60000u ||
      address - USER_BASE > 0x60000u - length)
    return 0;
  if (!length)
    return 1;
  u32 a = (address - USER_BASE) >> 12,
      b = (address + length - 1 - USER_BASE) >> 12;
  for (; a <= b; a++)
    if ((current->pt[a] & (write ? 7u : 5u)) != (write ? 7u : 5u))
      return 0;
  return 1;
}
/* On failure dest may be partially written and need not be terminated. */
static int copy_user_string(u32 address, char* dest, u32 size) {
  for (u32 i = 0; i < size; i++) {
    if (!user_access_ok(address + i, 1, 0))
      return 0;
    dest[i] = *(const char*)(address + i);
    if (!dest[i])
      return 1;
  }
  return 0;
}
static int homepath(u32 address, char* path, int writing) {
  char raw[VFS_PATH];
  if (!copy_user_string(address, raw, sizeof(raw)) || raw[0] != '/')
    return S_INVALID;
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
  cr3(0x90000);
}
/* Allow local mechanisms, public queries and explicitly authorized objects.
 * New calls are denied until reviewed; old ambient paths cannot bypass this. */
static int strict_call(u32 call) {
  switch (call) {
    case SYS_EXIT: case SYS_YIELD: case SYS_LOG: case SYS_TICKS:
    case SYS_CLOSE: case SYS_DUP: case SYS_THREAD: case SYS_THREAD_EXIT:
    case SYS_EVENT_CREATE: case SYS_EVENT_SET: case SYS_EVENT_WAIT:
    case SYS_TIME: case SYS_UI_LANGUAGE:
    case SYS_SYSTEM_INFO: case SYS_START_INFO:
    case SYS_PROCESS_RESULT: case SYS_PROCESS_CANCEL:
    case SYS_CLI_INFO: case SYS_STREAM_READ: case SYS_STREAM_WRITE:
    case SYS_RUN_RESULT: case SYS_RUN_READ: case SYS_RUN_LIMITS:
    case SYS_START_DIRECTORY: case SYS_OPEN_AT:
    case SYS_FILE_READ_AT: case SYS_FILE_WRITE_AT: case SYS_FILE_STAT:
    case SYS_DIR_READ: case SYS_REMOVE_AT: case SYS_RENAME_AT:
    case SYS_FILE_STAGE: case SYS_FILE_PUBLISH: case SYS_FILE_COMMIT:
    case SYS_PIPE_CREATE: case SYS_STREAM_READY: case SYS_PIPE_LIMITS:
      return 1;
    default: return 0;
  }
}
static int syscall(Frame* f) {
  u32 a = f->ebx, b = f->ecx, c = f->edx;
  char path[VFS_PATH];
  int n, r;
  Object* o;
  if (current->policy == PROCESS_STRICT && !strict_call(f->eax))
    return f->eax >= SYS_EXIT && f->eax < SYS_CALL_END ? S_DENIED : S_INVALID;
  if (f->eax >= SYS_SOCKET && f->eax <= SYS_RECVFROM &&
      !(current->caps & CAP_NETWORK))
    return S_DENIED;
  if (f->eax >= SYS_DIR_GRANT && f->eax <= SYS_FILE_COMMIT &&
      f->eax != SYS_START_DIRECTORY)
    return filesys_call(current->handles, current->caps, f->eax, a, b, c,
                        user_access_ok, copy_user_string);
  switch (f->eax) {
    case SYS_PIPE_CREATE: {
      if (c < sizeof(PipePair) || !user_access_ok(b, sizeof(PipePair), 1)) return S_INVALID;
      PipePair pair; r = pipe_create(current->handles, a, &pair);
      if (!r) memcpy((void*)b, &pair, sizeof(pair));
      return r;
    }
    case SYS_PIPE_LIMITS: {
      if (c || b < sizeof(PipeLimits) || !user_access_ok(a, sizeof(PipeLimits), 1)) return S_INVALID;
      PipeLimits limits; pipe_limits(&limits); memcpy((void*)a, &limits, sizeof(limits)); return 0;
    }
    case SYS_STREAM_READY: {
      if (b || c) return S_INVALID;
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
    case SYS_START_DIRECTORY:
      if (a || b || c) return S_INVALID;
      if (!current->directory) return S_NOTFOUND;
      return handle_get(current->handles, current->directory, OBJ_DIRECTORY, 0)
                 ? (int)current->directory : S_BADHANDLE;
    case SYS_RUN_START: {
      if (c || (b != sizeof(RunSpec) && b != sizeof(RunSpecV2) && b != sizeof(RunSpecV3)) ||
          !user_access_ok(a, b, 0)) return S_INVALID;
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
      if (spec.argc > RUN_ARGC || spec.argv_bytes > RUN_ARGS || spec.input_bytes > RUN_BYTES ||
          !copy_user_string(spec.path, path, sizeof(path)) ||
          (spec.argc && !user_access_ok(spec.offsets, spec.argc * 4, 0)) ||
          (spec.argv_bytes && !user_access_ok(spec.argv, spec.argv_bytes, 0)) ||
          (spec.input_bytes && !user_access_ok(spec.input, spec.input_bytes, 0))) return S_INVALID;
      spec.path = (u32)path;
      return process_run_start_bound(current->handles, &spec, directory, rights, streams);
    }
    case SYS_RUN_LIMITS: {
      if (c || b < sizeof(RunLimits) || !user_access_ok(a, sizeof(RunLimits), 1)) return S_INVALID;
      RunLimits limits; process_run_limits(&limits); memcpy((void*)a, &limits, sizeof(limits)); return 0;
    }
    case SYS_CLI_INFO: {
      if (c || b < sizeof(CliInfo) || !user_access_ok(a, sizeof(CliInfo), 1)) return S_INVALID;
      if (!current->completion_object) return S_NOTFOUND;
      RunData* run = completions[objects[current->completion_object - 1].value].run;
      if (!run) return S_NOTFOUND;
      memcpy((void*)a, &run->cli, sizeof(CliInfo)); return 0;
    }
    case SYS_STREAM_READ:
    case SYS_STREAM_WRITE: {
      int reading = f->eax == SYS_STREAM_READ;
      o = handle_get(current->handles, a, reading ? OBJ_PIPE_READ : OBJ_PIPE_WRITE, reading ? 1 : 2);
      if (o) {
        if (c > RUN_IO_MAX || (c && !user_access_ok(b, c, reading))) return S_INVALID;
        return pipe_transfer(o, reading, (void*)b, c);
      }
      o = handle_get(current->handles, a, OBJ_STREAM, reading ? 1 : 2);
      if (!o || current->completion_object != (u32)(o->value / 3) + 1) return S_BADHANDLE;
      u32 role = (u32)o->value % 3;
      if ((reading && role != 0) || (!reading && role == 0)) return S_BADHANDLE;
      if (!c) return 0;
      if (c > RUN_IO_MAX || !user_access_ok(b, c, reading)) return S_INVALID;
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
    case SYS_RUN_RESULT: {
      if (c < sizeof(RunResult) || !user_access_ok(b, sizeof(RunResult), 1)) return S_INVALID;
      RunResult result; r = process_run_result(current->handles, a, &result);
      if (!r) memcpy((void*)b, &result, sizeof(result)); return r;
    }
    case SYS_RUN_READ: {
      if (c != sizeof(RunRead) || !user_access_ok(b, sizeof(RunRead), 0)) return S_INVALID;
      RunRead read; memcpy(&read, (void*)b, sizeof(read));
      if (read.capacity > RUN_IO_MAX ||
          (read.capacity && !user_access_ok(read.buffer, read.capacity, 1))) return S_INVALID;
      return process_run_read(current->handles, a, read.role, read.offset, (void*)read.buffer, read.capacity);
    }
    case SYS_SPAWN_TRACKED: {
      char args[256];
      if (c) return S_INVALID;
      r = homepath(a, path, 0);
      if (r) return r;
      if (b) {
        if (!copy_user_string(b, args, sizeof(args))) return S_INVALID;
      } else args[0] = 0;
      return tracked_launch(current->handles, path, args, 0);
    }
    case SYS_PROCESS_RESULT:
      o = handle_get(current->handles, a, OBJ_PROCESS, PROCESS_WAIT);
      if (!o) return S_BADHANDLE;
      if (c < sizeof(ProcessCompletion) || !user_access_ok(b, sizeof(ProcessCompletion), 1))
        return S_INVALID;
      if (!completions[o->value].complete) return S_AGAIN;
      memcpy((void*)b, &completions[o->value].result, sizeof(ProcessCompletion));
      return 0;
    case SYS_PROCESS_CANCEL:
      o = handle_get(current->handles, a, OBJ_PROCESS, PROCESS_CANCEL);
      if (!o) return S_BADHANDLE;
      if (b || c) return S_INVALID;
      return process_cancel_owned(current->handles, a);
    case SYS_SYSTEM_INFO: {
      if (c || b < sizeof(SystemInfo) || !user_access_ok(a, sizeof(SystemInfo), 1))
        return S_INVALID;
      SystemInfo info = {sizeof(SystemInfo), 1, SYSTEM_FEATURES, 100,
                         NP, NT, HANDLES, OBJECTS, MAX_FILE, USER_BYTES, 16384, 0};
      memcpy((void*)a, &info, sizeof(info));
      return 0;
    }
    case SYS_START_INFO: {
      if (c || b < sizeof(StartInfo) || !user_access_ok(a, sizeof(StartInfo), 1))
        return S_INVALID;
      StartInfo info = {sizeof(StartInfo), 1, current->policy, current->caps,
                        SYSTEM_FEATURES, 0};
      memcpy((void*)a, &info, sizeof(info));
      return 0;
    }
    case SYS_LEASE: {
      char name[64];
      if (!copy_user_string(a, name, sizeof(name)) || !name[0])
        return S_INVALID;
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
      if (!user_access_ok(b, sizeof(SockMessage), f->eax == SYS_RECVFROM))
        return S_INVALID;
      o = handle_get(current->handles, a, OBJ_SOCKET,
                     f->eax == SYS_SENDTO ? 2 : 1);
      if (!o)
        return S_BADHANDLE;
      {
        SockMessage* m = (SockMessage*)b;
        if (m->size > 1024 ||
            !user_access_ok(m->buffer, m->size, f->eax == SYS_RECVFROM))
          return S_INVALID;
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
      if (!user_access_ok(b, sizeof(SockAddress), 0))
        return S_INVALID;
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
      if (c > 1024 || !user_access_ok(b, c, f->eax == SYS_RECV))
        return S_INVALID;
      return f->eax == SYS_SEND ? net_send(o->value, (const u8*)b, c)
                                : net_recv(o->value, (u8*)b, c);
    case SYS_NET_READY:
      o = handle_get(current->handles, a, OBJ_SOCKET, 1);
      return o ? net_ready(o->value) : S_BADHANDLE;
    case SYS_NET_INFO:
      if (!user_access_ok(a, sizeof(NetStats), 1))
        return S_INVALID;
      *(NetStats*)a = net_loop.stats;
      return 0;
    case SYS_PING:
      return net_ping(a, b);
    case SYS_TIME:
      if (b < sizeof(TimeOfDay) || !user_access_ok(a, sizeof(TimeOfDay), 1))
        return S_INVALID;
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
      if (!user_access_ok(b, sizeof(FileClipboard), a == 0))
        return S_INVALID;
      FileClipboard clip;
      memset(&clip, 0, sizeof(clip));
      if (a == 1) {
        if (!copy_user_string(b, clip.path, sizeof(clip.path)))
          return S_INVALID;
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
      if (c > DROP_ZONES || !user_access_ok(b, c * sizeof(UiDropZone), 0))
        return S_INVALID;
      return gui_user_drop_zones(o->value, (const UiDropZone*)b, c);
    case SYS_WINDOW_SIZE:
      o = handle_get(current->handles, a, OBJ_WINDOW, 1);
      if (!o)
        return S_BADHANDLE;
      if (c < sizeof(WindowSize) || !user_access_ok(b, sizeof(WindowSize), 1))
        return S_INVALID;
      return gui_user_size(o->value, (WindowSize*)b);
    case SYS_UI_LANGUAGE:
      return settings_language;
    case SYS_WINDOW_TITLE:
      o = handle_get(current->handles, a, OBJ_WINDOW, 2);
      if (!o)
        return S_BADHANDLE;
      {
        char title[64];
        if (!copy_user_string(b, title, sizeof(title)))
          return S_INVALID;
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
      if (b > 16384 || !user_access_ok(a, b, 1))
        return S_INVALID;
      return gui_clip_read((u8*)a, b);
    case SYS_CLIP_SET:
      if (b > 16384 || !user_access_ok(a, b, 0))
        return S_INVALID;
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
      if (c > 1024 || !user_access_ok(a, c, 0))
        return S_INVALID;
      klog_write_bytes((const void*)a, c);
      return (int)c;
    case SYS_WINDOW: {
      char title[64];
      if (!copy_user_string(a, title, sizeof(title)) || b < 200 || b > 1000 ||
          c < 120 || c > 714)
        return S_INVALID;
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
      if (!user_access_ok(b, 4, 0))
        return S_INVALID;
      {
        u32 count = *(const u32*)b;
        if (count > UI_CANVAS_COMMANDS ||
            !user_access_ok(b, 4 + count * sizeof(UiCommand), 0))
          return S_INVALID;
      }
      return gui_user_scene(o->value, (const UiScene*)b);
    case SYS_EVENT:
      if (!user_access_ok(a, sizeof(UiEvent), 1))
        return S_INVALID;
      if (current->head == current->tail)
        return S_AGAIN;
      *(UiEvent*)a = current->queue[current->tail];
      current->tail = (current->tail + 1) & 15;
      return 0;
    case SYS_CLOSE:
      return handle_close(current->handles, a);
    case SYS_READ:
      r = homepath(a, path, 0);
      if (r)
        return r;
      n = vfs_lookup(path);
      if (n < 0)
        return S_NOTFOUND;
      if (c > MAX_FILE || !user_access_ok(b, c, 1))
        return S_INVALID;
      if (nodes[n].size > c)
        return S_LIMIT;
      return vfs_read(n, (u8*)b, c) < 0 ? S_IO : (int)nodes[n].size;
    case SYS_WRITE:
      r = homepath(a, path, 1);
      if (r)
        return r;
      if (c > MAX_FILE || !user_access_ok(b, c, 0))
        return S_INVALID;
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
      gui_files_changed();
      r = homepath(a, path, 1);
      if (r)
        return r;
      return vfs_create(path, 1, 1) < 0 ? S_IO : 0;
    case SYS_REMOVE:
      gui_files_changed();
      r = homepath(a, path, 1);
      if (r)
        return r;
      r = vfs_remove(path, (int)b);
      return r < 0 ? (r == S_BUSY ? S_BUSY : S_IO) : 0;
    case SYS_LIST:
      r = homepath(a, path, 0);
      if (r)
        return r;
      n = vfs_lookup(path);
      if (n < 0 || !nodes[n].dir)
        return S_NOTFOUND;
      if (c > VFS_NODES || !user_access_ok(b, c * sizeof(FileInfo), 1))
        return S_INVALID;
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
        if (!copy_user_string(b, args, sizeof(args)))
          return S_INVALID;
      } else
        args[0] = 0;
      return process_launch_args(path, current->caps & CAP_HOME,
                                 args); /* no implicit network grant */
    case SYS_KILL:
      if (a != current->pid && !(current->caps & CAP_MANAGE))
        return S_DENIED;
      return process_kill(a, (int)b);
    case SYS_PROCESSES:
      if (c > NP || !user_access_ok(b, c * sizeof(ProcessInfo), 1))
        return S_INVALID;
      {
        u32 count = 0;
        ProcessInfo* out = (ProcessInfo*)b;
        for (int i = 0; i < NP && count < c; i++)
          if (processes[i].state) {
            Process* p = &processes[i];
            ProcessInfo* v = &out[count++];
            memset(v, 0, sizeof(*v));
            v->pid = p->pid;
            v->state = p->state;
            v->exit_code = p->code;
            memcpy(v->name, p->name, 32);
            for (int j = 0; j < NT; j++)
              if (p->thread[j].state)
                v->threads++;
            for (int j = 0; j < 1024; j++)
              if (p->pt[j] & 1)
                v->pages++;
          }
        return (int)count;
      }
    case SYS_SYNC:
      if (storage_sync())
        return 0;
      pr_debug("SYS_SYNC failed: ");
      pr_debug(storage_error);
      pr_debug("\n");
      return S_IO;
    case SYS_THREAD:
      if (a < USER_BASE + 32 || a >= USER_BASE + USER_BYTES ||
          !user_access_ok(a, 1, 0) || current->pt[(a - USER_BASE) >> 12] & 2)
        return S_INVALID;
      for (int i = 1; i < NT; i++)
        if (!current->thread[i].state) {
          for (u32 j = 60 + i * 8; j < 64 + (u32)i * 8; j++)
            if (!map_page(current, j, 7)) {
              for (u32 k = 60 + i * 8; k < j; k++) {
                page_free((void*)(current->pt[k] & ~4095u));
                current->pt[k] = 0;
              }
              return S_NOMEM;
            }
          initial(&current->thread[i], a, USER_STACK(i));
          return i;
        }
      return S_LIMIT;
    case SYS_THREAD_EXIT:
      current->thread[current_thread].state =
          0; /* pages reclaimed with process; thread slots not reused until
                their stack is freed */
      int live = 0;
      for (int i = 0; i < NT; i++)
        if (current->thread[i].state)
          live = 1;
      if (!live) {
        current->state = 2;
        current->reason = PROCESS_EXITED;
        current->code = 0;
      }
      for (u32 j = 60 + current_thread * 8; j < 64 + (u32)current_thread * 8;
           j++) {
        page_free((void*)(current->pt[j] & ~4095u));
        current->pt[j] = 0;
      }
      leave(f);
      return 0;
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
    if (user_switch_out)
      current->thread[current_thread].saved.frame.eax = (u32)r;
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
  for (int i = 0; i < NP * NT; i++) {
    int which = cursor++ % (NP * NT);
    Process* p = &processes[which / NT];
    Thread* t = &p->thread[which % NT];
    if (p->state != 1 || t->state != 1 || (i32)(ticks - t->wake) < 0)
      continue;
    current = p;
    current_thread = which % NT;
    user_switch_out = 0;
    quantum_end = ticks + 2;
    tss[1] = (u32)p->kernel_stack + 4096;
    cli();
    cr3((u32)p->pd);
    process_switches++;
    user_enter(&t->saved);
    cr3(0x90000);
    current = 0;
    sti();
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
        if (p->thread[j].state)
          v->threads++;
      for (int j = 0; j < 1024; j++)
        if (p->pt[j] & 1)
          v->pages++;
    }
  return (int)count;
}

int process_alive(u32 pid) {
  for (int i = 0; i < NP; i++)
    if (processes[i].pid == pid && processes[i].state == 1)
      return 1;
  return 0;
}
