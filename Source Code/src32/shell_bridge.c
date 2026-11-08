#if defined(SINO_SHELL_BRIDGE) || defined(SELFTEST)
#include "klog.h"
#include "run_bridge.h"
#include "rtc.h"
/* SINO SH: project-specific shell, separate from the SINO32 kernel. */
#include "vfs.h"
#include "process.h"
#include "storage.h"
#include "lib/kstring.h"
#include "mm/page.h"
#include "panic.h"
#define LINE_SIZE 256
#define TOKENS 32
#define PIPE_SIZE 8192u
#define SCRIPT_DEPTH 4
typedef struct {
  u8* data;
  u32 length, capacity;
  int overflow;
} Sink;
typedef struct {
  const u8* input;
  u32 input_length;
  int input_present;
  Sink* out;
  int depth;
} Context;
typedef struct {
  char text[LINE_SIZE];
  int type;
} Token;
static char hostname[32] = "sino";
static int last_status, previous_cwd;
void shell_command_init(void) { previous_cwd = cwd; }
static u8 work[MAX_FILE + 1], pipes[SCRIPT_DEPTH][2][PIPE_SIZE + 1];
static char scripts[SCRIPT_DEPTH][4096];
u32 native_base, native_size;
int native_running;
static Sink* native_sink;
static const char* commands[] = {
    "help",    "ls",     "cd",       "pwd",     "cat",     "echo",   "clear",
    "uname",   "ver",    "mem",      "free",    "uptime",  "date",   "time",
    "history", "whoami", "hostname", "env",     "which",   "touch",  "mkdir",
    "rmdir",   "rm",     "cp",       "mv",      "write",   "append", "head",
    "tail",    "wc",     "grep",     "hexdump", "stat",    "tree",   "find",
    "df",      "run",    "source",   "sleep",   "ps",      "about",  "credits",
    "reboot",  "halt",   "sync",     "nettest", "kill",   0};
