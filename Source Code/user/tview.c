/* G4 strict read-only tools. The kernel supplies handles and byte streams;
 * listing policy, text sanitizing and paging stay in this user program. */
#include "stream.h"
#include "file_text.h"
#include "text_scan.h"
static CliInfo cli;
static int root, terminal, file, paged;
static FileStat original;
static u8 input[512];
static char output[512];
static u32 offset, position, available, used, range_end;
static FileLayout layout;
static u32 columns = 120, rows = 25;
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
    if (offset == range_end)
      return -1;
    u32 count = range_end - offset;
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
  range_end = original.length;
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
/* Range output and scans share the same checked file identity. No line-size
 * buffer is needed: grep rescans selected ranges and tail uses two passes. */
static int block(u32 start, u8 *buffer, u32 count) {
  int s = unchanged();
  if (s < 0)
    return s;
  s = FileReadAt(file, start, buffer, count);
  if (s < 0)
    return s;
  if ((u32)s != count)
    return S_CHANGED;
  return unchanged();
}
static int show_range(u32 start, u32 end) {
  offset = start;
  range_end = end;
  position = available = 0;
  FileText decoder = {0};
  u32 cp;
  int s;
  while ((s = file_text_next(&decoder, get, 0, &cp)) > 0) {
    s = emit(cp);
    if (s)
      break;
  }
  if (!s)
    s = unchanged();
  if (flush() < 0)
    s = S_IO;
  if (layout.column) {
    if (put("\n") < 0)
      s = S_IO;
    layout.column = 0;
  }
  return s;
}
static int text_tool(const char *cmd, const char *path) {
  file = OpenAt(root, path, FS_READ, 0);
  if (file < 0)
    return failure(file);
  int s = FileGetStat(file, &original);
  if (s < 0)
    return failure(s);
  dimensions();
  u32 argc = cli.argc, numbered = 0, count = 10, which = 0;
  const char *pattern = "";
  if (streq(cmd, "head") || streq(cmd, "tail")) {
    if (argc == 4 && streq(cli.bytes + cli.offsets[1], "-n")) {
      if (text_unsigned(cli.bytes + cli.offsets[2], &count)) {
        put("invalid line count\n");
        return 2;
      }
    } else if (argc != 2) {
      put("invalid head/tail arguments\n");
      return 2;
    }
  } else if (streq(cmd, "wc")) {
    if (argc == 3) {
      const char *flag = cli.bytes + cli.offsets[1];
      which = streq(flag, "-l")   ? 1
              : streq(flag, "-w") ? 2
              : streq(flag, "-c") ? 3
                                  : 4;
      if (which == 4) {
        put("wc: use -l, -w or -c\n");
        return 2;
      }
    } else if (argc != 2)
      return 2;
  } else if (streq(cmd, "grep")) {
    numbered = argc > 1 && streq(cli.bytes + cli.offsets[1], "-n");
    if (argc != (numbered ? 4u : 3u))
      return 2;
    pattern = cli.bytes + cli.offsets[numbered ? 2 : 1];
  }
  if (streq(cmd, "hexdump")) {
    const char *hex = "0123456789abcdef";
    u8 bytes[16];
    for (u32 off = 0; off < original.length; off += 16) {
      u32 n = original.length - off;
      if (n > 16)
        n = 16;
      s = block(off, bytes, n);
      if (s < 0)
        return failure(s);
      char line[88];
      u32 at = 0;
      for (u32 i = 0; i < 8; i++)
        line[at++] = hex[(off >> (28 - i * 4)) & 15];
      line[at++] = ' ';
      line[at++] = ' ';
      for (u32 i = 0; i < 16; i++) {
        line[at++] = i < n ? hex[bytes[i] >> 4] : ' ';
        line[at++] = i < n ? hex[bytes[i] & 15] : ' ';
        line[at++] = ' ';
      }
      line[at++] = ' ';
      line[at++] = '|';
      for (u32 i = 0; i < n; i++)
        line[at++] = bytes[i] >= 32 && bytes[i] < 127 ? (char)bytes[i] : '.';
      line[at++] = '|';
      line[at++] = '\n';
      if (write_bytes(line, at) < 0)
        return 1;
    }
    return unchanged() < 0 ? failure(S_CHANGED) : 0;
  }
  TextCounts counts = {0};
  TextMatch match;
  int found = 0, hit = !*pattern;
  u32 row = 1, begin = 0, head_end = 0;
  u8 buffer[512], last = 0;
  if (text_pattern(&match, pattern, strlen(pattern)))
    return 2;
  for (u32 off = 0; off < original.length;) {
    u32 n = original.length - off;
    if (n > sizeof(buffer))
      n = sizeof(buffer);
    s = block(off, buffer, n);
    if (s < 0)
      return failure(s);
    for (u32 i = 0; i < n; i++) {
      u8 b = buffer[i];
      text_count(&counts, b);
      last = b;
      if (counts.lines <= count && !head_end && b == '\n' &&
          counts.lines == count)
        head_end = off + i + 1;
      if (streq(cmd, "grep")) {
        if (b != '\n' && text_match(&match, b))
          hit = 1;
        if (b == '\n' || off + i + 1 == original.length) {
          if (hit) {
            if (numbered && (digit(row) < 0 || put(":") < 0))
              return 1;
            s = show_range(begin, off + i + 1);
            if (s < 0)
              return failure(s);
            found = 1;
          }
          row++;
          begin = off + i + 1;
          hit = !*pattern;
          match.matched = 0;
        }
      }
    }
    off += n;
  }
  if (streq(cmd, "wc")) {
    if (!which || which == 1)
      if (digit(counts.lines) < 0)
        return 1;
    if (!which && put(" ") < 0)
      return 1;
    if (!which || which == 2)
      if (digit(counts.words) < 0)
        return 1;
    if (!which && put(" ") < 0)
      return 1;
    if (!which || which == 3)
      if (digit(counts.bytes) < 0)
        return 1;
    return put("\n") < 0;
  }
  if (streq(cmd, "grep"))
    return unchanged() < 0 ? failure(S_CHANGED) : found ? 0 : 1;
  if (streq(cmd, "head")) {
    s = show_range(0, count ? (head_end ? head_end : original.length) : 0);
    return s < 0 ? failure(s) : 0;
  }
  u32 lines = counts.lines + (original.length && last != '\n'),
      skip = lines > count ? lines - count : 0, start = 0;
  if (!count)
    return 0;
  if (skip)
    for (u32 off = 0; off < original.length && skip;) {
      u32 n = original.length - off;
      if (n > sizeof(buffer))
        n = sizeof(buffer);
      s = block(off, buffer, n);
      if (s < 0)
        return failure(s);
      for (u32 i = 0; i < n; i++)
        if (buffer[i] == '\n' && !--skip) {
          start = off + i + 1;
          break;
        }
      off += n;
    }
  s = show_range(start, original.length);
  return s < 0 ? failure(s) : 0;
}
/* DFS paths live in bounded user BSS; only one traversal directory is open.
 * Every directory retains its own enumeration revision, not a tree snapshot. */
