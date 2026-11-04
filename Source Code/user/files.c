/* Ring 3 Files owns directory policy and UI. Paint has been hidden 261103. */
#include "sdk.h"
#define ITEMS 128
static FileInfo items[ITEMS], scratch[ITEMS];
static char paths[ITEMS][128], folder[128] = "/home/user", status[128];
static u8 transfer[65536];
static UiCanvas canvas;
static UiDropZone zones[DROP_ZONES];
static int window, count, selected = -1, first, width = 650, height = 460,
                          language;
static int prompt, operation, input_selected, context, cx, cy, context_file;
static char answer[128], question[128];
static int last_selected = -1;
static u32 last_click;
static const char *sides[] = {
    "/home/user", "/home/user/Desktop", "/home/user/Documents", "/Applications",
    "/boot",      "/home/user/Trash"};
static const char *tr(const char *zh, const char *en) {
  return language ? en : zh;
}
static int suffix(const char *s, const char *end) {
  u32 n = strlen(s), m = strlen(end);
  if (n < m)
    return 0;
  for (u32 i = 0; i < m; i++) {
    char a = s[n - m + i];
    if (a >= 'A' && a <= 'Z')
      a += 32;
    if (a != end[i])
      return 0;
  }
  return 1;
}
static const char *leaf(const char *p) {
  const char *r = p;
  for (; *p; p++)
    if (*p == '/')
      r = p + 1;
  return r;
}
static int join(char *out, const char *base, const char *name) {
  u32 n = strlen(base), m = strlen(name);
  if (n + m + 2 > 128)
    return 0;
  copy(out, base);
  if (n > 1)
    out[n++] = '/';
  copy(out + n, name);
  return 1;
}
static int valid_name(const char *s) {
  if (!*s || strlen(s) > 31 || streq(s, ".") || streq(s, ".."))
    return 0;
  for (; *s; s++)
    if (*s == '/' || *s == '\\')
      return 0;
  return 1;
}
static void parent(char *p) {
  u32 n = strlen(p);
  while (n > 1 && p[n - 1] != '/')
    n--;
  if (n > 1)
    n--;
  p[n] = 0;
}
static const char *folder_title(void) {
  if (streq(folder, "/"))
    return "/";
  if (streq(folder, sides[0]))
    return tr("个人目录", "Home");
  if (streq(folder, sides[1]))
    return tr("桌面", "Desktop");
  if (streq(folder, sides[2]))
    return tr("文稿", "Documents");
  if (streq(folder, sides[3]))
    return tr("应用程序", "Applications");
  if (streq(folder, sides[4]))
    return tr("启动磁盘", "Startup Disk");
  if (streq(folder, sides[5]))
    return tr("废纸篓", "Trash");
  return leaf(folder);
}
static int hidden(const char *path, const FileInfo *info) {
  if (info->dir)
    return 0;
  if (strlen(info->name) == 9 && suffix(info->name, "paint.app"))
    return 1;
  if (suffix(info->name, ".lnk")) {
    char b[200];
    int n = Read(path, b, 199);
    if (n > 0) {
      b[n] = 0;
      for (int i = 0; b[i]; i++)
        if (b[i] == '\n' || b[i] == '\r') {
          b[i] = 0;
          break;
        }
      const char *p = leaf(b);
      return strlen(p) == 9 && suffix(p, "paint.app");
    }
  }
  return 0;
}
static int exists(const char *p) {
  u8 b;
  int r = Read(p, &b, 1);
  return r != S_NOTFOUND;
}
static int visible_rows(void) {
  int r = (height - 139) / 28;
  return r < 1 ? 1 : r > 24 ? 24 : r;
}
static void append(const char *base, int links) {
  int n = List(base, scratch, ITEMS);
  if (n < 0)
    return;
  for (int i = 0; i < n && count < ITEMS; i++) {
    if (links && !suffix(scratch[i].name, ".lnk"))
      continue;
    if (!join(paths[count], base, scratch[i].name) ||
        hidden(paths[count], &scratch[i]))
      continue;
    items[count++] = scratch[i];
  }
}
static void refresh(void) {
  char old[128];
  old[0] = 0;
  if (selected >= 0 && selected < count)
    copy(old, paths[selected]);
  int r = List(folder, scratch, ITEMS);
  if (r < 0) {
    copy(status, tr("目录已不存在，返回个人目录。",
                    "Directory unavailable; returning Home."));
    copy(folder, sides[0]);
  }
  count = 0;
  append(folder, 0);
  if (streq(folder, "/Applications"))
    append("/home/user/.sino/menu", 1);
  selected = -1;
  for (int i = 0; i < count; i++)
    if (streq(paths[i], old))
      selected = i;
  int max = count - visible_rows();
  if (max < 0)
    max = 0;
  if (first > max)
    first = max;
  if (first < 0)
    first = 0;
  WindowTitle(window, folder_title());
}
static void set_folder(const char *p) {
  last_selected = -1;
  if (strlen(p) >= 128)
    return;
  copy(folder, p);
  selected = -1;
  first = 0;
  refresh();
}
static void ask(int op, const char *text, const char *initial) {
  last_selected = -1;
  operation = op;
  prompt = 1;
  context = 0;
  copy(question, text);
  copy(answer, initial);
  input_selected = 1;
  DropZones(window, 0, 0);
}
static void notice(const char *text) {
  last_selected = -1;
  prompt = 2;
  context = 0;
  copy(question, text);
  DropZones(window, 0, 0);
}
static void failure(int r) {
  if (r < 0)
    notice(tr("操作失败：检查只读位置、重名、容量或文件是否仍存在。",
              "Operation failed: check permissions, conflicts, capacity or "
              "stale paths."));
}
static int commit(int r) {
  if (r >= 0)
    r = Sync();
  if (r < 0) {
    failure(r);
    return r;
  }
  copy(status, tr("已保存更改。", "Changes saved."));
  refresh();
  return 0;
}
static void open_selected(void) {
  if (selected < 0 || selected >= count) {
    ask(15, tr("打开（完整路径）", "Open full path"), folder);
    return;
  }
  if (items[selected].dir) {
    char p[128];
    copy(p, paths[selected]);
    set_folder(p);
  } else
    failure(OpenPath(paths[selected]));
}
static void clip(int cut) {
  if (selected < 0 || selected >= count)
    return;
  FileClipboard c;
  copy(c.path, paths[selected]);
  c.cut = (u32)cut;
  failure(FileClip(1, &c));
}
static void paste(void) {
  FileClipboard c;
  int r = FileClip(0, &c);
  if (r < 0) {
    failure(r);
    return;
  }
  char dest[128], name[32];
  copy(name, leaf(c.path));
  if (!join(dest, folder, name)) {
    failure(S_LIMIT);
    return;
  }
  if (!c.cut && exists(dest)) {
    for (int i = 1; i <= 128; i++) {
      char prefix[20], num[12];
      if (i == 1)
        copy(prefix, "Copy of ");
      else {
        copy(prefix, "Copy ");
        number((u32)i, num);
        copy(prefix + 5, num);
        copy(prefix + strlen(prefix), " ");
      }
      u32 n = strlen(prefix), j = 0;
      copy(name, prefix);
      const char *base = leaf(c.path);
      while (base[j] && n + j < 31) {
        name[n + j] = base[j];
        j++;
      }
      while (j && ((u8)base[j] & 0xc0) == 0x80)
        j--;
      name[n + j] = 0;
      if (!join(dest, folder, name)) {
        failure(S_LIMIT);
        return;
      }
      if (!exists(dest))
        break;
      if (i == 128) {
        failure(S_LIMIT);
        return;
      }
    }
  }
  if (c.cut) {
    r = api(SYS_MOVE, (u32)c.path, (u32)dest, 0);
    if (r >= 0)
      FileClip(2, &c);
  } else {
    int n = Read(c.path, transfer, sizeof(transfer));
    r = n < 0 ? n : Write(dest, transfer, (u32)n);
  }
  commit(r);
}
static void remove_selected(void) {
  if (selected < 0 || selected >= count)
    return;
  int r =
      streq(folder, "/home/user/Trash")
          ? Remove(paths[selected], (int)items[selected].dir)
          : api(SYS_MOVE, (u32)paths[selected], (u32) "/home/user/Trash", 0);
  commit(r);
}
static void properties(void) {
  if (selected < 0 || selected >= count)
    return;
  char info[128], n[12];
  copy(info, items[selected].name);
  copy(info + strlen(info), items[selected].dir ? tr("  文件夹  ", "  Folder  ")
                                                : tr("  文件  ", "  File  "));
  number(items[selected].size, n);
  copy(info + strlen(info), n);
  notice(info);
}
static void command(int cmd) {
  last_selected = -1;
  switch (cmd) {
    case 1:
      failure(SpawnArgs("/boot/files.snx", folder));
      break;
    case 2:
      ask(2, tr("新建文件夹", "New folder"), tr("新建文件夹", "New Folder"));
      break;
    case 3:
      ask(3, tr("新建文本文件", "New text file"), "Untitled.txt");
      break;
    case 6:
      open_selected();
      break;
    case 8:
      if (selected >= 0)
        ask(8, tr("重命名", "Rename"), items[selected].name);
      break;
    case 9:
      remove_selected();
      break;
    case 10:
      failure(OpenPath(sides[0]));
      break;
    case 11:
      failure(OpenPath(sides[3]));
      break;
    case 12:
      refresh();
      break;
    case 14:
      if (selected >= 0)
        ask(14, tr("移动到（完整路径）", "Move to full path"), paths[selected]);
      break;
    case 15:
      ask(15, tr("打开（完整路径）", "Open full path"), folder);
      break;
    case 16:
      properties();
      break;
  }
}
static void accept(void) {
  char path[128];
  int r = S_INVALID, op = operation;
  if (op == 15) {
    r = OpenPath(answer);
    if (r >= 0)
      prompt = 0;
    else
      failure(r);
    return;
  }
  if (op == 2 || op == 3 || op == 8) {
    if (!valid_name(answer) || !join(path, folder, answer)) {
      failure(S_INVALID);
      return;
    }
  }
  prompt = 0;
  if (op == 2)
    r = Mkdir(path);
  else if (op == 3) {
    if (exists(path))
      r = S_EXISTS;
    else
      r = Write(path, "", 0);
  } else if (op == 8 && selected >= 0)
    r = api(SYS_MOVE, (u32)paths[selected], (u32)path, 0);
  else if (op == 14 && selected >= 0)
    r = api(SYS_MOVE, (u32)paths[selected], (u32)answer, 0);
  if (commit(r) >= 0 && op == 3)
    failure(OpenPath(path));
}
static void text(int x, int y, const char *s, u32 color) {
  char b[80];
  u32 n = strlen(s);
  if (n > 79) {
    n = 79;
    while (n && ((u8)s[n] & 0xc0) == 0x80)
      n--;
  }
  memcpy(b, s, n);
  b[n] = 0;
  ui_canvas_text(&canvas, x, y, 0, b, color);
}
static void fill(int x, int y, int w, int h, u32 c) {
  ui_canvas_fill(&canvas, x, y, w, h, c);
}
static void button(int x, int y, int w, const char *s) {
  fill(x, y, w, 26, 0xe4e5e8);
  text(x + 5, y + 3, s, 0x24252a);
}
static void zone(int *n, int x, int y, int w, int h, u32 flags,
                 const char *path) {
  if (*n >= DROP_ZONES || strlen(path) >= 128)
    return;
  UiDropZone *z = &zones[(*n)++];
  *z = (UiDropZone){x, y, w, h, flags, {0}};
  copy(z->path, path);
}
static void draw(void) {
  ui_canvas_begin(&canvas);
  fill(2, 30, width - 4, height - 32, 0xffffff);
  int zn = 0;
  if (prompt) {
    fill(24, 65, width - 48, 260, 0xe5e9ef);
    text(40, 86, question, 0x24252a);
    if (prompt == 1) {
      fill(40, 150, width - 80, 30, input_selected ? 0xdce8f8 : 0xffffff);
      text(44, 154, answer, 0x24252a);
    }
    text(40, 220, tr("Esc 取消", "Esc Cancel"), 0x24252a);
    button(330, 265, 120, "OK / Enter");
    Scene(window, (UiScene *)&canvas);
    return;
  }
  fill(2, 30, width - 4, 40, 0xe4e5e8);
  button(12, 37, 44, tr("上级", "Up"));
  button(66, 37, 95, tr("新建目录", "New Folder"));
  button(171, 37, 95, tr("新建文本", "New Text"));
  button(276, 37, 68, tr("打开", "Open"));
  button(354, 37, 70, tr("废纸篓", "Trash"));
  fill(2, 78, 135, height - 80, 0xe7e9ed);
  const char *zh[] = {"个人目录", "桌面",     "文稿",
                      "应用程序", "启动磁盘", "废纸篓"},
             *en[] = {"Home",         "Desktop",      "Documents",
                      "Applications", "Startup Disk", "Trash"};
  for (int j = 0; j < 6; j++) {
    int y = 93 + j * 37;
    if (y + 30 > height - 28)
      break;
    if (streq(folder, sides[j]))
      fill(5, y, 128, 30, 0xc4d0e1);
    text(16, y + 4, language ? en[j] : zh[j], 0x24252a);
    zone(&zn, 5, y, 128, 30, DROP_TARGET, sides[j]);
  }
  text(150, 78, tr("名称", "Name"), 0x656a72);
  text(width - 109, 78, tr("大小", "Size"), 0x656a72);
  zone(&zn, 142, 106, width - 154, height - 134, DROP_TARGET, folder);
  int rows = visible_rows();
  for (int row = 0; row < rows && row + first < count; row++) {
    int i = row + first, y = 106 + row * 28;
    u32 col = i == selected ? 0xffffff : 0x24252a;
    if (i == selected)
      fill(142, y, width - 154, 28, 0x355e9a);
    else if (row & 1)
      fill(142, y, width - 154, 28, 0xf1f3f5);
    text(181, y + 3, items[i].name, col);
    char n[16];
    number(items[i].size, n);
    text(width - 108, y + 3, items[i].dir ? tr("文件夹", "Folder") : n, col);
    zone(&zn, 142, y, width - 154, 28,
         DROP_SOURCE | (items[i].dir ? DROP_TARGET : 0), paths[i]);
  }
  if (count > rows) {
    button(width - 31, 34, 20, "^");
    button(width - 31, height - 55, 20, "v");
  }
  fill(2, height - 27, width - 4, 25, 0xd6d7db);
  text(12, height - 25, folder, 0x555b65);
  if (status[0])
    text(145, height - 48, status, 0xa43d43);
  if (context) {
    const char *file[] = {
        tr("打开", "Open"),      tr("重命名", "Rename"),
        tr("复制", "Copy"),      tr("剪切", "Cut"),
        tr("粘贴", "Paste"),     tr("移到废纸篓 / 删除", "Trash / Delete"),
        tr("属性", "Properties")};
    const char *blank[] = {tr("新建文件夹", "New Folder"),
                           tr("新建文本文件", "New Text"), tr("粘贴", "Paste"),
                           tr("刷新", "Refresh")};
    for (int j = 0; j < (context_file ? 7 : 4); j++)
      button(cx, cy + j * 28, 220, context_file ? file[j] : blank[j]);
  }
  Scene(window, (UiScene *)&canvas);
  DropZones(window, context ? 0 : zones, context ? 0u : (u32)zn);
}
static int row_at(int x, int y) {
  if (x < 142 || x > width - 12 || y < 106 || y >= height - 28)
    return -1;
  int i = (y - 106) / 28 + first;
  return i < count ? i : -1;
}
static void key(UiEvent *e) {
  if (prompt) {
    if (e->type == MSG_KEY && e->key == 27) {
      prompt = 0;
      return;
    }
    if (e->type == MSG_KEY && e->key == 13) {
      if (prompt == 2)
        prompt = 0;
      else
        accept();
      return;
    }
    if (prompt == 2)
      return;
    if (e->type == MSG_KEY && e->key == 1) {
      input_selected = 1;
      return;
    }
    if (e->type == MSG_KEY && e->key == 8) {
      if (input_selected) {
        answer[0] = 0;
        input_selected = 0;
      } else
        answer[text_previous(answer, strlen(answer))] = 0;
      return;
    }
    char encoded[4];
    int n = 0;
    if (e->type == MSG_TEXT)
      n = text_encode((u32)e->key, encoded);
    else if (e->key >= 32 && e->key < 127) {
      encoded[0] = (char)e->key;
      n = 1;
    }
    if (n) {
      u32 at = input_selected ? 0 : strlen(answer);
      if (at + (u32)n < 128) {
        memcpy(answer + at, encoded, (u32)n);
        answer[at + n] = 0;
        input_selected = 0;
      }
    }
    return;
  }
  if (e->type != MSG_KEY)
    return;
  last_selected = -1;
  if (context) {
    if (e->key == 27)
      context = 0;
    return;
  }
  if (e->key == 3 || e->key == 24)
    clip(e->key == 24);
  else if (e->key == 22)
    paste();
  else if (e->key == 15)
    command(15);
  else if (e->key == 14)
    command(1);
  else if (e->key == 13)
    open_selected();
  else if (e->key == 0x153)
    remove_selected();
  else if (e->key == 8) {
    char p[128];
    copy(p, folder);
    parent(p);
    set_folder(p);
  } else if (e->key == 0x148 || e->key == 0x150) {
    if (count) {
      selected += e->key == 0x148 ? -1 : 1;
      if (selected < 0)
        selected = 0;
      if (selected >= count)
        selected = count - 1;
      if (selected < first)
        first = selected;
      if (selected >= first + visible_rows())
        first = selected - visible_rows() + 1;
    }
  }
}
static int require(int ok, const char *message) {
  if (!ok) {
    Log("FILES SELFTEST FAIL: ");
    Log(message);
    Exit(1);
  }
  return ok;
}
static void selftest(void) {
  WindowSize size = {0, 0};
  UiDropZone z = {0, 30, 20, 20, DROP_TARGET, {0}};
  copy(z.path, "/home/user");
  require(GetWindowSize(window, &size) == 0 && size.width == 650,
          "window dimensions");
  require(
      api(SYS_WINDOW_SIZE, (u32)window, USER_BASE, sizeof(size)) == S_INVALID,
      "readonly size output");
  require(api(SYS_WINDOW_SIZE, (u32)window, USER_STACK(0) - 4, sizeof(size)) ==
              S_INVALID,
          "size guard crossing");
  require(api(SYS_WINDOW_SIZE, (u32)window, (u32)&size, 7) == S_INVALID,
          "size capacity");
  require(api(SYS_WINDOW_SIZE, 0, (u32)&size, sizeof(size)) == S_BADHANDLE,
          "size wrong handle");
  int ro = api(SYS_DUP, (u32)window, 1, 0);
  require(ro > 0 && DropZones(ro, &z, 1) == S_BADHANDLE, "zone reduced rights");
  Close(ro);
  require(DropZones(window, &z, 1) == 0, "valid target");
  z.w = -1;
  require(DropZones(window, &z, 1) == S_INVALID, "zone negative geometry");
  z.w = 20;
  z.x = 650;
  require(DropZones(window, &z, 1) == S_INVALID, "zone bounds");
  z.x = 0;
  z.flags = 4;
  require(DropZones(window, &z, 1) == S_INVALID, "zone flags");
  z.flags = DROP_TARGET;
  copy(z.path, "/boot/readme.txt");
  require(DropZones(window, &z, 1) == S_INVALID,
          "file is not target directory");
  require(api(SYS_DROP_ZONES, (u32)window, USER_STACK(0) - 4, 1) == S_INVALID,
          "zone pointer range");
  require(DropZones(window, &z, DROP_ZONES + 1) == S_INVALID, "zone count");
  require(api(SYS_FILE_CLIP, 0, USER_BASE, 0) == S_INVALID,
          "clipboard readonly output");
  require(api(SYS_FILE_CLIP, 1, 0, 0) == S_INVALID, "clipboard invalid input");
  FileClipboard c;
  copy(c.path, "/boot/readme.txt");
  c.cut = 1;
  require(FileClip(1, &c) == S_DENIED, "cut protected source");
  c.cut = 2;
  require(FileClip(1, &c) == S_INVALID, "clipboard flags");
  require(api(SYS_OPEN_PATH, 0xffffffffu, 0, 0) == S_INVALID,
          "open invalid string");
  require(Write("/boot/files.snx", "x", 1) == S_DENIED, "protected boot");
  require(api(SYS_SOCKET, 1, 0, 0) == S_DENIED, "network denied");
  require(api(SYS_KILL, 0xdeadbeefu, 0, 0) == S_DENIED, "manage denied");
  DropZones(window, 0, 0);
  Log("FILES SELFTEST PASS: window rights pointers drag zones clipboard and "
      "capabilities\n");
}
int main(void) {
  const char *args = Arguments();
  int test = streq(args, "/selftest"), fault = streq(args, "/fault-test");
  if (!test && !fault && args[0] && strlen(args) < 128)
    copy(folder, args);
  language = UiLanguage();
  window = Window("Files", 650, 460);
  if (window < 0)
    return 1;
  api(SYS_WINDOW_POLICY, (u32)window, WINDOW_DESKTOP_COMMANDS, 0);
  refresh();
  draw();
  if (test) {
    selftest();
    return 0;
  }
  if (fault) {
    Yield(10);
    __asm__ volatile("ud2");
  }
  u32 polled = (u32)api(SYS_TICKS, 0, 0, 0);
  for (;;) {
    UiEvent e;
    int got = Event(&e) == 0, redraw = got;
    if (got) {
      if (e.type == MSG_KEY || e.type == MSG_TEXT)
        key(&e);
      else if (e.type == MSG_FILES_CHANGED)
        refresh();
      else if (e.type == MSG_SCROLL) {
        first += e.key;
        refresh();
      } else if (e.type == MSG_COMMAND)
        command(e.key);
      else if (e.type == MSG_CONTEXT && !prompt) {
        selected = row_at(e.x, e.y);
        context_file = selected >= 0;
        context = 1;
        cx = e.x > width - 224 ? width - 224 : e.x;
        cy = e.y;
        if (cy < 32)
          cy = 32;
        int max = height - 12 - (context_file ? 7 : 4) * 28;
        if (cy > max)
          cy = max;
      } else if (e.type == MSG_CLICK) {
        if (prompt) {
          if (e.x >= 330 && e.x < 450 && e.y >= 265 && e.y < 291) {
            if (prompt == 2)
              prompt = 0;
            else
              accept();
          }
        } else if (context) {
          int row =
              e.x >= cx && e.x < cx + 220 && e.y >= cy ? (e.y - cy) / 28 : -1;
          context = 0;
          if (row >= 0 && row < (context_file ? 7 : 4)) {
            if (context_file) {
              if (row == 2 || row == 3)
                clip(row == 3);
              else if (row == 4)
                paste();
              else {
                const int cmds[] = {6, 8, 0, 0, 0, 9, 16};
                command(cmds[row]);
              }
            } else if (row == 2)
              paste();
            else {
              const int cmds[] = {2, 3, 0, 12};
              command(cmds[row]);
            }
          }
        } else if (e.x > width - 35 && e.y >= 34 && e.y < 61) {
          first -= visible_rows();
          refresh();
        } else if (e.x > width - 35 && e.y >= height - 55 &&
                   e.y < height - 29) {
          first += visible_rows();
          refresh();
        } else if (e.y >= 35 && e.y < 66) {
          if (e.x < 60) {
            char p[128];
            copy(p, folder);
            parent(p);
            set_folder(p);
          } else if (e.x < 165)
            command(2);
          else if (e.x < 270)
            command(3);
          else if (e.x < 350)
            command(6);
          else if (e.x < 430)
            command(9);
        } else if (e.x < 137 && e.y >= 93 && e.y < 315)
          set_folder(sides[(e.y - 93) / 37]);
        else {
          selected = row_at(e.x, e.y);
          u32 now = (u32)api(SYS_TICKS, 0, 0, 0);
          if (selected >= 0 && selected == last_selected &&
              now - last_click < 40) {
            last_selected = -1;
            open_selected();
          } else {
            last_selected = selected;
            last_click = now;
          }
        }
      }
    }
    u32 now = (u32)api(SYS_TICKS, 0, 0, 0);
    if (now - polled >= 10) {
      polled = now;
      WindowSize size;
      if (!GetWindowSize(window, &size) &&
          ((int)size.width != width || (int)size.height != height)) {
        width = (int)size.width;
        height = (int)size.height;
        refresh();
        redraw = 1;
      }
      int lang = UiLanguage();
      if (lang != language) {
        language = lang;
        refresh();
        redraw = 1;
      }
    }
    if (redraw)
      draw();
    if (!got)
      Yield(1);
  }
}
