/* Independent headless text sessions. Frontends hold control, programs hold
   client handles. No ambient keyboard, screen or command-evaluator authority.
 */
#include "terminal.h"
#include "mm/uaccess.h"
#include "task/task.h"
#include "task/wait.h"
#include "sync/irq.h"
#include "lib/kstring.h"
#include "mm/kmalloc.h"
#include "mm/page.h"
#include "panic.h"
struct ClientIdentity {
  u32 index; /* Object index + 1, or zero when detached. */
  u32 generation;
};
typedef struct {
  u32 refs, width, height, mode, foreground;
  u32 foreground_object, control_object; /* Object index + 1. */
  u32 input_size, output_size, line_size, events;
  u8 input[1024], output[1024], line[256];
  TerminalEvent event[8];
  u32 shell, task, root, rights, request_id, phase, desktop;
  ClientIdentity shell_client;
  InteractiveSpec request;
  InteractiveResult result;
  TerminalEdit edit;
  bool closed, eof, edit_pending, editing;
} Terminal;
typedef struct {
  Terminal* terminal;
  u32 pid;
} Client;
static_assert(sizeof(Terminal) <= 4096, "terminal metadata fits page");
static u32 count;
static task::WaitKey keyboard_wait;

static void wake_console(const Terminal& terminal) {
  task::notify(terminal.control_object, task::WaitReason::CONSOLE);
}
terminal::ControlIdentity terminal::controlIdentity(void* session) {
  const auto& terminal = *static_cast<const Terminal*>(session);
  const u32 index = terminal.control_object;
  return index ? ControlIdentity{index, objects[index - 1].generation} : ControlIdentity{};
}
static const Object* live_control(u32 index, u32 generation) {
  if (!index || index > OBJECTS) {
    return nullptr;
  }
  const Object& object = objects[index - 1];
  return object.refs && object.type == ObjType::TERM_CONTROL && object.generation == generation
    ? &object : nullptr;
}
void terminal::completionPublished(ControlIdentity identity, u32 pid) {
  const Object* control = live_control(identity.index, identity.generation);
  if (control) {
    const auto& terminal = *reinterpret_cast<const Terminal*>(control->value);
    if (terminal.shell == pid || terminal.task == pid) {
      wake_console(terminal);
    }
  }
}
bool terminal::armConsoleKeyboard(task::WaitKey key) {
  sync::IrqGuard guard;
  if (keyboard_wait.object) {
    return false;
  }
  keyboard_wait = key;
  return true;
}
void terminal::disarmConsoleKeyboard(task::WaitKey key) {
  sync::IrqGuard guard;
  if (keyboard_wait.object == key.object && keyboard_wait.generation == key.generation) {
    keyboard_wait = {};
  }
}
void terminal::keyboardQueued() {
  if (live_control(keyboard_wait.object, keyboard_wait.generation)) {
    task::notify(keyboard_wait.object, task::WaitReason::CONSOLE);
  }
}
static bool input_ready(const Terminal& t, bool events) {
  return t.input_size || (!t.closed && t.eof) || (events && t.events);
}
void terminal::wakeWaiter(u32 object, task::WaitReason reason) {
  if (!object || object > OBJECTS || !objects[object - 1].refs ||
      objects[object - 1].type != ObjType::TERM_CLIENT) {
    return;
  }
  const Client* c = (const Client*)objects[object - 1].value;
  const Terminal* t = c->terminal;
  const bool writing = reason == task::WaitReason::TERM_WRITE;
  if (t->closed || t->foreground_object != object || (!writing && t->eof)) {
    task::notify(object, reason, true);
    return;
  }
  const bool ready = writing ? t->output_size < sizeof(t->output)
                             : input_ready(*t, reason == task::WaitReason::TERM_INPUT);
  if (ready) {
    task::notify(object, reason);
  }
}
static void wake_readers(Terminal* t) {
  terminal::wakeWaiter(t->foreground_object, task::WaitReason::TERM_READ);
  terminal::wakeWaiter(t->foreground_object, task::WaitReason::TERM_INPUT);
}
static void wake_events(Terminal* t) {
  terminal::wakeWaiter(t->foreground_object, task::WaitReason::TERM_INPUT);
}
static void wake_writers(Terminal* t) {
  terminal::wakeWaiter(t->foreground_object, task::WaitReason::TERM_WRITE);
}
static void wake_shell(Terminal* t, task::WaitReason reason) {
  const ClientIdentity identity = t->shell_client;
  if (!t->shell || !identity.index || identity.index > OBJECTS) {
    return;
  }
  const Object& object = objects[identity.index - 1];
  if (!object.refs || object.type != ObjType::TERM_CLIENT ||
      object.generation != identity.generation) {
    return;
  }
  const Client* client = (const Client*)object.value;
  if (client->terminal == t && client->pid == t->shell) {
    task::notify(identity.index, reason, true);
  }
}
static void remember_shell_client(Terminal* t, u32 object) {
  t->shell_client = {object, objects[object - 1].generation};
}
static void publish_result(Terminal* t) {
  t->phase = 3;
  wake_shell(t, task::WaitReason::IX_RESULT);
}
static void reset(Terminal* t) {
  if (t->foreground_object) {
    task::terminalForeground(t->foreground, false);
    task::notify(t->foreground_object, task::WaitReason::TERM_READ, true);
    task::notify(t->foreground_object, task::WaitReason::TERM_INPUT, true);
    task::notify(t->foreground_object, task::WaitReason::TERM_WRITE, true);
    task::notify(t->foreground_object, task::WaitReason::TERM_EDIT_ACK, true);
  }
  t->edit_pending=t->editing=0;
  t->mode = TERM_LINE | TERM_ECHO;
  t->foreground = t->foreground_object = 0;
  t->input_size = t->line_size = t->events = t->eof = 0;
  wake_console(*t);
}
static void set_foreground(Terminal* t, u32 pid, u32 object) {
  reset(t);
  t->foreground = pid;
  t->foreground_object = object;
  if (pid == t->shell) {
    remember_shell_client(t, object);
  }
  task::terminalForeground(pid, true);
}
static void unref(Terminal* t) {
  if (!--t->refs) {
    if (t->root)
      object_unref(t->root - 1);
    kpage_free(t);
    count--;
  }
}
void terminal::release(const Object& object) {
  if (object.type == ObjType::TERM_CLIENT) {
    Client* c = (Client*)object.value;
    Terminal* t = c->terminal;
    if (t->shell_client.index == (u32)(&object - objects) + 1 &&
        t->shell_client.generation == object.generation) {
      t->shell_client = {};
    }
    if (t->foreground == c->pid)
      reset(t);
    kfree(c);
    unref(t);
  } else {
    Terminal* t = (Terminal*)object.value;
    t->closed = 1;
    wake_readers(t);
    wake_writers(t);
    wake_shell(t, task::WaitReason::IX_RESULT);
    wake_shell(t, task::WaitReason::TERM_EDIT_ACK);
    disarmConsoleKeyboard({t->control_object, object.generation, task::WaitReason::CONSOLE});
    t->control_object = 0; // objectClosed already notified the console wait.
    t->shell_client = {};
    if (t->shell)
      process_terminal_cancel(t->shell);
    if (t->task)
      process_terminal_cancel(t->task);
    unref(t);
  }
}
static int dimensions(u32 w, u32 h) {
  return w && w <= 240 && h && h <= 100;
}
static int create(Handle* owner, u32 w, u32 h) {
  if (!dimensions(w, h))
    return S_INVALID;
  if (count >= 2)
    return S_LIMIT;
  Terminal* t = (Terminal*)kpage_alloc();
  if (!t)
    return S_NOMEM;
  t->refs = 1;
  t->width = w;
  t->height = h;
  reset(t);
  count++;
  int o = object_new(ObjType::TERM_CONTROL, (int)t);
  if (o < 0) {
    unref(t);
    return o;
  }
  t->control_object = (u32)o + 1;
  int handle = handle_open(owner, o, 1);
  if (handle < 0)
    object_unref(o);
  return handle;
}
static Client* client(Handle* owner, u32 h, u32 pid) {
  Object* o = handle_get(owner, h, ObjType::TERM_CLIENT, 0);
  if (!o || (!handle_get(owner, h, ObjType::TERM_CLIENT, 1) &&
             !handle_get(owner, h, ObjType::TERM_CLIENT, 8)))
    return 0;
  Client* c = (Client*)o->value;
  return c->pid == pid ? c : 0;
}
static int attach(Terminal* t, Handle* target, u32 pid) {
  if (t->closed)
    return S_CLOSED;
  int handle = 0;
  u32 object = 0;
  for (u32 i = 0; i < HANDLES; i++)
    if (target[i].generation) {
      Object* o = &objects[target[i].object];
      if (o->type != ObjType::TERM_CLIENT)
        continue;
      Client* c = (Client*)o->value;
      if (c->terminal != t)
        return S_BUSY;
      handle = (int)((target[i].cookie << 8) | (i + 1));
      object = target[i].object + 1;
      break;
    }
  if (!handle) {
    auto* c = static_cast<Client*>(kzalloc(sizeof(Client)));
    if (!c)
      return S_NOMEM;
    c->terminal = t;
    c->pid = pid;
    int o = object_new(ObjType::TERM_CLIENT, (int)c);
    if (o < 0) {
      kfree(c);
      return o;
    }
    t->refs++;
    handle = handle_open(target, o, 1);
    if (handle < 0) {
      object_unref(o);
      return handle;
    }
    object = (u32)o + 1;
  }
  set_foreground(t, pid, object);
  return handle;
}
static int pop(u8* from, u32* size, void* out, u32 bytes) {
  if (bytes > *size)
    bytes = *size;
  memcpy(out, from, bytes);
  *size -= bytes;
  for (u32 i = 0; i < *size; i++)
    from[i] = from[i + bytes];
  return (int)bytes;
}
static int event(Terminal* t, u32 type, u32 value, u32 w, u32 h) {
  if (t->events == 8)
    return S_AGAIN;
  t->event[t->events++] = (TerminalEvent){type, value, w, h};
  return 0;
}
static bool editing_input(const Terminal& t) {
  return t.editing && t.foreground == t.shell && !t.mode;
}
static int feed_status(const Terminal& t, u32 type, u32 bytes) {
  if (t.closed) {
    return S_CLOSED;
  }
  if (type == TERM_CANCEL) {
    return t.shell && t.foreground == t.shell && !t.editing && t.events == 8
      ? S_AGAIN : S_OK;
  }
  if (type == TERM_HANGUP || type == TERM_RESIZE) {
    return S_OK;
  }
  if (!t.foreground) {
    return S_AGAIN;
  }
  if (type == TERM_KEY) {
    return (t.mode & TERM_LINE) ? S_INVALID : t.events == 8 ? S_AGAIN : S_OK;
  }
  if (type == TERM_EOF) {
    return !t.eof && (t.events == 8 || t.line_size > sizeof(t.input) - t.input_size)
      ? S_AGAIN : S_OK;
  }
  if (t.eof) {
    return S_CLOSED;
  }
  return editing_input(t) && bytes > 8 - t.events ? S_AGAIN : S_OK;
}
struct TextInput {
  u8 line[256], committed[512], echo[256];
  u32 line_size, committed_size, echo_size;
};
static int prepare_text(const Terminal& t, const u8* text, u32 bytes, TextInput& input) {
  input.line_size = t.line_size;
  input.committed_size = input.echo_size = 0;
  memcpy(input.line, t.line, input.line_size);
  if (t.mode & TERM_LINE) {
    for (u32 i = 0; i < bytes; i++) {
      u8 ch = text[i];
      if (ch == 8 || ch == 127) {
        if (input.line_size) {
          input.line_size--;
          while (input.line_size && (input.line[input.line_size] & 0xc0) == 0x80) {
            input.line_size--;
          }
          input.echo[input.echo_size++] = 8;
        }
        continue;
      }
      if (input.line_size == sizeof(input.line)) {
        return S_LIMIT;
      }
      input.line[input.line_size++] = ch;
      input.echo[input.echo_size++] = ch;
      if (ch == '\n') {
        memcpy(input.committed + input.committed_size, input.line, input.line_size);
        input.committed_size += input.line_size;
        input.line_size = 0;
      }
    }
  } else {
    memcpy(input.committed, text, bytes);
    memcpy(input.echo, text, bytes);
    input.committed_size = input.echo_size = bytes;
  }
  if (input.committed_size > sizeof(t.input) - t.input_size ||
      ((t.mode & TERM_ECHO) && input.echo_size > sizeof(t.output) - t.output_size)) {
    return S_AGAIN;
  }
  return S_OK;
}
int terminal::consoleReady(const Object& control, const ConsoleWaitRequest& request,
                           u32 shellPid, u32 childPid, u32 completed) {
  const auto& t = *reinterpret_cast<const Terminal*>(control.value);
  if (t.closed) {
    return S_CLOSED;
  }
  if (!shellPid || t.shell != shellPid || (request.child && (!childPid || t.task != childPid))) {
    return S_DENIED;
  }
  const u32 interests = request.interests;
  if ((interests & completed) ||
      ((interests & CONSOLE_KEY) && key_pending()) ||
      ((interests & CONSOLE_OUTPUT) && (t.output_size || t.edit_pending)) ||
      ((interests & CONSOLE_REQUEST) && t.phase == 1)) {
    return S_OK;
  }
  if (interests & CONSOLE_INPUT_RETRY) {
    const bool text = request.input_type == TERM_TEXT;
    int status = feed_status(t, request.input_type, text ? 1u : 0u);
    if (status == S_OK && text && !editing_input(t)) {
      TextInput input;
      const u8 byte = (u8)request.input_value;
      status = prepare_text(t, &byte, 1, input);
    }
    if (status != S_AGAIN) {
      return S_OK;
    }
  }
  return S_AGAIN;
}
#ifdef SINO_SHELL_BRIDGE
void terminal::consoleSelfTest() {
  auto* t = static_cast<Terminal*>(kpage_alloc());
  if (!t) {
    panic("Console wait test allocation", 0, 0);
  }
  constexpr u32 shell = 1;
  t->shell = t->foreground = shell;
  const Object control{1, 1, ObjType::TERM_CONTROL, (int)t};
  ConsoleWaitRequest request{0, CONSOLE_INPUT_RETRY, 0, 0, TERM_EOF, 0};
  t->events = 8;
  t->input_size = sizeof(t->input);
  t->line_size = 1;
  const int full = consoleReady(control, request, shell, 0, 0);
  t->events--;
  const int eventSpace = consoleReady(control, request, shell, 0, 0);
  t->input_size--;
  const int bothSpaces = consoleReady(control, request, shell, 0, 0);
  t->mode = TERM_LINE | TERM_ECHO;
  t->output_size = sizeof(t->output);
  request.input_type = TERM_TEXT;
  request.input_value = 'x';
  const int fullEcho = consoleReady(control, request, shell, 0, 0);
  t->output_size--;
  const int echoSpace = consoleReady(control, request, shell, 0, 0);
  t->closed = true;
  const int closed = consoleReady(control, request, shell, 0, 0);
  kpage_free(t);
  if (full != S_AGAIN || eventSpace != S_AGAIN || bothSpaces != S_OK ||
      fullEcho != S_AGAIN || echoSpace != S_OK || closed != S_CLOSED) {
    panic("Console input wait predicate", full, closed);
  }
}
#endif
static int feed(Terminal* t, const TerminalRequest* r) {
  if (r->value < TERM_TEXT || r->value > TERM_RESIZE) {
    return S_INVALID;
  }
  const int status = feed_status(*t, r->value, r->bytes);
  if (status) {
    return status;
  }
  if (r->value == TERM_CANCEL) {
    if (t->shell && t->foreground == t->shell) {
      if (t->editing) {
        t->events = 0;
      }
      event(t, TERM_INTERRUPT, 0, 0, 0);
      t->input_size = t->line_size = t->eof = 0;
      wake_events(t);
      wake_console(*t);
      return S_OK;
    }
    return process_terminal_cancel(t->foreground);
  }
  if (r->value == TERM_HANGUP) {
    t->closed = 1;
    event(t, TERM_HANGUP, 0, 0, 0);
    wake_readers(t);
    wake_writers(t);
    wake_shell(t, task::WaitReason::IX_RESULT);
    wake_shell(t, task::WaitReason::TERM_EDIT_ACK);
    wake_console(*t);
    t->shell_client = {};
    return S_OK;
  }
  if (r->value == TERM_RESIZE) {
    u32 w = r->extra >> 16, h = r->extra & 65535;
    if (!dimensions(w, h)) {
      return S_INVALID;
    }
    const int queued = event(t, TERM_RESIZE, 0, w, h);
    if (queued == S_OK) {
      t->width = w;
      t->height = h;
      wake_events(t);
    }
    return queued;
  }
  if (r->value == TERM_KEY) {
    event(t, TERM_KEY, r->extra, 0, 0);
    wake_events(t);
    return S_OK;
  }
  if (r->value == TERM_EOF) {
    if (!t->eof) {
      memcpy(t->input + t->input_size, t->line, t->line_size);
      t->input_size += t->line_size;
      t->line_size = 0;
      t->eof = 1;
      event(t, TERM_EOF, 0, 0, 0);
      wake_readers(t);
    }
    return S_OK;
  }
  const auto* text = reinterpret_cast<const u8*>(r->buffer);
  if (editing_input(*t)) {
    for (u32 i = 0; i < r->bytes; i++) {
      event(t, TERM_TEXT, text[i], 0, 0);
    }
    if (r->bytes) {
      wake_events(t);
    }
    return S_OK;
  }
  TextInput input;
  const int prepared = prepare_text(*t, text, r->bytes, input);
  if (prepared) {
    return prepared;
  }
  memcpy(t->input + t->input_size, input.committed, input.committed_size);
  t->input_size += input.committed_size;
  memcpy(t->line, input.line, input.line_size);
  t->line_size = input.line_size;
  if ((t->mode & TERM_ECHO) && input.echo_size) {
    memcpy(t->output + t->output_size, input.echo, input.echo_size);
    t->output_size += input.echo_size;
    wake_console(*t);
  }
  if (input.committed_size) {
    wake_readers(t);
  }
  return S_OK;
}
int terminal_bind_interactive(void* session, Handle* handles, u32 pid,
                              CliInfo* cli) {
  Terminal* t = (Terminal*)session;
  if (t->closed)
    return S_CLOSED;
  auto* c = static_cast<Client*>(kzalloc(sizeof(Client)));
  if (!c)
    return S_NOMEM;
  c->terminal = t;
  c->pid = pid;
  int object = object_new(ObjType::TERM_CLIENT, (int)c);
  if (object < 0) {
    kfree(c);
    return object;
  }
  t->refs++;
  int h[4];
  u32 rights[4] = {1, 10, 12, 12};
  for (int i = 0; i < 4; i++) {
    if (i)
      objects[object].refs++;
    h[i] = handle_open(handles, object, rights[i]);
    if (h[i] < 0) {
      object_unref(object);
      for (int j = i - 1; j >= 0; j--)
        handle_close(handles, h[j]);
      return h[i];
    }
  }
  cli->in = h[1];
  cli->out = h[2];
  cli->err = h[3];
  set_foreground(t, pid, (u32)object + 1);
  return 0;
}
static int interactive_call(Terminal* t, Client* c, Object* control,
                            Handle* owner, u32 pid, u32 policy,
                            const TerminalRequest* r, u32* user_pd) {
  if (t->closed)
    return S_CLOSED;
  if (r->op == IX_SUBMIT) {
    if (!c || pid != t->shell || t->foreground != pid)
      return S_DENIED;
    if (t->phase)
      return S_BUSY;
    if (r->value || r->extra || r->bytes != sizeof(InteractiveSpec)) return S_INVALID;
    UACCESS_TRY(uaccess_prepare(user_pd, r->buffer, sizeof(InteractiveSpec), UserAccess::READ));
    const InteractiveSpec* q = (const InteractiveSpec*)r->buffer;
    if (q->size != sizeof(*q) || q->version != 1 || !q->id ||
        t->request_id == 0xffffffffu || q->id != t->request_id + 1 ||
        (q->program != 2 && q->program != IX_COMMAND &&
         q->program != IX_READTOOLS && q->program != IX_WRITETOOLS &&
         q->program != IX_BOOTTOOLS && q->program != IX_BOOTCOPY) ||
        q->argc > RUN_ARGC ||
        q->bytes > RUN_ARGS || (q->rights & ~t->rights))
      return S_INVALID;
    u32 n = 0;
    for (u32 i = 0; i < q->argc; i++) {
      if (q->offsets[i] != n || n >= q->bytes)
        return S_INVALID;
      while (n < q->bytes && q->argv[n])
        n++;
      if (n == q->bytes)
        return S_INVALID;
      n++;
    }
    if (n != q->bytes)
      return S_INVALID;
    memcpy(&t->request, q, sizeof(*q));
    t->request_id = q->id;
    t->phase = 1;
    wake_console(*t);
    return 0;
  }
  if (r->op == IX_RESULT) {
    if (!c || pid != t->shell)
      return S_DENIED;
    if (r->extra || r->bytes < sizeof(InteractiveResult)) return S_INVALID;
    UACCESS_TRY(uaccess_prepare(user_pd, r->buffer, sizeof(InteractiveResult), UserAccess::WRITE));
    if (r->value != t->request_id || !t->phase)
      return S_NOTFOUND;
    if (t->phase != 3)
      return S_AGAIN;
    memcpy((void*)r->buffer, &t->result, sizeof(t->result));
    t->phase = 0;
    return 0;
  }
  if (!control || policy == PROCESS_STRICT)
    return S_DENIED;
  if (r->op == IX_REQUEST) {
    if (r->value || r->extra || r->bytes < sizeof(InteractiveSpec)) return S_INVALID;
    UACCESS_TRY(uaccess_prepare(user_pd, r->buffer, sizeof(InteractiveSpec), UserAccess::WRITE));
    if (t->phase != 1)
      return S_AGAIN;
    memcpy((void*)r->buffer, &t->request, sizeof(t->request));
    return 0;
  }
  if (r->op == IX_FINISH) {
    if (r->buffer || r->bytes || r->extra || t->phase != 2)
      return S_INVALID;
    ProcessCompletion result;
    int status = process_interactive_result(owner, r->value, &result);
    if (status)
      return status;
    if (result.pid != t->task)
      return S_DENIED;
    Handle* shell = 0;
    /* Resolve the session-owned shell by its primary client, without PID
     * authority from the caller. */
    shell = process_interactive_handles(t->shell);
    if (!shell)
      return S_CLOSED;
    status = attach(t, shell, t->shell);
    if (status < 0)
      return status;
    t->task = 0;
    t->result = (InteractiveResult){sizeof(InteractiveResult), 1, t->request_id,
                                    0, result};
    publish_result(t);
    return 0;
  }
  if (r->op != IX_START || r->extra > 1 || r->bytes != sizeof(InteractiveSpec)) return S_INVALID;
  UACCESS_TRY(uaccess_prepare(user_pd, r->buffer, sizeof(InteractiveSpec), UserAccess::READ));
  InteractiveSpec* q = (InteractiveSpec*)kpage_alloc();
  if (!q) {
    if (r->extra == 1 && t->phase == 1) {
      memset(&t->result, 0, sizeof(t->result));
      t->result.size = sizeof(t->result);
      t->result.version = 1;
      t->result.id = t->request_id;
      t->result.status = S_NOMEM;
      publish_result(t);
    }
    return S_NOMEM;
  }
  memcpy(q, (void*)r->buffer, sizeof(*q));
  int role = r->extra, status;
  u32 child = 0;
  if ((!role && (t->shell || t->foreground ||
                 (q->program != 1 && q->program != IX_SHELL) || q->id)) ||
      (role && (!t->shell || t->phase != 1))) {
    kpage_free(q);
    return S_BUSY;
  }
  if (role) {
    const u8* a = (const u8*)q;
    const u8* b = (const u8*)&t->request;
    for (u32 i = 0; i < sizeof(*q); i++)
      if (a[i] != b[i]) {
        kpage_free(q);
        return S_INVALID;
      }
  }
  Object* directory = q->rights ? handle_get(owner, r->value, ObjType::DIRECTORY,
                                             q->rights | FS_DELEGATE)
                                : 0;
  int boot = q->program == IX_BOOTTOOLS;
  if ((q->rights && !directory) || (!q->rights && r->value) ||
      (boot && (!t->desktop || !directory || !filesys_is_boot_directory(directory))) ||
      (q->program == IX_BOOTCOPY && (!t->desktop || !(t->rights & FS_COMMIT))) ||
      (role && directory && !boot && (u32)(directory - objects) + 1 != t->root)) {
    status = S_DENIED;
  } else {
    // The shell submitted the command. A new session has no shell parent yet.
    status = task::interactiveStart(owner, q, r->value, t, role ? t->shell : 0, &child);
  }
  if (status >= 0) {
    if (role) {
      t->task = child;
      t->phase = 2;
    } else {
      t->shell = child;
      remember_shell_client(t, t->foreground_object);
      t->rights = q->rights;
      t->desktop = q->program == IX_SHELL && process_workspace_frontend(pid);
      if (directory) {
        directory->refs++;
        t->root = (u32)(directory - objects) + 1;
      }
    }
  } else if (role) {
    memset(&t->result, 0, sizeof(t->result));
    t->result.size = sizeof(t->result);
    t->result.version = 1;
    t->result.id = t->request_id;
    t->result.status = status;
    publish_result(t);
  }
  kpage_free(q);
  return status;
}