static void strcopy(char* d, const char* s) {
  while ((*d++ = *s++)) {
  }
}
static int starts(const char* s, const char* prefix) {
  while (*prefix)
    if (*s++ != *prefix++)
      return 0;
  return 1;
}
static int contains(const char* s, u32 length, const char* pattern) {
  u32 n = strlen(pattern);
  if (n > length)
    return 0;
  for (u32 i = 0; i + n <= length; i++) {
    u32 j = 0;
    while (j < n && s[i + j] == pattern[j])
      j++;
    if (j == n)
      return 1;
  }
  return 0;
}
static void emit(Sink* sink, const void* bytes, u32 n) {
  const u8* p = bytes;
  if (sink && sink->data) {
    if (n > sink->capacity - sink->length) {
      sink->overflow = 1;
      return;
    }
    memcpy(sink->data + sink->length, p, n);
    sink->length += n;
    sink->data[sink->length] = 0;
  } else {
    klog_write_bytes(p, n);
  }
}
static void put(Sink* s, const char* text) {
  emit(s, text, strlen(text));
}
static void num(Sink* s, u32 n) {
  char b[11];
  number(n, b);
  put(s, b);
}
static int error(Sink* s, const char* command, const char* reason) {
  put(s, command);
  put(s, ": ");
  put(s, reason);
  put(s, "\n");
  return 1;
}
static int verror(Sink* s, const char* command) {
  return error(s, command, vfs_error);
}
void native_write(const char* s, u32 n) {
  emit(native_sink, s, n);
}
static const char* variable(const char* name, char* buffer) {
  if (streq(name, "PWD")) {
    vfs_path(cwd, buffer);
    return buffer;
  }
  if (streq(name, "HOME"))
    return "/home/user";
  if (streq(name, "USER"))
    return "user";
  if (streq(name, "HOSTNAME"))
    return hostname;
  if (streq(name, "SHELL"))
    return "sinosh";
  if (streq(name, "PATH"))
    return "/boot";
  if (streq(name, "VERSION"))
    return "0.10";
  if (streq(name, "?")) {
    number((u32)last_status, buffer);
    return buffer;
  }
  return "";
}
static int token_char(Token* t, u32* n, char c) {
  if (*n >= LINE_SIZE - 1)
    return 0;
  t->text[(*n)++] = c;
  return 1;
}
static int tokenize(const char* s, Token* t) {
  int count = 0;
  u32 i = 0;
  while (s[i]) {
    while (s[i] == ' ' || s[i] == '\t')
      i++;
    if (!s[i] || s[i] == '#')
      break;
    if (count >= TOKENS)
      return -1;
    Token* v = &t[count];
    v->type = 0;
    u32 n = 0;
    if (s[i] == '|' || s[i] == ';' || s[i] == '<' || s[i] == '>') {
      v->type = (u8)s[i++];
      if (v->type == '>' && s[i] == '>') {
        v->type = '+';
        i++;
      }
      v->text[0] = 0;
      count++;
      continue;
    }
    char quote = 0;
    int began = 0;
    while (s[i]) {
      char c = s[i];
      if (!quote && (c == ' ' || c == '\t' || c == '|' || c == ';' ||
                     c == '<' || c == '>'))
        break;
      began = 1;
      if ((c == '\'' || c == '"') && (!quote || quote == c)) {
        quote = quote ? 0 : c;
        i++;
        continue;
      }
      if (c == '\\' && quote != '\'') {
        if (quote == '"' && s[i + 1] != '"' && s[i + 1] != '\\' &&
            s[i + 1] != '$') {
          if (!token_char(v, &n, '\\'))
            return -1;
          i++;
          continue;
        }
        i++;
        if (!s[i])
          return -1;
        if (!token_char(v, &n, s[i++]))
          return -1;
        continue;
      }
      if (c == '$' && quote != '\'') {
        i++;
        char name[32], buffer[VFS_PATH];
        u32 p = 0;
        if (s[i] == '?')
          name[p++] = s[i++];
        else if (s[i] == '{') {
          i++;
          while (s[i] && s[i] != '}' && p < 31)
            name[p++] = s[i++];
          if (s[i] != '}')
            return -1;
          i++;
        } else {
          while (((s[i] >= 'A' && s[i] <= 'Z') ||
                  (s[i] >= 'a' && s[i] <= 'z') || s[i] == '_' ||
                  (s[i] >= '0' && s[i] <= '9')) &&
                 p < 31)
            name[p++] = s[i++];
        }
        name[p] = 0;
        if (!p) {
          if (!token_char(v, &n, '$'))
            return -1;
        } else
          for (const char* value = variable(name, buffer); *value; value++)
            if (!token_char(v, &n, *value))
              return -1;
        continue;
      }
      if (!token_char(v, &n, c))
        return -1;
      i++;
    }
    if (quote)
      return -1;
    v->text[n] = 0;
    if (began)
      count++;
  }
  return count;
}
static int execute(const char*, Sink*, int);
static int read_input(Context* c, const char* path, const u8** data,
                      u32* size) {
  if (!path) {
    if (!c->input_present)
      return error(c->out, "input", "provide a file or pipe");
    *data = c->input;
    *size = c->input_length;
    return 0;
  }
  int n = vfs_lookup(path);
  if (n < 0)
    return verror(c->out, path);
  int bytes = vfs_read(n, work, MAX_FILE);
  if (bytes < 0)
    return verror(c->out, path);
  work[bytes] = 0;
  *data = work;
  *size = (u32)bytes;
  return 0;
}
static int parse_number(const char* s, u32* value) {
  if (!*s)
    return 0;
  u32 n = 0;
  while (*s) {
    if (*s < '0' || *s > '9' || n > 100000)
      return 0;
    n = n * 10 + (u32)(*s++ - '0');
  }
  *value = n;
  return 1;
}
static int builtin(const char* name) {
  for (int i = 0; commands[i]; i++)
    if (streq(name, commands[i]))
      return 1;
  return 0;
}
static void list_node(Sink* out, int n, int detail) {
  Node* p = &nodes[n];
  if (detail) {
    put(out, p->dir ? "dir " : p->readonly ? "ro  " : "ram ");
    num(out, p->size);
    put(out, "\t");
  }
  put(out, p->name);
  if (p->dir)
    put(out, "/");
  put(out, "\n");
}
static void list_tree(Sink* out, int parent, u32 depth) {
  if (depth > 16)
    return;
  for (int i = 1; i < VFS_NODES; i++)
    if (nodes[i].used && nodes[i].parent == parent) {
      for (u32 j = 0; j < depth; j++)
        put(out, "  ");
      list_node(out, i, 0);
      if (nodes[i].dir)
        list_tree(out, i, depth + 1);
    }
}
static int run_program(Context* c, const char* path, const char* args) {
  int n = vfs_lookup(path);
  if (n < 0)
    return verror(c->out, path);
  if (nodes[n].size >= 32) {
    int size = vfs_read(n, work, MAX_FILE);
    if (size >= 32 && *(u32*)work == NATIVE_MAGIC) {
      int pid = process_launch_args(path, CAP_HOME, args);
      if (pid < 0)
        return error(c->out, path, "native launch failed");
      put(c->out, "Started user process ");
      num(c->out, (u32)pid);
      put(c->out, "\n");
      return 0;
    }
  }
  if (nodes[n].boot < 0 || !streq(nodes[n].name, "hello.f32"))
    return error(c->out, path,
                 "legacy ring-0 binaries are restricted to trusted boot demo");
  int bytes = vfs_read(n, work, 4096);
  if (bytes < 0)
    return verror(c->out, path);
  if (bytes < 16 || work[0] != 'F' || work[1] != 'X' || work[2] != '3' ||
      work[3] != '2')
    return error(c->out, path, "not a native F32 executable");
  u32 entry = *(u32*)(work + 4);
  if (entry < 16 || entry >= (u32)bytes)
    return error(c->out, path, "invalid F32 entry");
  native_base = (u32)page_alloc();
  if (!native_base)
    return error(c->out, path, "out of memory");
  native_size = (u32)bytes;
  memcpy((void*)native_base, work, native_size);
  native_sink = c->out;
  native_running = 1;
  pr_debug("NATIVE32 shell program entered\n");
  run_native(native_base + entry);
  native_running = 0;
  native_sink = 0;
  if (!page_free((void*)native_base))
    panic("Native page release", native_base, 0);
  native_base = native_size = 0;
  pr_debug("NATIVE32 shell program returned\n");
  return 0;
}
static void two(Sink* out, u32 n) {
  if (n < 10)
    put(out, "0");
  num(out, n);
}
static int clock_output(Sink* out, int date) {
  rtc_poll();
  RtcDateTime t;
  if (rtc_read(&t))
    return error(out, date ? "date" : "time", "RTC time unavailable");
  if (date) {
    num(out, t.year);
    put(out, "-");
    two(out, t.month);
    put(out, "-");
    two(out, t.day);
  } else {
    two(out, t.hour);
    put(out, ":");
    two(out, t.minute);
    put(out, ":");
    two(out, t.second);
  }
  put(out, "\n");
  return 0;
}
static int command(Context* c, int argc, char** argv) {
  Sink* out = c->out;
  char* cmd = argv[0];
  for (char* p = cmd; *p; p++)
    if (*p >= 'A' && *p <= 'Z')
      *p += 32;
  if (streq(cmd, "dir"))
    cmd = "ls";
  if (streq(cmd, "type"))
    cmd = "cat";
  if (streq(cmd, "cls"))
    cmd = "clear";
  if (streq(cmd, "md"))
    cmd = "mkdir";
  if (streq(cmd, "del"))
    cmd = "rm";
  if (streq(cmd, "copy"))
    cmd = "cp";
  if (streq(cmd, "ren"))
    cmd = "mv";
  if (streq(cmd, "."))
    cmd = "source";
  if (streq(cmd, "help")) {
    put(out,
        "SINO SH commands\n"
        "  help (command summary)\n"
        "  ls [-l|-a|-la] [path]   cd [path]   pwd   tree [path]   find [path] "
        "[text]\n"
        "  cat [file]       head/tail [-n N] [file]       stat file\n"
        "  grep [-n] text [file]   wc [-l|-w|-c] [file]   hexdump file\n"
        "  mkdir [-p] path  rmdir path  touch file  rm file  cp/mv source "
        "dest\n"
        "  write file text   append file text   echo [-e|-n] text\n"
        "  run file.snx [argument]   run /boot/hello.f32   source file.sh\n"
        "  run -s file.snx [argument]  strict: no file or spawn authority\n"
        "  uname [-a]  ver  mem/free  df  uptime  date  time  ps\n"
        "  history  whoami  hostname [name]  env  which command\n"
        "  sync   kill PID   sleep seconds   nettest (127.0.0.1 loopback)\n"
        "  clear  about/credits  reboot  halt\n\n"
        "run .snx reports process creation, not the program exit status.\n"
        "reboot/halt end the current session; sync saves home data first.\n"
        "Syntax: quotes, $PWD/$HOME/$USER/$?, ;, pipes, <, > and >>\n"
        "Keys: Tab complete; Up/Down history; Left/Right/Home/End edit;\n"
        "      PgUp/PgDn scroll; Ctrl+L clear; Ctrl+C cancel input.\n"
        "DOS aliases: DIR TYPE CLS MD DEL COPY REN VER; . file.sh = source "
        "file.sh\n"
        "Home files are saved to the data disk when one is attached. /tmp is "
        "temporary.\n");
    return 0;
  }
  if (streq(cmd, "pwd")) {
    char p[VFS_PATH];
    vfs_path(cwd, p);
    put(out, p);
    put(out, "\n");
    return 0;
  }
  if (streq(cmd, "cd")) {
    int back = argc > 1 && streq(argv[1], "-"),
        n = back ? previous_cwd : vfs_lookup(argc > 1 ? argv[1] : "/home/user");
    if (n < 0)
      return verror(out, "cd");
    if (!nodes[n].used || !nodes[n].dir)
      return error(out, "cd", "not a directory");
    previous_cwd = cwd;
    cwd = n;
    if (back) {
      char p[VFS_PATH];
      vfs_path(cwd, p);
      put(out, p);
      put(out, "\n");
    }
    return 0;
  }
  if (streq(cmd, "ls")) {
    int detail = 0, all = 0;
    const char* path = ".";
    for (int i = 1; i < argc; i++) {
      if (argv[i][0] == '-' && argv[i][1]) {
        for (u32 j = 1; argv[i][j]; j++) {
          if (argv[i][j] == 'l')
            detail = 1;
          else if (argv[i][j] == 'a')
            all = 1;
          else
            return error(out, "ls", "supported options: -l, -a, -la");
        }
      } else
        path = argv[i];
    }
    int n = vfs_lookup(path);
    if (n < 0)
      return verror(out, "ls");
    if (!nodes[n].dir)
      list_node(out, n, detail);
    else
      for (int i = 1; i < VFS_NODES; i++)
        if (nodes[i].used && nodes[i].parent == n &&
            (all || nodes[i].name[0] != '.'))
          list_node(out, i, detail);
    return 0;
  }
  if (streq(cmd, "tree")) {
    int n = vfs_lookup(argc > 1 ? argv[1] : ".");
    if (n < 0)
      return verror(out, "tree");
    put(out, argc > 1 ? argv[1] : ".");
    put(out, "\n");
    if (nodes[n].dir)
      list_tree(out, n, 1);
    return 0;
  }
  if (streq(cmd, "find")) {
    char base[VFS_PATH], p[VFS_PATH];
    if (vfs_canonical(argc > 1 ? argv[1] : ".", base) < 0)
      return verror(out, "find");
    int n = vfs_lookup(base);
    if (n < 0)
      return verror(out, "find");
    u32 len = strlen(base);
    for (int i = 0; i < VFS_NODES; i++)
      if (nodes[i].used) {
        vfs_path(i, p);
        if (starts(p, base) && (len == 1 || p[len] == 0 || p[len] == '/') &&
            (argc < 3 || contains(p, strlen(p), argv[2]))) {
          put(out, p);
          put(out, "\n");
        }
      }
    return 0;
  }
  if (streq(cmd, "cat")) {
    if (argc == 1) {
      const u8* data;
      u32 size;
      if (read_input(c, 0, &data, &size))
        return 1;
      emit(out, data, size);
    } else
      for (int i = 1; i < argc; i++) {
        const u8* data;
        u32 size;
        if (read_input(c, argv[i], &data, &size))
          return 1;
        emit(out, data, size);
      }
    return 0;
  }
  if (streq(cmd, "echo")) {
    int escape = 0, newline = 1, start = 1;
    while (start < argc && argv[start][0] == '-') {
      if (streq(argv[start], "-e"))
        escape = 1;
      else if (streq(argv[start], "-n"))
        newline = 0;
      else
        break;
      start++;
    }
    for (int i = start; i < argc; i++) {
      if (i > start)
        put(out, " ");
      for (u32 j = 0; argv[i][j]; j++) {
        char ch = argv[i][j];
        if (escape && ch == '\\' && argv[i][j + 1]) {
          ch = argv[i][++j];
          if (ch == 'n')
            ch = '\n';
          else if (ch == 't')
            ch = '\t';
          else if (ch == 'r')
            ch = '\r';
        }
        emit(out, &ch, 1);
      }
    }
    if (newline)
      put(out, "\n");
    return 0;
  }
  if (streq(cmd, "clear")) {
    return 0;
  }
  if (streq(cmd, "touch") || streq(cmd, "mkdir")) {
    int dir = streq(cmd, "mkdir"), parents = argc > 1 && streq(argv[1], "-p"),
        start = parents ? 2 : 1;
    if (argc <= start)
      return error(out, cmd, "missing path");
    for (int i = start; i < argc; i++) {
      int n = vfs_create(argv[i], dir, parents);
      if (n < 0)
        return verror(out, cmd);
      if (nodes[n].readonly)
        return error(out, cmd, "read-only file system (/boot)");
    }
    return 0;
  }
  if (streq(cmd, "rm") || streq(cmd, "rmdir")) {
    if (argc < 2)
      return error(out, cmd, "missing path");
    for (int i = 1; i < argc; i++)
      if (vfs_remove(argv[i], streq(cmd, "rmdir")) < 0)
        return verror(out, cmd);
    return 0;
  }
  if (streq(cmd, "cp") || streq(cmd, "mv")) {
    if (argc != 3)
      return error(out, cmd, "usage: source destination");
    int status = streq(cmd, "cp") ? vfs_copy(argv[1], argv[2], work)
                                  : vfs_move(argv[1], argv[2]);
    return status < 0 ? verror(out, cmd) : 0;
  }
  if (streq(cmd, "write") || streq(cmd, "append")) {
    if (argc < 3)
      return error(out, cmd, "usage: file text");
    u32 n = 0;
    for (int i = 2; i < argc; i++) {
      if (i > 2)
        work[n++] = ' ';
      u32 bytes = strlen(argv[i]);
      if (n + bytes + 1 > MAX_FILE)
        return error(out, cmd, "text too long");
      memcpy(work + n, argv[i], bytes);
      n += bytes;
    }
    work[n++] = '\n';
    int index = vfs_create(argv[1], 0, 0);
    if (index < 0 || vfs_write(index, work, n, streq(cmd, "append")) < 0)
      return verror(out, cmd);
    return 0;
  }
  if (streq(cmd, "stat")) {
    if (argc != 2)
      return error(out, cmd, "provide one path");
    int n = vfs_lookup(argv[1]);
    if (n < 0)
      return verror(out, cmd);
    char path[VFS_PATH];
    vfs_path(n, path);
    put(out, "Path: ");
    put(out, path);
    put(out, "\nType: ");
    put(out, nodes[n].dir ? "directory" : "file");
    put(out, "\nSize: ");
    num(out, nodes[n].size);
    put(out, " bytes\nStorage: ");
    if (nodes[n].boot >= 0 || n == vfs_lookup("/boot")) {
      put(out, "FAT12 boot image (read-only)\n");
    } else {
      put(out, nodes[n].readonly ? "RAM (read-only)\n" : "RAM\n");
      int tracked = storage_tracks_node(n);
      int user_root = n == vfs_lookup("/home/user");
      if (tracked)
        put(out, "Persistence: user snapshot\n");
      else if (user_root)
        put(out,
            "Persistence: directory recreated at boot; descendants use user "
            "snapshots\n");
      else
        put(out, "Persistence: none (outside user snapshots)\n");
      if (tracked || user_root) {
        put(out, storage_ready
                     ? "Data disk: ready (use sync to commit changes)\n"
                     : "Data disk: unavailable (changes remain in RAM)\n");
      }
    }
    return 0;
  }
  if (streq(cmd, "head") || streq(cmd, "tail")) {
    u32 lines = 10;
    int a = 1;
    if (argc > 2 && streq(argv[1], "-n")) {
      if (!parse_number(argv[2], &lines))
        return error(out, cmd, "invalid line count");
      a = 3;
    }
    const u8* data;
    u32 size;
    if (read_input(c, a < argc ? argv[a] : 0, &data, &size))
      return 1;
    if (streq(cmd, "head")) {
      u32 end = 0, n = 0;
      while (end < size && n < lines)
        if (data[end++] == '\n')
          n++;
      emit(out, data, end);
    } else {
      u32 n = 0, start = size;
      if (start && data[start - 1] == '\n')
        start--;
      while (start && n < lines) {
        start--;
        if (data[start] == '\n')
          n++;
      }
      if (n == lines)
        start++;
      if (lines)
        emit(out, data + start, size - start);
    }
    return 0;
  }
  if (streq(cmd, "grep")) {
    int numbered = argc > 1 && streq(argv[1], "-n"), a = numbered ? 2 : 1;
    if (a >= argc)
      return error(out, cmd, "missing literal search text");
    const char* pattern = argv[a++];
    const u8* data;
    u32 size;
    if (read_input(c, a < argc ? argv[a] : 0, &data, &size))
      return 1;
    u32 begin = 0, row = 1;
    int found = 0;
    for (u32 i = 0; i <= size; i++)
      if (i == size || data[i] == '\n') {
        if (i > begin &&
            contains((const char*)data + begin, i - begin, pattern)) {
          if (numbered) {
            num(out, row);
            put(out, ":");
          }
          emit(out, data + begin, i - begin);
          put(out, "\n");
          found = 1;
        }
        begin = i + 1;
        row++;
      }
    return found ? 0 : 1;
  }
  if (streq(cmd, "wc")) {
    int a = 1, which = 0;
    if (argc > 1 && argv[1][0] == '-') {
      which = streq(argv[1], "-l")   ? 1
              : streq(argv[1], "-w") ? 2
              : streq(argv[1], "-c") ? 3
                                     : 0;
      if (!which)
        return error(out, cmd, "use -l, -w or -c");
      a++;
    }
    const u8* data;
    u32 size;
    if (read_input(c, a < argc ? argv[a] : 0, &data, &size))
      return 1;
    u32 rows = 0, words = 0;
    int inside = 0;
    for (u32 i = 0; i < size; i++) {
      if (data[i] == '\n')
        rows++;
      int space = data[i] == ' ' || data[i] == '\t' || data[i] == '\n' ||
                  data[i] == '\r';
      if (!space && !inside)
        words++;
      inside = !space;
    }
    if (!which || which == 1)
      num(out, rows);
    if (!which)
      put(out, " ");
    if (!which || which == 2)
      num(out, words);
    if (!which)
      put(out, " ");
    if (!which || which == 3)
      num(out, size);
    put(out, "\n");
    return 0;
  }
  if (streq(cmd, "hexdump")) {
    const u8* data;
    u32 size;
    if (read_input(c, argc > 1 ? argv[1] : 0, &data, &size))
      return 1;
    const char* hex = "0123456789abcdef";
    for (u32 off = 0; off < size; off += 16) {
      char address[9];
      for (u32 j = 0; j < 8; j++)
        address[j] = hex[(off >> (28 - j * 4)) & 15];
      address[8] = 0;
      put(out, address);
      put(out, "  ");
      for (u32 j = 0; j < 16; j++) {
        if (off + j < size) {
          char byte[3] = {hex[data[off + j] >> 4], hex[data[off + j] & 15], 0};
          put(out, byte);
          put(out, " ");
        } else
          put(out, "   ");
      }
      put(out, " |");
      for (u32 j = 0; j < 16 && off + j < size; j++) {
        char ch = data[off + j] >= 32 && data[off + j] <= 126
                      ? (char)data[off + j]
                      : '.';
        emit(out, &ch, 1);
      }
      put(out, "|\n");
    }
    return 0;
  }
  if (streq(cmd, "uname") || streq(cmd, "ver")) {
    put(out, "SINO");
    if (streq(cmd, "ver") || (argc > 1 && streq(argv[1], "-a"))) {
      put(out, " ");
      put(out, hostname);
      put(out, " 0.10 SINO32 i386 (ring-3 native processes; legacy desktop)");
    }
    put(out, "\n");
    return 0;
  }
  if (streq(cmd, "mem") || streq(cmd, "free")) {
    put(out, "Mapped usable range: ");
    num(out, usable_memory / 1024);
    put(out, " KiB\nFree memory: ");
    num(out, free_pages * 4);
    put(out, " KiB / ");
    num(out, free_pages);
    put(out, " pages (4 KiB each)\n");
    return 0;
  }
  if (streq(cmd, "df")) {
    u32 pages = 0;
    for (int i = 1; i < VFS_NODES; i++)
      if (nodes[i].used && !nodes[i].readonly)
        pages += (nodes[i].size + 4095) / 4096;
    put(out,
        "Mount   Type    Mode  Usage\n/boot   FAT12   ro    1440 KiB boot "
        "image\n/       RAMFS   rw    ");
    num(out, pages * 4);
    put(out, " KiB allocated; home is mirrored to the data disk\n");
    return 0;
  }
  if (streq(cmd, "uptime")) {
    put(out, "up ");
    num(out, ticks / 100);
    put(out, " seconds (100 Hz kernel clock)\n");
    return 0;
  }
  if (streq(cmd, "date") || streq(cmd, "time")) {
    return clock_output(out, streq(cmd, "date"));
  }
  if (streq(cmd, "whoami")) {
    put(out, "user\n");
    return 0;
  }
  if (streq(cmd, "hostname")) {
    if (argc > 1) {
      u32 n = strlen(argv[1]);
      if (!n || n >= sizeof(hostname))
        return error(out, cmd, "use 1-31 characters");
      for (u32 i = 0; i < n; i++)
        if (!(argv[1][i] >= 'a' && argv[1][i] <= 'z') &&
            !(argv[1][i] >= 'A' && argv[1][i] <= 'Z') &&
            !(argv[1][i] >= '0' && argv[1][i] <= '9') && argv[1][i] != '-')
          return error(out, cmd, "use letters, digits or hyphens");
      strcopy(hostname, argv[1]);
    }
    put(out, hostname);
    put(out, "\n");
    return 0;
  }
  if (streq(cmd, "env")) {
    const char* names[] = {"USER",  "HOME", "PWD",     "HOSTNAME",
                           "SHELL", "PATH", "VERSION", 0};
    for (int i = 0; names[i]; i++) {
      char b[VFS_PATH];
      put(out, names[i]);
      put(out, "=");
      put(out, variable(names[i], b));
      put(out, "\n");
    }
    return 0;
  }
  if (streq(cmd, "which")) {
    if (argc < 2)
      return error(out, cmd, "missing command");
    if (builtin(argv[1])) {
      put(out, argv[1]);
      put(out, " (shell builtin)\n");
      return 0;
    }
    return error(out, cmd,
                 "not found; use run /boot/hello.f32 for native programs");
  }
  if (streq(cmd, "history")) {
    return 0;
  }
  if (streq(cmd, "ps")) {
    ProcessInfo list[8];
    int count = process_list(list, 8);
    put(out, "PID  THREADS  PAGES  NATIVE APP\n");
    for (int i = 0; i < count; i++) {
      num(out, list[i].pid);
      put(out, "    ");
      num(out, list[i].threads);
      put(out, "        ");
      num(out, list[i].pages);
      put(out, "    ");
      put(out, list[i].name);
      put(out, "\n");
    }
    return 0;
  }
  if (streq(cmd, "about") || streq(cmd, "credits")) {
    put(out,
        "SINO 0.10 + SINO SH\nProject-specific C/assembly generated and "
        "written for this project.\nNot a FreeDOS or Linux kernel fork; SINO "
        "is this project's name.\nThird-party tools: NASM, GCC/Binutils, "
        "Node.js and QEMU.\nFirmware: SeaBIOS / VGA BIOS (QEMU).\nReferences include "
        "Intel architecture manuals and QEMU documentation.\nSource details: "
        "cat /boot/sources.txt\n");
    return 0;
  }
  if (streq(cmd, "kill")) {
    u32 pid;
    if (argc != 2 || !parse_number(argv[1], &pid))
      return error(out, cmd, "expected PID");
    return process_kill(pid, 0) < 0 ? error(out, cmd, "no such process") : 0;
  }
  if (streq(cmd, "nettest")) {
    return process_launch("/boot/nettest.snx", CAP_HOME | CAP_NETWORK) < 0
               ? error(out, cmd, "network test cannot start")
               : 0;
  }
  if (streq(cmd, "run")) {
    if (argc >= 2 && streq(argv[1], "-s")) {
      if (argc < 3 || argc > 4) return error(out, cmd, "run -s native.snx [argument]");
      int pid = process_launch_strict(argv[2], argc == 4 ? argv[3] : "");
      if (pid < 0) return error(out, cmd, "native launch failed");
      put(out, "Started user process "); num(out, (u32)pid); put(out, "\n");
      return 0;
    }
    if (argc < 2 || argc > 3)
      return error(out, cmd, "run native.snx [argument]");
    return run_program(c, argv[1], argc == 3 ? argv[2] : "");
  }
  if (streq(cmd, "source")) {
    if (argc != 2)
      return error(out, cmd, "provide one script file");
    if (c->depth >= SCRIPT_DEPTH - 1)
      return error(out, cmd, "script nesting limit is 4");
    int index = vfs_lookup(argv[1]);
    if (index < 0)
      return verror(out, cmd);
    int size = vfs_read(index, (u8*)scripts[c->depth], 4095);
    if (size < 0)
      return verror(out, cmd);
    scripts[c->depth][size] = 0;
    u32 p = 0;
    int status = 0;
    while (p < (u32)size) {
      char script_line[LINE_SIZE];
      u32 n = 0;
      while (p < (u32)size && scripts[c->depth][p] != '\n') {
        char ch = scripts[c->depth][p++];
        if (ch != '\r') {
          if (n >= LINE_SIZE - 1)
            return error(out, cmd, "script line exceeds 255 characters");
          script_line[n++] = ch;
        }
      }
      if (p < (u32)size)
        p++;
      script_line[n] = 0;
      status = execute(script_line, out, c->depth + 1);
    }
    return status;
  }
  if (streq(cmd, "sleep")) {
    u32 seconds;
    if (argc != 2 || !parse_number(argv[1], &seconds) || seconds > 3600)
      return error(out, cmd, "use 0-3600 seconds");
    u32 start = ticks;
    while (ticks - start < seconds * 100) {
      if (key_read() == 3)
        return 130;
      halt();
    }
    return 0;
  }
  if (streq(cmd, "sync")) {
    if (!storage_sync())
      return error(out, cmd, storage_error);
    put(out, "Home data saved.\n");
    return 0;
  }
  if (streq(cmd, "reboot")) {
    if (storage_ready && !storage_sync())
      return error(out, cmd, storage_error);
    put(out, "Restarting SINO...\n");
    for (u32 n = 0; n < 100000 && (inb(0x64) & 2); n++) {
    }
    outb(0x64, 0xfe);
    for (;;)
      halt();
  }
  if (streq(cmd, "halt")) {
    if (storage_ready && !storage_sync())
      return error(out, cmd, storage_error);
    put(out, "SINO halted. Close or reset the virtual machine.\n");
    cli();
    for (;;)
      halt();
  }
  return error(out, cmd, "command not found; type help");
}
static int execute_segment(Token* tokens, int begin, int end, Sink* outer,
                           int depth) {
  u32 input_length = 0;
  const u8* input = 0;
  int input_present = 0;
  int status = 0, stage = 0;
  while (begin < end) {
    int finish = begin;
    while (finish < end && tokens[finish].type != '|')
      finish++;
    if (finish == begin)
      return error(outer, "syntax", "empty pipeline stage");
    char* argv[TOKENS];
    int argc = 0, append = 0;
    char redirect[VFS_PATH];
    redirect[0] = 0;
    Context context = {input, input_length, input_present, outer, depth};
    for (int i = begin; i < finish; i++) {
      int type = tokens[i].type;
      if (type == '>' || type == '+' || type == '<') {
        if (i + 1 >= finish || tokens[i + 1].type)
          return error(outer, "syntax", "redirection requires a filename");
        const char* name = tokens[++i].text;
        if (strlen(name) >= VFS_PATH)
          return error(outer, "syntax", "path too long");
        if (type == '<') {
          if (input_present)
            return error(outer, "syntax", "input already provided");
          const u8* bytes;
          u32 size;
          if (read_input(&context, name, &bytes, &size))
            return 1;
          if (size > PIPE_SIZE)
            return error(outer, "input", "redirection exceeds 8192 bytes");
          u8* buffer = pipes[depth][(stage + 1) & 1];
          memcpy(buffer, bytes, size);
          input = buffer;
          input_length = size;
          input_present = 1;
          context.input = input;
          context.input_length = size;
          context.input_present = 1;
        } else {
          if (finish < end)
            return error(outer, "syntax",
                         "output redirection is allowed on the final stage");
          if (redirect[0])
            return error(outer, "syntax", "multiple output targets");
          strcopy(redirect, name);
          append = type == '+';
        }
      } else if (type)
        return error(outer, "syntax", "unexpected operator");
      else
        argv[argc++] = tokens[i].text;
    }
    if (!argc)
      return error(outer, "syntax", "missing command");
    Sink capture = {pipes[depth][stage & 1], 0, PIPE_SIZE, 0};
    int capturing = finish < end || redirect[0];
    if (capturing) {
      capture.data[0] = 0;
      context.out = &capture;
    }
    status = command(&context, argc, argv);
    if (capture.overflow)
      return error(outer, "output", "exceeds 8192-byte pipe/redirection limit");
    if (redirect[0] && !status) {
      int index = vfs_create(redirect, 0, 0);
      if (index < 0 ||
          vfs_write(index, capture.data, capture.length, append) < 0)
        return verror(outer, "redirect");
    }
    input = capture.data;
    input_length = capture.length;
    input_present = 1;
    stage++;
    begin = finish + 1;
  }
  return status;
}
static int execute(const char* s, Sink* out, int depth) {
  if (depth >= SCRIPT_DEPTH)
    return error(out, "source", "nesting limit exceeded");
  Token tokens[TOKENS];
  char segment[LINE_SIZE];
  u32 used = 0;
  char quote = 0;
  int escaped = 0, status = 0, comment = 0;
  for (u32 i = 0;; i++) {
    char ch = s[i];
    if (!quote && !escaped && ch == '#' &&
        (used == 0 || segment[used - 1] == ' ' || segment[used - 1] == '\t'))
      comment = 1;
    if (ch == 0 || comment || (!quote && !escaped && ch == ';')) {
      segment[used] = 0;
      int count = tokenize(segment, tokens);
      if (count < 0) {
        error(out, "syntax",
              "unclosed quote, too many tokens or argument too long");
        last_status = 2;
        return 2;
      }
      if (count) {
        status = execute_segment(tokens, 0, count, out, depth);
        last_status = status;
      } else if (ch == ';') {
        error(out, "syntax", "empty command");
        last_status = 2;
        return 2;
      }
      used = 0;
      if (!ch || comment)
        break;
      continue;
    }
    if (used >= LINE_SIZE - 1) {
      last_status = 2;
      error(out, "syntax", "command exceeds 255 characters");
      return 2;
    }
    segment[used++] = ch;
    if (escaped) {
      escaped = 0;
      continue;
    }
    if (ch == '\\' && quote != '\'') {
      escaped = 1;
      continue;
    }
    if ((ch == '\'' || ch == '"') && (!quote || quote == ch))
      quote = quote ? 0 : ch;
  }
  return status;
}
/* Development-only COM1 command transport. The normal shell parser and Sink
   produce the result. IRQ4 only stages bytes; framing, commands and replies
   run with bounded work in the main loop. SCS2 is specified in bounded-cli.md.
   Request: "SCS1", id:u32, length:u32, UTF-8 command (all fields
   little-endian). Reply: "SCR1", id:u32, status:i32, flags:u32, length:u32,
   captured bytes. flags bit 0 means output overflow. One request is outstanding
   at a time. */
