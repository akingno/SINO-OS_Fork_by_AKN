/* Trusted text device owner. Shell commands still run as strict Ring 3 jobs. */
#include "interactive.h"
#include "terminal_edit_model.h"
static TerminalModel model;
static TerminalEditView view;
static TerminalEdit edit;
static ConsoleFrame frame;
static InteractiveSpec intent;
static int control, directory, shell, child;

static int present(void) {
  frame.cursor = CONSOLE_COLUMNS * CONSOLE_ROWS;
  for (u32 y = 0; y < CONSOLE_ROWS; y++) {
    u32 row = terminal_model_row(&model, y);
    for (u32 x = 0; x < CONSOLE_COLUMNS; x++) {
      u32 cp = y < model.used ? model.cells[row][x] : ' ';
      frame.cells[y * CONSOLE_COLUMNS + x] = 0x0700u |
          (cp == TERMINAL_CONTINUATION ? ' ' : cp >= 32 && cp < 127 ? cp : '?');
    }
    u32 caret = view.active ? view.caret_line : model.line;
    u32 column = view.active ? view.caret_column : model.column;
    if (row == caret && y < model.used && column < CONSOLE_COLUMNS)
      frame.cursor = y * CONSOLE_COLUMNS + column;
  }
  return ConsolePresent(&frame);
}
static void cleanup(void) {
  if (control > 0) Close(control); /* Hangup cancels owned root and worker. */
  if (child > 0) Close(child);
  if (shell > 0) Close(shell);
  if (directory > 0) Close(directory);
  control = directory = shell = child = 0;
}
static int start(void) {
  memset(&view, 0, sizeof(view));
  terminal_model_init(&model, CONSOLE_COLUMNS, CONSOLE_ROWS);
  control = TerminalCreate(CONSOLE_COLUMNS, CONSOLE_ROWS);
  directory = WorkspaceDirectory();
  if (control < 0 || directory < 0) return 1;
  memset(&intent, 0, sizeof(intent));
  intent.size = sizeof(intent); intent.version = 1; intent.program = IX_SHELL;
  intent.argc = 1; copy(intent.argv, "shell-write");
  intent.bytes = strlen(intent.argv) + 1; intent.rights = FS_RIGHTS & ~FS_DELEGATE;
  shell = InteractiveStart(control, &intent, directory, 0);
  return shell < 0;
}
static int input(int key) {
  if (key == 3) return TerminalFeed(control, TERM_CANCEL, 0, 0, 0);
  if (key == 4) return TerminalFeed(control, TERM_EOF, 0, 0, 0);
  if (key == 13 || key == 8 || key == 9 || (key >= 32 && key < 127)) {
    char c = key == 13 ? '\n' : (char)key;
    return TerminalFeed(control, TERM_TEXT, &c, 1, 0);
  }
  return TerminalFeed(control, TERM_KEY, 0, 0, (u32)key);
}
int main(void) {
  int pending = 0, announced = 0;
  for (;;) {
    if (start()) { cleanup(); Log("CONSOLE SHELL start failed\n"); return 1; }
    for (;;) {
      int dirty = 0;
      if (child > 0) {
        int status = InteractiveFinish(control, child);
        if (!status) { Close(child); child = 0; }
        else if (status != S_AGAIN) { cleanup(); return 1; }
      }
      if (!child && InteractiveRequest(control, &intent) == S_OK) {
        int boot = intent.program == IX_BOOTTOOLS;
        int grant = intent.rights ? directory : 0;
        if (boot) grant = GrantDirectory("/boot", intent.rights | FS_DELEGATE);
        child = InteractiveStart(control, &intent, grant > 0 ? grant : 0, 1);
        if (boot && grant > 0) Close(grant);
        if (child < 0) child = 0; /* The broker publishes the launch error. */
        pending = 0;
      }
      for (int i = 0; i < 8; i++) {
        if (!pending) pending = ConsoleReadKey();
        if (pending < 0) { cleanup(); return 1; }
        if (!pending) break;
        int status = input(pending);
        if (status == S_AGAIN) break;
        if (status < 0) { cleanup(); return 1; }
        pending = 0;
      }
      u8 bytes[256];
      for (int i = 0; i < 4; i++) {
        int n = TerminalDrain(control, bytes, sizeof(bytes));
        if (n <= 0) break;
        terminal_model_write(&model, bytes, (u32)n); dirty = 1;
      }
      if (TerminalCall(TERM_EDIT_VIEW, control, &edit, sizeof(edit), 0, 0) == S_OK) {
        terminal_edit_apply(&model, &view, &edit); dirty = 1;
        if (TerminalCall(TERM_EDIT_ACK, control, 0, 0, edit.serial, 0) < 0) {
          cleanup(); return 1;
        }
      }
      if (dirty) {
        if (present() < 0) { cleanup(); return 1; }
        if (!announced && view.active) { Log("CONSOLE SHELL ready\n"); announced = 1; }
      }
      ProcessCompletion done;
      if (GetProcessResult(shell, &done) == S_OK) {
        cleanup(); pending = 0;
        Log("CONSOLE SHELL restarting\n"); Yield(2); break;
      }
      Yield(1);
    }
  }
}
