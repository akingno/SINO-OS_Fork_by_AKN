#include "interactive.h"
#include "stream.h"
#include "gsh_parse.h"
#include "shell_commands.h"
#include "shell_banner.h"
static CliInfo cli;
static ShellWords words;
static int terminal, root, cwd_handle;
static char cwd[128], history[16][SHELL_LINE];
static u32 history_total, history_count, request_id;
static i32 last_code, last_start;
static u32 last_reason = PROCESS_EXITED;
static char input[64], line[SHELL_LINE];
static u32 position, available, length;
static void put(const char *text) {
  u32 written;
  if (StreamWriteAllFor(cli.out, text, strlen(text), 300, &written) < 0)
    Exit(1);
}
static void integer(i32 value) {
  u32 magnitude = (u32)value;
  if (value < 0) {
    put("-");
    magnitude = 0u - magnitude;
  }
  char text[12];
  number(magnitude, text);
  put(text);
}
static void prompt(void) {
  put("gsh:/");
  put(cwd);
  put(" $ ");
}
static void status(i32 code, u32 reason, i32 startup) {
  last_code = code;
  last_reason = reason;
  last_start = startup;
}
static int usage(u32 minimum, u32 maximum) {
  if (words.argc >= minimum && words.argc <= maximum)
    return 0;
  put("usage error\n");
  status(2, PROCESS_EXITED, 0);
  return -1;
}
static void external(u32 program, u32 rights) {
  InteractiveSpec request;
  memset(&request, 0, sizeof(request));
  request.size = sizeof(request);
  request.version = 1;
  request.program = program;
  request.rights = rights;
  if (request_id == 0xffffffffu) {
    put("request limit\n");
    status(126, 0, S_LIMIT);
    return;
  }
  request.id = request_id + 1;
  request.argc = words.argc;
  request.bytes = words.used;
  memcpy(request.offsets, words.offsets, sizeof(request.offsets));
  memcpy(request.argv, words.bytes, words.used);
  int error = InteractiveSubmit(terminal, &request);
  if (error < 0) {
    put("request rejected\n");
    status(126, 0, error);
    return;
  }
  request_id++;
  position = available; /* Foreground transfer discards queued typeahead. */
  InteractiveResult result;
  while ((error = InteractiveResultRead(terminal, request_id, &result)) ==
         S_AGAIN)
    Yield(1);
  if (error < 0)
    Exit(1);
  if (result.status < 0) {
    put("start failed ");
    integer(result.status);
    put("\n");
    status(126, 0, result.status);
    return;
  }
  u32 reason = result.completion.reason;
  i32 code = result.completion.code;
  if (reason != PROCESS_EXITED) {
    code = reason == PROCESS_CANCELED  ? 130
           : reason == PROCESS_FAULTED ? 128 + (i32)result.completion.vector
                                       : 137;
    put("task ended reason=");
    integer((i32)reason);
    put("\n");
  }
  status(code, reason, 0);
}
static void execute(void) {
  if (!length)
    return;
  if (history_total == 0xffffffffu)
    history_total = history_count;
  copy(history[history_total % 16], line);
  history_total++;
  if (history_count < 16)
    history_count++;
  if (shell_parse(line, length, &words)) {
    put("syntax error\n");
    status(2, PROCESS_EXITED, 0);
    return;
  }
  if (!words.argc)
    return;
  const ShellCommand *entry = shell_command(words.bytes + words.offsets[0]);
  const char *command = entry ? entry->name : words.bytes + words.offsets[0];
  if (entry) {
    ShellWords normalized = words;
    u32 used = 0;
    for (u32 i = 0; i < normalized.argc; i++) {
      const char *arg = i ? normalized.bytes + normalized.offsets[i] : command;
      words.offsets[i] = used;
      copy(words.bytes + used, arg);
      used += strlen(arg) + 1;
    }
    words.used = used;
  }
  if (streq(command, "status")) {
    if (usage(1, 1))
      return;
    put("status code=");
    integer(last_code);
    put(" reason=");
    integer((i32)last_reason);
    put(" start=");
    integer(last_start);
    put("\n");
    return;
  }
  if (streq(command, "exit")) {
    if (usage(1, 2))
      return;
    i32 code = last_code;
    if (words.argc == 2 &&
        (shell_integer(words.bytes + words.offsets[1], &code) || code < 0 ||
         code > 255)) {
      put("exit expects 0..255\n");
      status(2, PROCESS_EXITED, 0);
      return;
    }
    Exit(code);
  }
  if (streq(command, "help") || streq(command, "which")) {
    int which = streq(command, "which");
    if (usage(which ? 2 : 1, 2))
      return;
    if (words.argc == 2) {
      const ShellCommand *item = shell_command(words.bytes + words.offsets[1]);
      if (!item || item->kind == 4) {
        put(which ? "which: unavailable command\n"
                  : "help: unavailable command\n");
        status(1, PROCESS_EXITED, 0);
        return;
      }
      if (!which) {
        put(item->usage);
        put("\n");
      } else {
        put(item->name);
        put(item->kind == 0   ? ": builtin\n"
            : item->kind == 2 ? ": /boot/tview.snx (catalog 5)\n"
                              : ": /boot/gcmd.snx (catalog 4)\n");
      }
    } else {
      put("builtins: help pwd cd history status exit\n");
      put("navigation: which; paths / = /home/user (read-only)\n");
      for (u32 i = 0; i < sizeof(shell_commands) / sizeof(*shell_commands); i++)
        if (shell_commands[i].kind == 1 || shell_commands[i].kind == 2) {
          put(shell_commands[i].usage);
          put("\n");
        }
      put("diagnostics: args exitcode spin fault\n");
    }
  } else if (streq(command, "pwd")) {
    if (usage(1, 1))
      return;
    put("/");
    put(cwd);
    put("\n");
  } else if (streq(command, "cd")) {
    if (usage(1, 2))
      return;
    char path[128];
    const char *target = words.argc == 1 ? "/" : words.bytes + words.offsets[1];
    if (shell_path(cwd, target, path)) {
      put("cd: outside grant or path too long\n");
      status(1, PROCESS_EXITED, 0);
      return;
    }
    int next = *path ? OpenAt(root, path, FS_READ | FS_LOOKUP | FS_ENUM,
                              OPEN_DIRECTORY)
                     : root;
    if (next < 0) {
      put("cd failed ");
      integer(next);
      put("\n");
      status(1, PROCESS_EXITED, 0);
      return;
    }
    if (cwd_handle != root)
      Close(cwd_handle);
    cwd_handle = next;
    copy(cwd, path);
  } else if (streq(command, "history")) {
    if (usage(1, 1))
      return;
    for (u32 i = history_total - history_count; i != history_total; i++) {
      integer((i32)(i + 1));
      put(" ");
      put(history[i % 16]);
      put("\n");
    }
  } else if (entry && (entry->kind == 1 || entry->kind == 3)) {
    external(IX_COMMAND, 0);
    return;
  } else if (entry && entry->kind == 4) {
    put("unavailable: ");
    put(command);
    put("\n");
    status(127, PROCESS_EXITED, 0);
    return;
  } else if (entry && entry->kind == 2) {
    int ls = streq(command, "ls"),
        walk = streq(command, "tree") || streq(command, "find");
    u32 index = 1;
    if (ls && index < words.argc && words.bytes[words.offsets[index]] == '-') {
      const char *option = words.bytes + words.offsets[index++];
      if (!streq(option, "-a") && !streq(option, "-l") &&
          !streq(option, "-la") && !streq(option, "-al")) {
        put("usage: ls [-a|-l|-la|-al] [path]\n");
        status(2, PROCESS_EXITED, 0);
        return;
      }
    } else if ((streq(command, "head") || streq(command, "tail")) &&
               words.argc > 1 && streq(words.bytes + words.offsets[1], "-n"))
      index = 3;
    else if (streq(command, "wc") && words.argc > 1 &&
             words.bytes[words.offsets[1]] == '-')
      index = 2;
    else if (streq(command, "grep"))
      index =
          words.argc > 1 && streq(words.bytes + words.offsets[1], "-n") ? 3 : 2;
    u32 maximum = index + 1 + (streq(command, "find") ? 1 : 0);
    if (words.argc > maximum || (!(ls || walk) && words.argc != index + 1) ||
        words.argc < index) {
      put("usage: ");
      put(entry->usage);
      put("\n");
      status(2, PROCESS_EXITED, 0);
      return;
    }
    char path[128];
    if (shell_path(
            cwd, index == words.argc ? "." : words.bytes + words.offsets[index],
            path)) {
      put("path outside grant or too long\n");
      status(1, PROCESS_EXITED, 0);
      return;
    }
    if (!*path)
      copy(path, ".");
    /* Replace the path with a normalized grant-relative argument, preserving
     * command/options. No ambient cwd or global path enters the worker. */
    u32 at = index < words.argc ? words.offsets[index] : words.used;
    words.offsets[index] = at;
    char filter[256];
    int filtered = streq(command, "find") && words.argc > index + 1;
    if (filtered)
      copy(filter, words.bytes + words.offsets[index + 1]);
    copy(words.bytes + at, path);
    words.used = at + strlen(path) + 1;
    if (filtered) {
      words.offsets[index + 1] = words.used;
      copy(words.bytes + words.used, filter);
      words.used += strlen(filter) + 1;
    }
    if (words.argc < index + 1)
      words.argc = index + 1;
    external(IX_READTOOLS, FS_READ | FS_LOOKUP | ((ls || walk) ? FS_ENUM : 0));
    return;
  } else {
    put("unknown command: ");
    put(command);
    put("\n");
    status(127, PROCESS_EXITED, 0);
    return;
  }
  status(0, PROCESS_EXITED, 0);
}
int main(void) {
  StartInfo start;
  terminal = TerminalCurrent();
  root = StartDirectory();
  cwd_handle = root;
  if (QueryStartInfo(&start) || start.policy != PROCESS_STRICT || start.caps ||
      QueryCliInfo(&cli) || terminal < 0 || root < 0) {
    Log("gsh requires an interactive session\n");
    return 2;
  }
  TerminalInfo display;
  u32 columns = 120, rows = 25;
  if (TerminalGetInfo(terminal, &display) == 0) {
    columns = display.width;
    rows = display.height;
  }
  if (columns >= 56)
    for (u32 row = 0; row < BANNER_ROWS; row++) {
      put(banner[row][0]);
      put("    ");
      put(banner[row][1]);
      put("\n");
    }
  put("SINO user Shell\nSINO 0.10 | ");
  integer((i32)columns);
  put(" x ");
  integer((i32)rows);
  put("\n/ = /home/user (read-only)\nType help for commands; about for "
      "sources.\n");
  prompt();
  for (;;) {
    TerminalEvent event;
    while (TerminalNextEvent(terminal, &event) == 0)
      if (event.type == TERM_INTERRUPT) {
        length = 0;
        position = available;
        status(130, PROCESS_CANCELED, 0);
        put("\n");
        prompt();
      }
    if (position == available) {
      int n = StreamRead(cli.in, input, sizeof(input));
      if (n == S_AGAIN) {
        Yield(1);
        continue;
      }
      if (n == 0) {
        if (length) {
          line[length] = 0;
          execute();
          length = 0;
        }
        return last_code;
      }
      if (n == S_CLOSED)
        return last_code;
      if (n < 0)
        return 1;
      position = 0;
      available = (u32)n;
    }
    char c = input[position++];
    if (c == '\n') {
      line[length] = 0;
      execute();
      length = 0;
      prompt();
    } else if (length < SHELL_LINE - 1)
      line[length++] = c;
    else {
      put("line too long\n");
      return 2;
    }
  }
}