#ifdef SINO_SHELL_BRIDGE
#define BRIDGE_UART 0x3f8
static u8* bridge_request; /* Two pages, only in explicit command-channel builds. */
static u8* bridge_rx;
static volatile u32 bridge_rx_head, bridge_rx_tail, bridge_rx_overflow;
static u8 bridge_output[PIPE_SIZE + 1];
static u32 bridge_reply[5];
static u32 bridge_received, bridge_needed, bridge_sent, bridge_size,
    bridge_updated;

void shell_bridge_init(void) {
  bridge_request = page_alloc_run(2);
  bridge_rx = page_alloc_run(2);
  if (!bridge_request || !bridge_rx) {
    if (bridge_request) page_free_run(bridge_request, 2);
    if (bridge_rx) page_free_run(bridge_rx, 2);
    bridge_request = bridge_rx = 0;
    pr_debug("SINO SHELL BRIDGE allocation failed\n"); return;
  }
  bridge_rx_head = bridge_rx_tail = bridge_rx_overflow = 0;
  run_bridge_init();
  outb(BRIDGE_UART + 1, 0);
  outb(BRIDGE_UART + 3, 0x80);
  outb(BRIDGE_UART, 1); /* 115200 baud, 8N1. */
  outb(BRIDGE_UART + 1, 0);
  outb(BRIDGE_UART + 3, 3);
  outb(BRIDGE_UART + 2, 7);
  outb(BRIDGE_UART + 4, 11); /* DTR/RTS/OUT2 routes the UART IRQ. */
  bridge_received = bridge_sent = bridge_size = 0;
  bridge_needed = 12;
  outb(0x21, inb(0x21) & (u8)~16u);
  outb(BRIDGE_UART + 1, 1); /* Receive only; no parsing or allocation in IRQ. */
  pr_debug("SINO SHELL BRIDGE v1 ready\n");
}