static char walk_paths[128][128];
static int walk(const char *path, const char *pattern, int tree) {
  u32 pending = 1, visited = 0;
  copy(walk_paths[0], path);
  while (pending) {
    char current[128];
    copy(current, walk_paths[--pending]);
    if (++visited > 128)
      return failure(S_LIMIT);
    int h =
        OpenAt(root, current, FS_READ | FS_LOOKUP | FS_ENUM, OPEN_DIRECTORY);
    if (h < 0) {
      h = OpenAt(root, current, FS_READ, 0);
      if (h < 0)
        return failure(h);
      Close(h);
    } else {
      DirEntry entries[8];
      DirPage page = {0, 0, (u32)entries, 8, 0, 0};
      int s = 0;
      do {
        s = DirectoryRead(h, &page);
        if (s < 0)
          break;
        for (u32 i = 0; i < page.count; i++) {
          u32 size = streq(current, ".") ? 0 : strlen(current);
          u32 length = strlen(entries[i].name);
          if (size + (size != 0) + length >= 128 || pending == 128) {
            s = S_LIMIT;
            break;
          }
          char *next = walk_paths[pending++];
          if (size) {
            copy(next, current);
            next[size++] = '/';
          }
          copy(next + size, entries[i].name);
        }
      } while (!page.done && s >= 0);
      Close(h);
      if (s < 0)
        return failure(s);
    }
    TextMatch m;
    int match = !*pattern;
    if (text_pattern(&m, pattern, strlen(pattern)))
      return 2;
    for (const char *p = current; *p; p++)
      if (text_match(&m, (u8)*p))
        match = 1;
    if (tree || match) {
      if (put("/") < 0 || (!streq(current, ".") && put(current) < 0) ||
          put("\n") < 0)
        return 1;
    }
  }
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
  if (streq(cmd, "tree") || streq(cmd, "find")) {
    if (cli.argc < 2 || cli.argc > 3)
      return 2;
    return walk(cli.bytes + cli.offsets[1],
                cli.argc == 3 ? cli.bytes + cli.offsets[2] : "",
                streq(cmd, "tree"));
  }
  if (streq(cmd, "head") || streq(cmd, "tail") || streq(cmd, "wc") ||
      streq(cmd, "grep") || streq(cmd, "hexdump"))
    return text_tool(cmd, cli.bytes + cli.offsets[cli.argc - 1]);
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
