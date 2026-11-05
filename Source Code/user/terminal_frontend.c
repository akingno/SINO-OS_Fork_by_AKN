/* Trusted text frontend; stage-number launch switches are not public ABI. */
#ifndef MODE
#define MODE 0
#endif
#include "sdk.h"
#include "interactive.h"
#include "terminal_model.h"
#include "terminal_edit_model.h"
#include "line_edit.h"
static char paste_text[256];
static u32 paste_size, paste_position;
static TerminalEditView edit_view;
static TerminalEdit edit_frame;
int terminal_probe(void);

#define PENDING_INPUT 64u
_Static_assert(((TERMINAL_COLUMNS * 4 / 3 + 78) / 79) * TERMINAL_ROWS + 3 <=
                   UI_CANVAS_COMMANDS,
               "worst-case UTF-8 text fits one bounded scene");
typedef struct {
  u32 type, value, bytes;
  char text[4];
} PendingInput;
static PendingInput pending[PENDING_INPUT];
static u32 head, count;
static TerminalModel model;
static UiCanvas canvas;
static int window, control, task, finished, dirty = 1;
static int interactive_mode, child, directory, shell_mode;
static InteractiveSpec intent;
static const char *notice = "Ctrl+C cancel | Ctrl+D EOF | PgUp/PgDn scroll";
static const char input_full[] = "Input queue full; new input rejected";
static u32 width, height, resize_width, resize_height;