void shell_bridge_irq(void) {
  if (!bridge_rx) return;
  for (u32 budget = 0; budget < 16; budget++) {
    u8 status = inb(BRIDGE_UART + 5);
    if (status & 0x1e) bridge_rx_overflow = 1;
    if (!(status & 1)) break;
    u8 byte = inb(BRIDGE_UART);
    u32 next = (bridge_rx_head + 1) & 8191u;
    if (next == bridge_rx_tail) { bridge_rx_overflow = 1; continue; }
    bridge_rx[bridge_rx_head] = byte;
    __asm__ volatile("" ::: "memory");
    bridge_rx_head = next;
  }
}

static void bridge_complete(int status, u32 flags, u32 size) {
  bridge_reply[0] = *(u32*)bridge_request == 0x32534353u ? 0x32524353u : 0x31524353u;
  bridge_reply[1] = *(u32*)(bridge_request + 4);
  bridge_reply[2] = (u32)status;
  bridge_reply[3] = flags;
  bridge_reply[4] = size;
  bridge_size = sizeof(bridge_reply) + size;
  bridge_sent = 0;
  bridge_received = 0;
  bridge_needed = 12;
}

void shell_bridge_poll(void) {
  if (!bridge_request) return;
  /* Also drain a bounded burst in case an edge was coalesced while the FIFO
     remained nonempty. Mask IRQs so the ring still has a single producer. */
  u32 receive_flags;
  __asm__ volatile("pushfl; popl %0; cli":"=r"(receive_flags)::"memory");
  shell_bridge_irq();
  if (receive_flags & 0x200) sti();
  if (bridge_rx_overflow) {
    u32 flags; __asm__ volatile("pushfl; popl %0; cli":"=r"(flags)::"memory");
    bridge_rx_tail = bridge_rx_head; bridge_rx_overflow = 0;
    if (flags & 0x200) sti();
    bridge_received = bridge_size = bridge_sent = 0; bridge_needed = 12;
    run_bridge_abort(); pr_debug("SHELL BRIDGE RX overflow; session aborted\n");
    return;
  }
  run_bridge_tick();
  /* Never wait for the peer to receive output. */
  if (bridge_size) {
    for (u32 budget = 0; budget < 128 && bridge_sent < bridge_size;) {
      if (!(inb(BRIDGE_UART + 5) & 0x20))
        return;
      /* FCR enables the 16550's 16-byte TX FIFO. THRE means it is empty;
         fill one bounded burst instead of waiting one timer tick per byte. */
      for (u32 burst = 0; burst < 16 && bridge_sent < bridge_size && budget < 128; burst++, budget++) {
        u32 offset = bridge_sent++;
        u8 byte = offset < sizeof(bridge_reply)
                      ? ((u8*)bridge_reply)[offset]
                      : bridge_output[offset - sizeof(bridge_reply)];
        outb(BRIDGE_UART, byte);
      }
    }
    if (bridge_sent == bridge_size)
      bridge_size = 0;
    return;
  }
  /* Discard an incomplete frame after two seconds. Host timeout remains
     responsible for commands that do not return (e.g. halt or sleep). */
  if (bridge_received && ticks - bridge_updated > 200) {
    bridge_received = 0;
    bridge_needed = 12;
  }
  for (u32 budget = 0; budget < 128 && bridge_rx_tail != bridge_rx_head; budget++) {
    __asm__ volatile("" ::: "memory");
    u8 byte = bridge_rx[bridge_rx_tail];
    __asm__ volatile("" ::: "memory");
    bridge_rx_tail = (bridge_rx_tail + 1) & 8191u;
    bridge_request[bridge_received++] = byte;
    bridge_updated = ticks;
    if (bridge_received == 4 && *(u32*)bridge_request != 0x31534353u &&
        *(u32*)bridge_request != 0x32534353u) {
      /* Find a frame boundary after a corrupt prefix. */
      memcpy(bridge_request, bridge_request + 1, 3);
      bridge_received = 3;
    }
    if (bridge_received == 12) {
      u32 size = *(u32*)(bridge_request + 8);
      if ((*(u32*)bridge_request == 0x31534353u && size >= LINE_SIZE) ||
          size > RUN_BRIDGE_PAYLOAD) {
        bridge_complete(S_INVALID, 0, 0);
        return;
      }
      bridge_needed = 12 + size;
    }
    if (bridge_received == bridge_needed) {
      u32 size = bridge_needed - 12;
      if (*(u32*)bridge_request == 0x32534353u) {
        int status;
        u32 output = run_bridge_request(bridge_request + 12, size,
            *(u32*)(bridge_request + 4), bridge_output, &status);
        bridge_complete(status, 0, output);
        return;
      }
      for (u32 i = 0; i < size; i++) {
        u8 byte = bridge_request[12 + i];
        if (!byte || byte == '\n' || byte == '\r') {
          bridge_complete(S_INVALID, 0, 0);
          return;
        }
      }
      bridge_request[12 + size] = 0;
      Sink captured = {bridge_output, 0, PIPE_SIZE, 0};
      int status = execute((char*)bridge_request + 12, &captured, 0);
      bridge_complete(status, captured.overflow ? 1u : 0u, captured.length);
      return;
    }
  }
}
#else
void shell_bridge_init(void) {
}
void shell_bridge_poll(void) {
}
void shell_bridge_irq(void) {
}
#endif