int terminal_call(Handle* owner, u32 pid, u32 policy, const TerminalRequest* r,
                  u32* user_pd) {
  if (r->op < TERM_CREATE || r->op > TERM_WAIT_INPUT)
    return S_INVALID;
  if (r->op == TERM_CREATE) {
    if (policy == PROCESS_STRICT)
      return S_DENIED;
    if (r->handle || r->buffer || r->bytes)
      return S_INVALID;
    return create(owner, r->value, r->extra);
  }
  if (r->op == TERM_CURRENT) {
    if (r->handle || r->buffer || r->bytes || r->value || r->extra)
      return S_INVALID;
    for (u32 i = 0; i < HANDLES; i++)
      if (owner[i].generation) {
        u32 h = (owner[i].cookie << 8) | (i + 1);
        if (handle_get(owner, h, ObjType::TERM_CLIENT, 1) && client(owner, h, pid))
          return (int)h;
      }
    return S_NOTFOUND;
  }
  Object* control = handle_get(owner, r->handle, ObjType::TERM_CONTROL, 1);
  Client* c = client(owner, r->handle, pid);
  Terminal* t = control ? (Terminal*)control->value : c ? c->terminal : 0;
  if (!t)
    return S_BADHANDLE;
  if (c && handle_get(owner, r->handle, ObjType::TERM_CLIENT, 8) &&
      r->op != TERM_INFO && r->op != TERM_READ && r->op != TERM_WRITE &&
      r->op != TERM_READY)
    return S_DENIED;
  if (r->op == TERM_DESKTOP_RUN) {
    if (!c || !t->desktop || !(t->rights & FS_COMMIT) ||
        pid != t->shell || pid != t->foreground || t->closed)
      return S_DENIED;
    if (r->extra || r->bytes != sizeof(DesktopRun)) return S_INVALID;
    UACCESS_TRY(uaccess_prepare(user_pd, r->buffer, sizeof(DesktopRun), UserAccess::READ));
    return process_desktop_run(pid, r->value, (const DesktopRun*)r->buffer);
  }
  if(r->op>=TERM_EDIT && r->op<=TERM_EDIT_ACK){
    if(r->op==TERM_EDIT_VIEW){
      if(!control)return S_DENIED;
      if (r->value || r->extra || r->bytes!=sizeof(TerminalEdit)) return S_INVALID;
      UACCESS_TRY(uaccess_prepare(user_pd, r->buffer, sizeof(TerminalEdit), UserAccess::WRITE));
      if(!t->edit_pending||t->output_size)return S_AGAIN;memcpy((void*)r->buffer,&t->edit,sizeof(t->edit));return 0;
    }
    if (r->op == TERM_EDIT_ACK) {
      if (!control) { return S_DENIED; }
      if (r->buffer || r->bytes || r->extra) { return S_INVALID; }
      if (!t->edit_pending || r->value != t->edit.serial) { return S_NOTFOUND; }
      t->edit_pending = false;
      wake_shell(t, task::WaitReason::TERM_EDIT_ACK);
      return S_OK;
    }
    if (!c || pid != t->shell || pid != t->foreground) { return S_DENIED; }
    if (t->closed) { return S_CLOSED; }
    if(r->op==TERM_EDIT_STATUS){if(r->buffer||r->bytes||r->value||r->extra)return S_INVALID;return t->edit_pending?S_AGAIN:0;}
    if (r->value || r->extra || r->bytes!=sizeof(TerminalEdit)) return S_INVALID;
    UACCESS_TRY(uaccess_prepare(user_pd, r->buffer, sizeof(TerminalEdit), UserAccess::READ));
    if(t->edit_pending)return S_AGAIN;
    const TerminalEdit *e=(const TerminalEdit*)r->buffer;
    if(e->bytes>sizeof(e->text)||e->cursor>e->bytes||e->action>2||e->serial)return S_INVALID;
    for(u32 i=0;i<e->bytes;i++)if((u8)e->text[i]<32||e->text[i]==127)return S_INVALID;
    if(e->cursor<e->bytes&&((u8)e->text[e->cursor]&0xc0)==0x80)return S_INVALID;
    const u32 serial = t->edit.serial + 1;
    memcpy(&t->edit, e, sizeof(t->edit));
    t->edit.serial = serial ? serial : 1;
    t->edit_pending = t->editing = true;
    wake_console(*t);
    return S_OK;
  }
  if (r->op >= IX_START && r->op <= IX_FINISH)
    return interactive_call(t, c, control, owner, pid, policy, r, user_pd);
  if (r->op == TERM_FOREGROUND) {
    if (t->shell)
      return S_DENIED;
    if (!control)
      return S_BADHANDLE;
    if (r->buffer || r->bytes || r->extra)
      return S_INVALID;
    u32 child;
    Handle* handles = process_terminal_target(owner, r->value, &child);
    return handles ? attach(t, handles, child) : S_BADHANDLE;
  }
  if (r->op == TERM_FEED) {
    if (!control)
      return S_BADHANDLE;
    if (r->bytes > 256 || (r->bytes && r->value != TERM_TEXT))
      return S_INVALID;
    if (r->bytes)
      UACCESS_TRY(uaccess_prepare(user_pd, r->buffer, r->bytes, UserAccess::READ));
    if (r->value != TERM_TEXT && r->buffer)
      return S_INVALID;
    if (r->value != TERM_KEY && r->value != TERM_RESIZE && r->extra)
      return S_INVALID;
    return feed(t, r);
  }
  if (r->op == TERM_INFO) {
    if (r->value || r->extra || r->bytes < sizeof(TerminalInfo)) return S_INVALID;
    UACCESS_TRY(uaccess_prepare(user_pd, r->buffer, sizeof(TerminalInfo), UserAccess::WRITE));
    TerminalInfo info = {sizeof(info),  1,         t->width, t->height, t->mode,
                         t->foreground, t->closed, 0};
    memcpy((void*)r->buffer, &info, sizeof(info));
    return 0;
  }
  if (r->op == TERM_DRAIN) {
    if (!control)
      return S_BADHANDLE;
  } else {
    if (!c)
      return S_BADHANDLE;
    if (t->foreground != pid)
      return S_DENIED;
  }
  if (r->op == TERM_MODE) {
    if (r->buffer || r->bytes || r->extra || (r->value & ~3u))
      return S_INVALID;
    if (t->closed)
      return S_CLOSED;
    if (t->line_size)
      return S_BUSY; /* Never silently discard a partial line. */
    t->mode = r->value;
    wake_console(*t);
    return 0;
  }
  if (r->value || r->extra)
    return S_INVALID;
  if (r->op == TERM_WAIT_INPUT) {
    if (r->buffer || r->bytes) {
      return S_INVALID;
    }
    return input_ready(*t, true) ? S_OK : t->closed ? S_CLOSED : S_AGAIN;
  }
  int role = handle_get(owner, r->handle, ObjType::TERM_CLIENT, 8) != 0;
  if (role && ((r->op == TERM_READ &&
                !handle_get(owner, r->handle, ObjType::TERM_CLIENT, 2)) ||
               (r->op == TERM_WRITE &&
                !handle_get(owner, r->handle, ObjType::TERM_CLIENT, 4))))
    return S_DENIED;
  if (r->op == TERM_READY) {
    if (r->buffer || r->bytes)
      return S_INVALID;
    u32 ready =
        (t->input_size || (!t->closed && t->eof) ? STREAM_READABLE : 0) |
        (!t->closed && t->output_size < 1024 ? STREAM_WRITABLE : 0) |
        (!t->closed && t->eof ? STREAM_EOF : 0) |
        (t->closed ? STREAM_HANGUP : 0);
    if (role && !handle_get(owner, r->handle, ObjType::TERM_CLIENT, 2))
      ready &= ~(STREAM_READABLE | STREAM_EOF);
    if (role && !handle_get(owner, r->handle, ObjType::TERM_CLIENT, 4))
      ready &= ~STREAM_WRITABLE;
    return (int)ready;
  }
  if (r->op == TERM_EVENT) {
    if (r->bytes < sizeof(TerminalEvent)) return S_INVALID;
    UACCESS_TRY(uaccess_prepare(user_pd, r->buffer, sizeof(TerminalEvent), UserAccess::WRITE));
    if (!t->events)
      return t->closed ? S_CLOSED : S_AGAIN;
    memcpy((void*)r->buffer, &t->event[0], sizeof(TerminalEvent));
    t->events--;
    for (u32 i = 0; i < t->events; i++)
      t->event[i] = t->event[i + 1];
    wake_console(*t);
    return 0;
  }
  if (r->op != TERM_READ && r->op != TERM_WRITE && r->op != TERM_DRAIN)
    return S_INVALID;
  if (r->bytes > 1024) return S_INVALID;
  if (r->bytes) UACCESS_TRY(uaccess_prepare(user_pd, r->buffer, r->bytes, (r->op != TERM_WRITE) ? UserAccess::WRITE : UserAccess::READ));
  if (!r->bytes)
    return 0;
  if (r->op == TERM_WRITE) {
    if (t->closed)
      return S_CLOSED;
    u32 n = 1024 - t->output_size;
    if (n > r->bytes)
      n = r->bytes;
    if (!n)
      return S_AGAIN;
    memcpy(t->output + t->output_size, (void*)r->buffer, n);
    t->output_size += n;
    wake_console(*t);
    return (int)n;
  }
  if (r->op == TERM_DRAIN) {
    if (!t->output_size) {
      return t->closed ? 0 : S_AGAIN;
    }
    const int drained = pop(t->output, &t->output_size, (void*)r->buffer, r->bytes);
    wake_writers(t);
    wake_console(*t);
    return drained;
  }
  if (t->input_size) {
    int result = pop(t->input, &t->input_size, (void*)r->buffer, r->bytes);
    wake_readers(t);
    wake_console(*t);
    return result;
  }
  return t->closed ? S_CLOSED : t->eof ? 0 : S_AGAIN;
}