static int draw(void) {
  ui_canvas_begin(&canvas);
  ui_canvas_fill(&canvas, 2, 30, (int)width - 4, (int)height - 32, 0x171c25);
  u32 shown = model.used < model.rows ? model.used : model.rows;
  for (u32 y = 0; y < shown; y++) {
    u32 column = 0, row = terminal_model_row(&model, y);
    while (column < model.columns) {
      UiCommand *c = &canvas.commands[canvas.count];
      u32 next;
      int bytes = terminal_model_segment(&model, row, column, c->text, &next);
      if (bytes) {
        c->kind = UI_CELL_TEXT;
        c->x = 16 + (int)column * 7;
        c->y = 40 + (int)y * 18;
        c->color = 0xd6dce6;
        canvas.count++;
      }
      column = next;
    }
  }
  u32 caret_line = edit_view.active ? edit_view.caret_line : model.line;
  u32 caret_column = edit_view.active ? edit_view.caret_column : model.column;
  u32 distance =
      (model.line + TERMINAL_HISTORY - caret_line) % TERMINAL_HISTORY;
  int caret_row = (int)shown - 1 - (int)distance + (int)model.scroll;
  if (caret_row >= 0 && caret_row < (int)shown && caret_column < model.columns)
    ui_canvas_fill(&canvas, 16 + (int)caret_column * 7, 56 + caret_row * 18, 6,
                   1, 0xd6dce6);
  ui_canvas_text(&canvas, 16, (int)height - 26, 2, notice, 0x94ade3);
  return Scene(window, (const UiScene *)&canvas);
}
static void size(void) {
  WindowSize next;
  if (GetWindowSize(window, &next) < 0 ||
      (width == next.width && height == next.height))
    return;
  width = next.width;
  height = next.height;
  terminal_model_resize(&model, (width - 32) / 7, (height - 72) / 18);
  resize_width = model.columns;
  resize_height = model.rows;
  dirty = 1;
}
static void enqueue(const UiEvent *e, u32 mode) {
  PendingInput item = {TERM_TEXT, 0, 0, {0}};
  if (paste_size && !(e->type == MSG_KEY && e->key == 3)) {
    notice = "Paste busy: new input rejected";
    dirty = 1;
    return;
  }
  if (e->type == MSG_KEY && e->key == 22) {
    if (!shell_mode || child) {
      notice = "Paste is available at the Shell prompt";
      dirty = 1;
      return;
    }
    int n = api(SYS_CLIP_GET, (u32)paste_text, 255, 0);
    LineEditor check = {0};
    if (n < 0 || line_insert(&check, paste_text, (u32)n) < 0) {
      notice = "Paste rejected: single line, valid UTF-8, at most 255 bytes";
      dirty = 1;
      return;
    }
    paste_size = (u32)n;
    paste_position = 0;
    return;
  }
  if (e->type == MSG_TEXT)
    item.bytes = (u32)text_encode((u32)e->key, item.text);
  else if (e->type == MSG_KEY) {
    if (e->key == 0x149 || e->key == 0x151) {
      terminal_model_scroll(
          &model, e->key == 0x149 ? (int)model.rows : -(int)model.rows);
      dirty = 1;
      return;
    }
    if (e->key == 3) {
      int status = TerminalFeed(control, TERM_CANCEL, 0, 0, 0);
      count = 0;
      paste_size = paste_position = 0;
      if (status == S_AGAIN) {
        pending[head] = (PendingInput){TERM_CANCEL, 0, 0, {0}};
        count = 1;
      } else if (status < 0)
        notice = "Cancel failed";
      dirty = 1;
      return;
    }
    if (e->key == 12) {
      item.type = TERM_KEY;
      item.value = 12;
    } else if (e->key == 4)
      item.type = TERM_EOF;
    else if (e->key == 13 || e->key == 8 || e->key == 9 ||
             (e->key >= 32 && e->key < 127)) {
      item.text[0] = e->key == 13 ? '\n' : (char)e->key;
      item.bytes = 1;
    } else if (!(mode & TERM_LINE)) {
      item.type = TERM_KEY;
      item.value = (u32)e->key;
    } else
      return;
  } else
    return;
  if (item.type == TERM_TEXT && !item.bytes)
    return;
  if (count == PENDING_INPUT) {
    head = count = 0;
    pending[0] = (PendingInput){TERM_CANCEL, 0, 0, {0}};
    count = 1;
    if (notice != input_full && streq(Arguments(), "/qa"))
      Log("TERMINAL INPUT FULL\n");
    notice = input_full;
    dirty = 1;
    return;
  }
  pending[(head + count) % PENDING_INPUT] = item;
  count++;
}
static void feed(void) {
  for (int i = 0; i < 8 && count; i++) {
    PendingInput *item = &pending[head];
    int status = TerminalFeed(control, item->type, item->bytes ? item->text : 0,
                              item->bytes, item->value);
    if (status == S_AGAIN)
      return; /* Keep the identical item, in order. */
    if (status < 0) {
      notice = "Input rejected (closed, mode or line limit)";
      dirty = 1;
    }
    head = (head + 1) % PENDING_INPUT;
    count--;
  }
}
static void queue_paste(void) {
  for (u32 turn = 0;
       turn < 8 && paste_position < paste_size && count < PENDING_INPUT - 1;
       turn++) {
    PendingInput item = {TERM_TEXT, 0, 0, {0}};
    u8 first = (u8)paste_text[paste_position];
    u32 n = first < 128 ? 1 : first < 0xe0 ? 2 : first < 0xf0 ? 3 : 4;
    item.bytes = n;
    memcpy(item.text, paste_text + paste_position, n);
    paste_position += n;
    pending[(head + count) % PENDING_INPUT] = item;
    count++;
  }
  if (paste_position == paste_size && !count)
    paste_size = paste_position = 0;
}
static int cleanup(void) {
  if (task > 0) {
    CancelProcess(task);
    Close(task);
  }
  if (child > 0)
    Close(child);
  if (control > 0)
    Close(control);
  if (directory > 0)
    Close(directory);
  return 0;
}
int main(void) {
  StartInfo start;
  if (QueryStartInfo(&start) || start.caps)
    return 1;
  if (start.policy == PROCESS_STRICT) {
#if MODE == 1
    return terminal_probe();
#else
    return 2;
#endif
  }
  const char *args = Arguments();
  if (MODE == 1 && streq(args, "/selftest")) {
    terminal_model_init(&model, 120, 25);
    terminal_model_write(&model, (const u8 *)"ab\bX\n\xe4\xbd", 7);
    terminal_model_write(&model, (const u8 *)"\xa0", 1);
    if (model.cells[0][1] != 'X' || model.cells[1][0] != 0x4f60) {
      Log("TERMINAL MODEL SELFTEST FAIL\n");
      return 1;
    }
    Log("TERMINAL MODEL PASS\n");
    return 0;
  }
  int writable = streq(args, "/write");
  shell_mode = MODE == 0;
  if (MODE == 0 && *args && !writable) {
    Log("usage: term [/write]\n");
    return 2;
  }
  interactive_mode =
      shell_mode || streq(args, "/broker-test") || streq(args, "/broker-fault");
  control = TerminalCreate(120, 25);
  if (control < 0) {
    Log("TERMINAL FRONTEND unavailable\n");
    return 1;
  }
  window = Window(shell_mode         ? "SINO Shell"
                  : interactive_mode ? "Broker diagnostic"
                                     : "Terminal diagnostic",
                  872, 522);
  if (window < 0) {
    cleanup();
    return 1;
  }
  if (api(SYS_WINDOW_POLICY, (u32)window,
          WINDOW_CLOSE_REQUEST | WINDOW_DESKTOP_COMMANDS, 0) < 0) {
    cleanup();
    return 1;
  }
  terminal_model_init(&model, 120, 25);
  size();
  u32 offset = 0;
  const char *probe_mode = streq(args, "/fault-test") ? "ignore-hangup"
                           : streq(args, "/qa")       ? "qa"
                                                      : "interactive";
  RunSpec spec = {sizeof(spec),
                  1,
                  0,
                  (u32) "/boot/termqa.snx",
                  1,
                  strlen(probe_mode) + 1,
                  (u32)probe_mode,
                  (u32)&offset,
                  0,
                  0,
                  streq(args, "/fault-test") ? 100u : 30000u,
                  0};
  if (interactive_mode) {
    directory =
        writable ? WorkspaceDirectory()
                 : GrantDirectory(shell_mode ? "/home/user" : "/boot",
                                  FS_READ | FS_LOOKUP | FS_ENUM | FS_DELEGATE);
    if (directory < 0) {
      cleanup();
      return 1;
    }
    memset(&intent, 0, sizeof(intent));
    intent.size = sizeof(intent);
    intent.version = 1;
    intent.program = shell_mode ? IX_SHELL : 1;
    intent.argc = 1;
    copy(intent.argv, writable ? "shell-write" : "shell");
    intent.bytes = strlen(intent.argv) + 1;
    intent.rights = FS_READ | FS_LOOKUP | (shell_mode ? FS_ENUM : 0);
    if (writable)
      intent.rights = FS_RIGHTS & ~FS_DELEGATE;
    task = InteractiveStart(control, &intent, directory, 0);
  } else
    task = StartRun(&spec);
  if (task < 0 ||
      (!interactive_mode && TerminalForeground(control, task) < 0)) {
    Log("TERMINAL FRONTEND launch failed\n");
    cleanup();
    return 1;
  }
  int probe_ready = 0;
  for (;;) {
    if (interactive_mode && !finished) {
      int changed = 0;
      if (child > 0) {
        int status = InteractiveFinish(control, child);
        if (status == 0) {
          Close(child);
          child = 0;
          changed = 1;
        } else if (status != S_AGAIN) {
          return cleanup();
        }
      }
      if (!child && InteractiveRequest(control, &intent) == 0) {
        child = InteractiveStart(control, &intent,
                                 intent.rights ? directory : 0, 1);
        changed = 1;
        if (child < 0)
          child = 0;
      }
      if (changed) {
        count = 0;
        paste_size = paste_position = 0;
        UiEvent stale;
        for (int i = 0; i < 16 && Event(&stale) == 0; i++)
          if (stale.type == MSG_CLOSE)
            return cleanup();
          else if (stale.type == MSG_KEY && stale.key == 3)
            enqueue(&stale, 0);
      }
    }
    TerminalInfo info;
    if (TerminalGetInfo(control, &info) < 0) {
      cleanup();
      return 1;
    }
    /* Continue accepting close/cancel even when the bounded input queue is
     * full. Already accepted items retain their order across S_AGAIN. */
    for (int i = 0; i < 8; i++) {
      UiEvent event;
      if (Event(&event) != 0)
        break;
      if (event.type == MSG_INPUT_REJECTED) {
        UiEvent cancel = {MSG_KEY, 0, 0, 0, 3};
        enqueue(&cancel, info.mode);
        notice = "Input overflow: unfinished input canceled";
        dirty = 1;
        continue;
      }
      if (event.type == MSG_CLOSE)
        return cleanup();
      if (!finished)
        enqueue(&event, info.mode);
      else if (event.type == MSG_KEY && event.key == 27)
        return cleanup();
    }
    u8 bytes[256];
    for (int i = 0; i < 4; i++) {
      int n = TerminalDrain(control, bytes, sizeof(bytes));
      if (n <= 0)
        break;
      terminal_model_write(&model, bytes, (u32)n);
      dirty = 1;
      probe_ready = 1;
    }
    if (TerminalCall(TERM_EDIT_VIEW, control, &edit_frame, sizeof(edit_frame),
                     0, 0) == 0) {
      terminal_edit_apply(&model, &edit_view, &edit_frame);
      dirty = 1;
      if (TerminalCall(TERM_EDIT_ACK, control, 0, 0, edit_frame.serial, 0) < 0)
        return cleanup();
    }
    if (probe_ready && (streq(args, "/fault-test") ||
                        (streq(args, "/broker-fault") && child > 0)))
      __asm__ volatile("ud2");
    if (!finished) {
      queue_paste();
      feed();
      size();
      if (resize_width) {
        int status = TerminalFeed(control, TERM_RESIZE, 0, 0,
                                  (resize_width << 16) | resize_height);
        if (!status)
          resize_width = 0;
        else if (status != S_AGAIN) {
          cleanup();
          return 1;
        }
      }
      RunResult result;
      int completed = interactive_mode
                          ? GetProcessResult(task, &result.completion)
                          : GetRunResult(task, &result);
      if (completed == 0) {
        if (interactive_mode && child > 0) {
          CancelProcess(child);
          Close(child);
          child = 0;
        }
        /* Consume the final output before drawing a final, typed result. */
        for (int i = 0; i < 4; i++) {
          int n = TerminalDrain(control, bytes, sizeof(bytes));
          if (n <= 0)
            break;
          terminal_model_write(&model, bytes, (u32)n);
        }
        terminal_model_finish(&model);
        static const char *const messages[] = {
            "\nTask ended\n",    "\nTask exited ",      "\nTask faulted\n",
            "\nTask canceled\n", "\nTask terminated\n", "\nTask output limit\n",
            "\nTask timed out\n"};
        u32 reason = result.completion.reason;
        const char *message =
            messages[reason < sizeof(messages) / sizeof(messages[0]) ? reason
                                                                     : 0];
        terminal_model_write(&model, (const u8 *)message, strlen(message));
        if (result.completion.reason == PROCESS_EXITED) {
          char n[12];
          u32 code = (u32)result.completion.code;
          if (result.completion.code < 0) {
            terminal_model_write(&model, (const u8 *)"-", 1);
            code = 0u - code;
          }
          number(code, n);
          terminal_model_write(&model, (const u8 *)n, strlen(n));
          terminal_model_write(&model, (const u8 *)"\n", 1);
        }
        Close(task);
        task = 0;
        count = 0;
        finished = 1;
        edit_view.active = 0;
        dirty = 1;
        notice =
            shell_mode
                ? "Shell exited. Close this window."
                : "Probe complete. Close this window to release the session.";
      }
    } else
      size();
    if (dirty) {
      if (draw() < 0) {
        cleanup();
        return 1;
      }
      dirty = 0;
    }
    Yield(1);
  }
}