#ifdef SELFTEST
static u8 test_output[PIPE_SIZE + 1];
static void check(int ok, const char* label) {
  if (!ok)
    panic(label, (u32)last_status, free_pages);
}
static void test_command(const char* command_line, const char* expected,
                         int status) {
  Sink out = {test_output, 0, PIPE_SIZE, 0};
  test_output[0] = 0;
  pr_debug("SHELL TEST: ");
  pr_debug(command_line);
  pr_debug("\n");
  int actual = execute(command_line, &out, 0);
  if (actual != status || out.overflow ||
      (expected && !contains((const char*)test_output, out.length, expected))) {
    pr_debug("TEST OUTPUT: ");
    pr_debug((const char*)test_output);
    pr_debug("\n");
    panic("Shell command self-check", (u32)actual, (u32)status);
  }
}
static void file_is(const char* path, const char* expected) {
  int n = vfs_lookup(path);
  check(n >= 0, "Test file exists");
  int bytes = vfs_read(n, work, MAX_FILE);
  check(bytes >= 0, "Test file read");
  work[bytes] = 0;
  check(streq((const char*)work, expected), "Test file contents");
}
void shell_tests(void) {
  check(*(volatile u16*)0x4f04 == 0x144 && *(volatile u8*)0x4f06 == 16,
        "VBE graphics and 8x16 font");
  test_command("pwd", "/home/user", 0);
  test_command("mkdir -p projects/demo", 0, 0);
  test_command("cd projects/demo; echo $PWD", "/home/user/projects/demo", 0);
  test_command("cd ~; cd -; pwd", "/home/user/projects/demo", 0);
  test_command("touch .hidden; ls -la", ".hidden", 0);
  test_command("echo \"alpha beta\" > notes.txt", 0, 0);
  test_command("echo gamma >> notes.txt", 0, 0);
  file_is("notes.txt", "alpha beta\ngamma\n");
  test_command("cp notes.txt copy.txt; mv copy.txt finished.txt", 0, 0);
  file_is("finished.txt", "alpha beta\ngamma\n");
  test_command("cat notes.txt | grep beta > matches.txt", 0, 0);
  file_is("matches.txt", "alpha beta\n");
  test_command("head -n 1 notes.txt", "alpha beta\n", 0);
  test_command("tail -n 1 notes.txt", "gamma\n", 0);
  test_command("cat < notes.txt | wc -l", "2\n", 0);
  test_command("echo one two | wc", "1 2 8\n", 0);
  test_command("echo \"hello world\" > \"two words.txt\"", 0, 0);
  file_is("two words.txt", "hello world\n");
  test_command("echo -e \"one\\ntwo\" > escaped.txt", 0, 0);
  file_is("escaped.txt", "one\ntwo\n");
  test_command("echo '$HOME'", "$HOME", 0);
  test_command("echo $HOME", "/home/user", 0);
  test_command("cat missing.txt", "no such file", 1);
  test_command("echo $?", "1\n", 0);
  test_command("echo hacked > /boot/readme.txt", "read-only", 1);
  test_command("mkdir tree; rmdir tree", 0, 0);
  test_command("mkdir -p tree/child; rmdir tree", "not empty", 1);
  test_command("mv tree tree/child", "itself", 1);
  test_command("echo 'echo script-ok > scripted.txt' > job.sh; source job.sh",
               0, 0);
  file_is("scripted.txt", "script-ok\n");
  test_command("echo 'source loop.sh' > loop.sh; source loop.sh",
               "nesting limit", 1);
  test_command("echo \"unterminated", "unclosed quote", 2);
  test_command("echo literal # comment; touch should-not-exist", "literal", 0);
  check(vfs_lookup("should-not-exist") < 0, "Comment suppresses commands");
  int large = vfs_create("large.bin", 0, 0);
  check(large >= 0, "Large RAM file create");
  for (u32 i = 0; i < MAX_FILE; i++)
    work[i] = (u8)i;
  u32 before = free_pages;
  check(vfs_write(large, work, MAX_FILE, 0) == 0 && free_pages + 16 == before,
        "RAM file page allocation");
  check(vfs_write(large, work, 1, 1) < 0 && nodes[large].size == MAX_FILE,
        "RAM file size limit");
  test_command("cat large.bin | wc", "8192-byte", 1);
  check(vfs_remove("large.bin", 0) == 0 && free_pages == before,
        "RAM file page reclamation");
  test_command("stat notes.txt", "Persistence: user snapshot", 0);
  test_command("grep -n gamma notes.txt", "2:gamma", 0);
  test_command("hexdump matches.txt", "61 6c 70 68 61", 0);
  before = free_pages;
  test_command("run /boot/hello.f32", "Returned safely to SINO shell.", 0);
  check(free_pages == before, "Native app page reclamation");
  test_command("source /boot/demo.sh", "hello from SINO SH", 0);
  test_command("credits", "Not a FreeDOS or Linux kernel fork", 0);
  test_command("date", "2026-", 0);
  pr_debug("SINO SH FILES PARSER PIPELINES SCRIPTS TEST PASS\n");
  execute("cd /home/user", 0, 0);
}
#endif
#endif
