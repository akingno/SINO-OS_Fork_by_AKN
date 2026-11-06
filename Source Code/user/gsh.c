#include "interactive.h"
#include "stream.h"
#include "gsh_parse.h"
#include "shell_commands.h"
#include "shell_banner.h"
#include "line_edit.h"
static CliInfo cli;
static ShellWords words;
static int terminal, root, cwd_handle, writable;
static char cwd[128], previous_cwd[128], history[16][SHELL_LINE];
static char hostname[32] = "sino";
static u32 history_total, history_count, request_id;
static i32 last_code, last_start;
static u32 last_reason = PROCESS_EXITED;
static char input[64];
static u32 position, available;
static LineEditor editor;
#define line editor.text
#define length editor.length
static int clear_next, recall = -1, rejected;
static char draft[256];
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
static void edit_failure(const char *stage, int error) {
  char n[12];
  Log(stage);
  if (error < 0) {
    Log("-");
    number(0u - (u32)error, n);
  } else
    number((u32)error, n);
  Log(n);
  Log("\n");
  Exit(1);
}
static void render_edit(u32 action) {
  TerminalEdit frame;
  memset(&frame, 0, sizeof(frame));
  copy(frame.text, "gsh:/");
  copy(frame.text + strlen(frame.text), cwd);
  copy(frame.text + strlen(frame.text), " $ ");
  u32 prefix = strlen(frame.text);
  copy(frame.text + prefix, line);
  frame.bytes = prefix + length;
  frame.cursor = prefix + editor.cursor;
  frame.action = action;
  int s;
  while ((s = TerminalEditSend(terminal, &frame)) == S_AGAIN)
    Yield(1);
  if (s < 0)
    edit_failure("SHELL EDIT SEND ", s);
  while ((s = TerminalCall(TERM_EDIT_STATUS, terminal, 0, 0, 0, 0)) == S_AGAIN)
    Yield(1);
  if (s < 0)
    edit_failure("SHELL EDIT ACK ", s);
}
static void prompt(void) {
  int mode = TerminalMode(terminal, 0);
  if (mode < 0)
    edit_failure("SHELL RAW MODE ", mode);
  render_edit(clear_next ? 2 : 0);
  clear_next = 0;
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
  error = InteractiveResultWait(terminal, request_id, &result);
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
static int path_prefix(const char *path, const char *prefix) {
  while (*prefix)
    if (*path++ != *prefix++)
      return 0;
  return !*path || *path == '/';
}
static int home_path(const char *path, char out[128]) {
  if (path_prefix(path, "/home/user"))
    path = path[10] ? path + 10 : "/";
  else if (path_prefix(path, "~"))
    path = path[1] ? path + 1 : "/";
  else if (path_prefix(path, "/boot"))
    return -1;
  return shell_path(cwd, path, out);
}
static const char *argument(u32 index) {
  return words.bytes + words.offsets[index];
}
static int writable_session(void) {
  if (writable)
    return 1;
  put("read-only session\n");
  status(1, PROCESS_EXITED, 0);
  return 0;
}
static int hostname_valid(const char *name) {
  if (!*name || strlen(name) >= sizeof(hostname))
    return 0;
  for (; *name; name++) {
    char c = *name;
    if (!(c >= 'a' && c <= 'z') && !(c >= 'A' && c <= 'Z') &&
        !(c >= '0' && c <= '9') && c != '-')
      return 0;
  }
  return 1;
}
static void desktop_run(const char *command) {
  if (!writable_session())
    return;
  DesktopRun request = {{0}, {0}};
  u32 operation = DESKTOP_RUN_APP;
  if (streq(command, "kill")) {
    if (usage(2, 2))
      return;
    i32 pid;
    if (shell_integer(argument(1), &pid) || pid <= 0)
      goto invalid;
    operation = DESKTOP_KILL;
    memcpy(request.path, &pid, sizeof(pid));
  } else if (streq(command, "install")) {
    if (usage(2, 2))
      return;
    operation = DESKTOP_INSTALL;
    if (strlen(argument(1)) >= sizeof(request.argument))
      goto invalid;
    copy(request.argument, argument(1));
  } else if (streq(command, "nettest")) {
    if (usage(1, 1))
      return;
    operation = DESKTOP_NETTEST;
  } else {
    int strict = words.argc > 1 && streq(argument(1), "-s");
    u32 path_index = strict ? 2 : 1;
    if (usage(path_index + 1, path_index + 2))
      return;
    const char *path = argument(path_index);
    char relative[128];
    int boot = path_prefix(path, "/boot");
    if (boot ? shell_path("", path[5] ? path + 5 : "/", relative)
             : home_path(path, relative))
      goto invalid;
    const char *prefix = boot ? "/boot/" : "/home/user/";
    if (!*relative || strlen(prefix) + strlen(relative) >= sizeof(request.path))
      goto invalid;
    copy(request.path, prefix);
    copy(request.path + strlen(prefix), relative);
    if (words.argc > path_index + 1) {
      const char *arg = argument(path_index + 1);
      if (strlen(arg) >= sizeof(request.argument))
        goto invalid;
      copy(request.argument, arg);
    }
    if (strict)
      operation = DESKTOP_RUN_STRICT;
  }
  int pid = TerminalCall(TERM_DESKTOP_RUN, terminal, &request,
                         sizeof(request), operation, 0);
  if (pid < 0) {
    put("start failed ");
    integer(pid);
    put("\n");
    status(126, 0, pid);
  } else if (operation != DESKTOP_KILL) {
    put("Started user process ");
    integer(pid);
    put("\n");
    status(0, PROCESS_EXITED, 0);
  } else
    status(0, PROCESS_EXITED, 0);
  return;
invalid:
  put("invalid application path or argument\n");
  status(2, PROCESS_EXITED, 0);
}
static void redirect_echo(u32 operator) {
  int append = line[operator + 1] == '>';
  u32 start = operator + 1 + (u32)append;
  ShellWords target;
  char path[128];
  if (!words.argc || !streq(argument(0), "echo") || words.argc == 16 ||
      shell_parse(line + start, length - start, &target) || target.argc != 1) {
    put("syntax error: expected echo text > path or echo text >> path\n");
    status(2, PROCESS_EXITED, 0);
    return;
  }
  if (!writable_session())
    return;
  if (home_path(target.bytes, path) || !*path) {
    put("path outside grant or invalid output file\n");
    status(1, PROCESS_EXITED, 0);
    return;
  }
  ShellWords original = words;
  copy(words.bytes, append ? "echo>>" : "echo>");
  words.offsets[0] = 0;
  words.offsets[1] = strlen(words.bytes) + 1;
  copy(words.bytes + words.offsets[1], path);
  words.used = words.offsets[1] + strlen(path) + 1;
  for (u32 i = 1; i < original.argc; i++) {
    const char *arg = original.bytes + original.offsets[i];
    words.offsets[i + 1] = words.used;
    copy(words.bytes + words.used, arg);
    words.used += strlen(arg) + 1;
  }
  words.argc++;
  external(IX_WRITETOOLS, FS_LOOKUP | FS_READ | FS_WRITE | FS_CREATE);
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
  u32 output_operator = shell_output_operator(line, length);
  if (shell_parse(line, output_operator, &words)) {
    put("syntax error\n");
    status(2, PROCESS_EXITED, 0);
    return;
  }
  if (output_operator < length) {
    redirect_echo(output_operator);
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
  if (entry && entry->kind == 6) {
    desktop_run(command);
    return;
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
  if (streq(command, "clear")) {
    if (usage(1, 1))
      return;
    clear_next = 1;
    status(0, PROCESS_EXITED, 0);
    return;
  }
  if (streq(command, "help") || streq(command, "which")) {
    int which = streq(command, "which");
    if (usage(which ? 2 : 1, 2))
      return;
    if (words.argc == 2) {
      const ShellCommand *item = shell_command(words.bytes + words.offsets[1]);
      if (!item || item->kind == 4 || (which && item->kind == 6 && !writable)) {
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
            : item->kind == 5 ? ": /boot/writer.snx (catalog 6)\n"
            : item->kind == 2 ? ": /boot/tview.snx (catalog 5)\n"
            : item->kind == 6 ? ": desktop launch service\n"
                              : ": /boot/gcmd.snx (catalog 4)\n");
      }
    } else {
      for (u32 i = 0; i < sizeof(shell_commands) / sizeof(*shell_commands); i++)
        if (shell_commands[i].kind == 1 || shell_commands[i].kind == 2) {
          put(shell_commands[i].usage);
          put("\n");
        }
      put("builtins: help pwd cd history status exit\n");
      put("navigation: which; clear (Ctrl+L); / = /home/user\n");
      put("identity: whoami hostname env\n");
      put("workspace (write session): touch mkdir rm rmdir write append cp mv "
          "sync\n");
      put("write session: echo text > path, echo text >> path, run, kill, install, nettest\n");
      put("diagnostics: args exitcode spin fault\n");
    }
  } else if (streq(command, "whoami")) {
    if (usage(1, 1))
      return;
    put("user\n");
  } else if (streq(command, "hostname")) {
    if (usage(1, 2))
      return;
    if (words.argc == 2) {
      if (!hostname_valid(argument(1))) {
        put("hostname: use 1..31 letters, digits or hyphens\n");
        status(2, PROCESS_EXITED, 0);
        return;
      }
      copy(hostname, argument(1));
    }
    put(hostname);
    put("\n");
  } else if (streq(command, "env")) {
    if (usage(1, 1))
      return;
    put("USER=user\nHOME=/home/user\nPWD=/");
    put(cwd);
    put("\nHOSTNAME=");
    put(hostname);
    put("\nSHELL=gsh\nPATH=/boot\nVERSION=0.10\n");
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
    if (streq(target, "-"))
      copy(path, previous_cwd);
    else if (home_path(target, path)) {
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
    copy(previous_cwd, cwd);
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
  } else if (entry && entry->kind == 5) {
    if (!writable_session())
      return;
    int dual = streq(command, "cp") || streq(command, "mv"),
        text = streq(command, "write") || streq(command, "append"),
        sync = streq(command, "sync");
    if ((sync && words.argc != 1) ||
        (!sync && !text && words.argc != (dual ? 3u : 2u)) ||
        (text && words.argc < 3)) {
      put("usage: ");
      put(entry->usage);
      put("\n");
      status(2, PROCESS_EXITED, 0);
      return;
    }
    int boot_copy = streq(command, "cp") && path_prefix(argument(1), "/boot");
    ShellWords original = words;
    words.used = 0;
    for (u32 i = 0; i < original.argc; i++) {
      const char *value = original.bytes + original.offsets[i];
      char path[128];
      if (boot_copy && i == 0)
        value = "cp-boot";
      if (i == 1 || (dual && i == 2)) {
        int boot_source = boot_copy && i == 1;
        if (boot_source ? shell_path("", value[5] ? value + 5 : "/", path)
                        : home_path(value, path)) {
          put("path outside grant or too long\n");
          status(1, PROCESS_EXITED, 0);
          return;
        }
        if (!*path)
          copy(path, ".");
        value = path;
      }
      words.offsets[i] = words.used;
      copy(words.bytes + words.used, value);
      words.used += strlen(value) + 1;
    }
    u32 rights = FS_LOOKUP;
    if (sync)
      rights = FS_COMMIT;
    else if (streq(command, "touch"))
      rights |= FS_CREATE | FS_WRITE;
    else if (streq(command, "mkdir"))
      rights |= FS_CREATE;
    else if (streq(command, "rm") || streq(command, "rmdir"))
      rights |= FS_REMOVE;
    else if (streq(command, "mv"))
      rights |= FS_REMOVE | FS_CREATE;
    else
      rights |= FS_READ | FS_WRITE | FS_CREATE;
    external(boot_copy ? IX_BOOTCOPY : IX_WRITETOOLS, rights);
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
    const char *target = index == words.argc ? "." : argument(index);
    int boot = path_prefix(target, "/boot");
    if (boot ? shell_path("", target[5] ? target + 5 : "/", path)
             : home_path(target, path)) {
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
    external(boot ? IX_BOOTTOOLS : IX_READTOOLS,
             FS_READ | FS_LOOKUP | ((ls || walk) ? FS_ENUM : 0));
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
static char candidates[128][128];
static int starts(const char *s, const char *prefix) {
  while (*prefix)
    if (*s++ != *prefix++)
      return 0;
  return 1;
}
static void complete_input(void) {
  if (editor.cursor != length)
    return;
  u32 start = editor.cursor;
  while (start && line[start - 1] != ' ')
    start--;
  for (u32 i = 0; i < length; i++)
    if (line[i] == '\\' || line[i] == '\'' || line[i] == '"')
      return;
  char token[256];
  copy(token, line + start);
  u32 n = 0, prefix = 0;
  if (!start) {
    for (u32 i = 0; i < sizeof(shell_commands) / sizeof(*shell_commands); i++)
      if (shell_commands[i].kind != 4 &&
          starts(shell_commands[i].name, token)) {
        copy(candidates[n++], shell_commands[i].name);
      }
  } else {
    char path[128], parent[256];
    copy(parent, token);
    char *base = token;
    for (u32 i = 0; token[i]; i++)
      if (token[i] == '/')
        base = token + i + 1;
    prefix = (u32)(base - token);
    if (prefix)
      parent[prefix] = 0;
    else
      copy(parent, ".");
    if (shell_path(cwd, parent, path))
      return;
    if (!*path)
      copy(path, ".");
    int dir = OpenAt(root, path, FS_READ | FS_LOOKUP | FS_ENUM, OPEN_DIRECTORY);
    if (dir < 0)
      return;
    DirEntry entries[8];
    DirPage page = {0, 0, (u32)entries, 8, 0, 0};
    int error = 0;
    do {
      error = DirectoryRead(dir, &page);
      if (error < 0)
        break;
      for (u32 i = 0; i < page.count; i++)
        if (n < 128 && starts(entries[i].name, base)) {
          copy(candidates[n], entries[i].name);
          if (entries[i].dir)
            copy(candidates[n] + strlen(candidates[n]), "/");
          n++;
        }
    } while (!page.done);
    Close(dir);
    if (error < 0)
      return;
  }
  if (!n)
    return;
  u32 common = strlen(candidates[0]);
  for (u32 i = 1; i < n; i++) {
    u32 j = 0;
    while (j < common && candidates[0][j] == candidates[i][j])
      j++;
    common = j;
  }
  while (common && ((u8)candidates[0][common] & 0xc0) == 0x80)
    common--;
  u32 typed = strlen(token + prefix);
  if (common > typed) {
    char suffix[256];
    u32 used = 0;
    for (u32 i = typed; i < common; i++) {
      char c = candidates[0][i];
      if (c == ' ' || c == '\\' || c == '\'' || c == '"' || c == '|' ||
          c == '&' || c == ';' || c == '<' || c == '>' || c == '$' ||
          c == '`' || c == '*' || c == '?')
        suffix[used++] = '\\';
      suffix[used++] = c;
    }
    if (line_insert(&editor, suffix, used) < 0)
      return;
  } else if (n > 1) {
    render_edit(1);
    for (u32 i = 0; i < n; i++) {
      for (u32 j = 0; candidates[i][j]; j++)
        if ((u8)candidates[i][j] < 32 || candidates[i][j] == 127)
          candidates[i][j] = '?';
      put(candidates[i]);
      put("\n");
    }
  }
  if (n == 1 && candidates[0][strlen(candidates[0]) - 1] != '/')
    line_insert(&editor, " ", 1);
  render_edit(0);
}
static void edit_key(int key) {
  if (key == 328) {
    if (recall < 0)
      copy(draft, line);
    if ((u32)(recall + 1) < history_count)
      recall++;
    if (recall >= 0)
      line_set(&editor, history[(history_total - 1 - (u32)recall) % 16]);
  } else if (key == 336) {
    if (recall >= 0) {
      recall--;
      line_set(&editor, recall < 0
                            ? draft
                            : history[(history_total - 1 - (u32)recall) % 16]);
    }
  } else if (key == 12) {
    render_edit(2);
    return;
  } else if (key == 9) {
    complete_input();
    return;
  } else
    line_key(&editor, key);
  render_edit(0);
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
  writable = cli.argc == 1 && streq(cli.bytes, "shell-write");
  put("SINO user Shell\nSINO 0.10 | ");
  integer((i32)columns);
  put(" x ");
  integer((i32)rows);
  put(writable ? "\n/ = /home/user (read-write)\n"
               : "\n/ = /home/user (read-only)\n");
  put("Type help for commands; about for sources.\n");
  prompt();
  char utf[4];
  u32 utf_used = 0, utf_need = 0;
  for (;;) {
    TerminalEvent event;
    if (TerminalNextEvent(terminal, &event) == 0) {
      if (event.type == TERM_TEXT) {
        input[0] = (char)event.value;
        position = 0;
        available = 1;
        goto consume;
      }
      if (event.type == TERM_EOF) {
        render_edit(1);
        if (length && !rejected)
          execute();
        return last_code;
      }
      if (event.type == TERM_INTERRUPT) {
        render_edit(1);
        line_set(&editor, "");
        position = available;
        recall = -1;
        utf_used = utf_need = 0;
        rejected = 0;
        status(130, PROCESS_CANCELED, 0);
        prompt();
      } else if (event.type == TERM_KEY) {
        utf_used = utf_need = 0;
        edit_key((int)event.value);
      } else if (event.type == TERM_RESIZE)
        render_edit(0);
      continue;
    }
    if (position == available) {
      int n = StreamRead(cli.in, input, sizeof(input));
      if (n == S_AGAIN) {
        Yield(1);
        continue;
      }
      if (n == 0) {
        render_edit(1);
        if (length && !rejected)
          execute();
        return last_code;
      }
      if (n == S_CLOSED)
        return last_code;
      if (n < 0)
        return 1;
      position = 0;
      available = (u32)n;
    }
  consume:;
    u8 c = (u8)input[position++];
    if (c == '\n') {
      utf_used = utf_need = 0;
      if (!rejected) {
        render_edit(1);
        execute();
      }
      line_set(&editor, "");
      recall = -1;
      rejected = 0;
      prompt();
      continue;
    }
    if (rejected)
      continue;
    if (c == 8 || c == 9) {
      utf_used = utf_need = 0;
      edit_key(c);
      continue;
    }
    int result = 0;
    if (c < 128) {
      utf_used = utf_need = 0;
      char ch = (char)c;
      result = line_insert(&editor, &ch, 1);
    } else {
      if (!utf_used)
        utf_need = c >= 0xc2 && c <= 0xdf   ? 2
                   : c >= 0xe0 && c <= 0xef ? 3
                   : c >= 0xf0 && c <= 0xf4 ? 4
                                            : 1;
      utf[utf_used++] = (char)c;
      if (utf_used == utf_need) {
        result = line_insert(&editor, utf, utf_used);
        utf_used = utf_need = 0;
      }
    }
    if (result < 0) {
      render_edit(1);
      put("input rejected: invalid UTF-8 or line limit\n");
      line_set(&editor, "");
      rejected = 1;
    }
    render_edit(0);
  }
}
