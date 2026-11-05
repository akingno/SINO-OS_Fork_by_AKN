/* G4 strict read-only tools. The kernel supplies handles and byte streams;
 * listing policy, text sanitizing and paging stay in this user program. */
#include "stream.h"
#include "file_text.h"
static CliInfo cli;
static int root, terminal, file, paged;
static FileStat original;
static u8 input[512];
static char output[512];
static u32 offset, position, available, used;
static FileLayout layout;
static u32 columns = 80, rows = 25;
static int write_bytes(const char *p, u32 size) {
  u32 n;
  return StreamWriteAllFor(cli.out, p, size, 300, &n);
}
static int flush(void) {
  int s = used ? write_bytes(output, used) : 0;
  used = 0;
  return s;
}
static int put(const char *p) {
  if (flush() < 0)
    return S_IO;
  return write_bytes(p, strlen(p));
}
static int digit(u32 n) {
  char b[12];
  number(n, b);
  return put(b);
}
static int failure(int error) {
  if (error == S_CHANGED)
    put("file changed; reopen\n");
  else {
    put("file error ");
    if (error < 0) {
      put("-");
      digit(0u - (u32)error);
    } else
      digit((u32)error);
    put("\n");
  }
  return 1;
}
static int unchanged(void) {
  FileStat now;
  int s = FileGetStat(file, &now);
  if (s < 0)
    return s;
  return now.generation == original.generation &&
                 now.revision == original.revision &&
                 now.length == original.length
             ? 0
             : S_CHANGED;
}
static int get(void *unused) {
  (void)unused;
  if (position == available) {
    int s = unchanged();
    if (s < 0)
      return s == S_INVALID ? S_IO : s;
    if (offset == original.length)
      return -1;
    u32 count = original.length - offset;
    if (count > sizeof(input))
      count = sizeof(input);
    s = FileReadAt(file, offset, input, count);
    if (s < 0)
      return s == S_INVALID ? S_IO : s;
    if ((u32)s != count)
      return S_CHANGED;
    int check = unchanged();
    if (check < 0)
      return check == S_INVALID ? S_IO : check;
    offset += (u32)s;
    position = 0;
    available = (u32)s;
  }
  return input[position++];
}
static void dimensions(void) {
  TerminalInfo info;
  if (TerminalGetInfo(terminal, &info) == 0) {
    columns = info.width < 4 ? 4 : info.width;
    rows = info.height;
  }
}
/* Returns 1 when the user requests normal early completion. */
static int pause_page(void) {
  int s = unchanged();
  if (s < 0)
    return s;
  if (layout.column) {
    if (put("\n") < 0)
      return S_IO;
    layout.column = 0;
  }
  if (put("-- more: Space / q --\n") < 0)
    return S_IO;
  for (;;) {
    TerminalEvent e;
    while (TerminalNextEvent(terminal, &e) == 0) {
      if (e.type == TERM_RESIZE) {
        dimensions();
        if (layout.column > columns)
          layout.column = columns;
      }
    }
    char c;
    int n = StreamRead(cli.in, &c, 1);
    if (n == 0)
      return 1;
    if (n == S_AGAIN) {
      Yield(1);
      continue;
    }
    if (n < 0)
      return n;
    if (c == 'q' || c == 'Q')
      return 1;
    if (c == ' ') {
      s = unchanged();
      if (s < 0)
        return s;
      dimensions();
      layout.lines = 0;
      return 0;
    }
  }
}
static int emit(u32 cp) {
  if (cp == '\t') {
    u32 spaces = 4 - layout.column % 4;
    while (spaces--) {
      int s = emit(' ');
      if (s)
        return s;
    }
    return 0;
  }
  if (paged) {
    TerminalEvent e;
    while (TerminalNextEvent(terminal, &e) == 0)
      if (e.type == TERM_RESIZE)
        dimensions();
    if (layout.lines >= (rows > 3 ? rows - 3 : 1)) {
      int s = pause_page();
      if (s)
        return s;
    }
  }
  if (file_text_wrap(&layout, columns, cp)) {
    if (paged) {
      if (used == sizeof(output) && flush() < 0)
        return S_IO;
      output[used++] = '\n';
    }
    layout.column = 0;
    layout.lines++;
    if (paged && layout.lines >= (rows > 3 ? rows - 3 : 1)) {
      int s = pause_page();
      if (s)
        return s;
    }
  }
  char bytes[4];
  u32 count = file_text_encode(cp, bytes);
  if (used + count > sizeof(output) && flush() < 0)
    return S_IO;
  memcpy(output + used, bytes, count);
  used += count;
  if (cp == '\n') {
    layout.column = 0;
    layout.lines++;
  } else
    layout.column += cp >= 128 ? 3 : 1;
  return 0;
}
static int read_file(const char *path) {
  file = OpenAt(root, path, FS_READ, 0);
  if (file < 0)
    return failure(file);
  int s = FileGetStat(file, &original);
  if (s < 0)
    return failure(s);
  dimensions();
  if (paged) {
    s = TerminalMode(terminal, 0);
    if (s < 0)
      return failure(s);
  }
  FileText decoder = {0};
  u32 cp;
  while ((s = file_text_next(&decoder, get, 0, &cp)) > 0) {
    s = emit(cp);
    if (s) {
      break;
    }
  }
  if (!s)
    s = unchanged();
  if (flush() < 0)
    s = S_IO;
  if (layout.column) {
    put("\n");
    layout.column = 0;
  }
  if (paged)
    TerminalMode(terminal, TERM_LINE | TERM_ECHO);
  Close(file);
  return s < 0 ? failure(s) : 0;
}
static int listing(const char *path, int all, int detailed) {
  int dir = OpenAt(root, path, FS_READ | FS_LOOKUP | FS_ENUM, OPEN_DIRECTORY);
  if (dir < 0)
    return failure(dir);
  DirEntry entries[8];
  DirPage page = {0, 0, (u32)entries, 8, 0, 0};
  int s;
  do {
    s = DirectoryRead(dir, &page);
    if (s < 0)
      break;
    for (u32 i = 0; i < page.count; i++) {
      DirEntry *e = entries + i;
      if (!all && e->name[0] == '.')
        continue;
      if (detailed) {
        if (put(e->dir ? "d " : "f ") < 0 || digit(e->length) < 0 ||
            put(" ") < 0) {
          s = S_IO;
          break;
        }
      }
      /* Names originate from the VFS but still cross the text boundary. */
      for (u32 j = 0; e->name[j]; j++)
        if ((u8)e->name[j] < 32 || e->name[j] == 127)
          e->name[j] = '?';
      if (put(e->name) < 0 || put(e->dir ? "/\n" : "\n") < 0) {
        s = S_IO;
        break;
      }
    }
  } while (!page.done && s >= 0);
  Close(dir);
  return s < 0 ? failure(s) : 0;
}
static int stat_file(const char *path) {
  int h = OpenAt(root, path, FS_READ, 0);
  if (h < 0)
    h = OpenAt(root, path, FS_READ | FS_LOOKUP, OPEN_DIRECTORY);
  if (h < 0)
    return failure(h);
  FileStat st;
  int s = FileGetStat(h, &st);
  Close(h);
  if (s < 0)
    return failure(s);
  if (put(st.kind == FILE_DIRECTORY ? "kind=directory\n" : "kind=file\n") < 0 ||
      put("length=") < 0 || digit(st.length) < 0 || put("\nrevision=") < 0 ||
      digit(st.revision) < 0 || put("\n") < 0)
    return 1;
  return 0;
}
int main(void) {
  StartInfo start;
  terminal = TerminalCurrent();
  root = StartDirectory();
  if (QueryStartInfo(&start) || start.policy != PROCESS_STRICT || start.caps ||
      QueryCliInfo(&cli) || terminal < 0 || root < 0 || !cli.argc)
    return 2;
  const char *cmd = cli.bytes + cli.offsets[0];
  int ls = streq(cmd, "ls");
  paged = streq(cmd, "view");
  if (!ls && !paged && !streq(cmd, "cat") && !streq(cmd, "stat"))
    return 2;
  int all = 0, detailed = 0;
  u32 index = 1;
  if (ls && index < cli.argc && cli.bytes[cli.offsets[index]] == '-') {
    const char *option = cli.bytes + cli.offsets[index++];
    if (streq(option, "-a"))
      all = 1;
    else if (streq(option, "-l"))
      detailed = 1;
    else if (streq(option, "-la") || streq(option, "-al"))
      all = detailed = 1;
    else {
      put("usage: ls [-a|-l|-la|-al] [path]\n");
      return 2;
    }
  }
  /* gsh always supplies one normalized root-relative path, '.' for root. */
  if (index + 1 != cli.argc) {
    put("file tool: expected one path\n");
    return 2;
  }
  const char *path = cli.bytes + cli.offsets[index];
  return ls                   ? listing(path, all, detailed)
         : streq(cmd, "stat") ? stat_file(path)
                              : read_file(path);
}
