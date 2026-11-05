#include "klog.h"
#include "vfs.h"
#include "storage.h"
#include "api.h"
Node nodes[VFS_NODES];
int cwd;
u32 vfs_generation[VFS_NODES];
u32 vfs_revision[VFS_NODES], vfs_refs[VFS_NODES];
static u32 next_generation;
static u32 next_revision;
static int revision_room(u32 count) { return next_revision <= ~0u - count; }
static void changed(int index) {
  vfs_revision[index] = ++next_revision;
  if (index) vfs_revision[nodes[index].parent] = ++next_revision;
  storage_changed();
}
/* Any live descendant reference also anchors every ancestor against moves. */
int vfs_busy(int index) {
  for (int i = 0; i < VFS_NODES; i++) if (nodes[i].used && vfs_refs[i]) {
    int p = i;
    for (;;) {
      if (p == index) return 1;
      if (!p) break;
      p = nodes[p].parent;
    }
  }
  return 0;
}
const char* vfs_error = "unknown error";
TextKey vfs_error_key = VFS_ERROR_UNKNOWN_ERROR;
static int vfs_fail(const char* s, TextKey key) {
  vfs_error = s;
  vfs_error_key = key;
  return -1;
}
static void copy(char* d, const char* s) {
  while ((*d++ = *s++)) {
  }
}
static int equal(const char* a, const char* b, int fold) {
  while (*a && *b) {
    char x = *a++, y = *b++;
    if (fold) {
      if (x >= 'A' && x <= 'Z')
        x += 32;
      if (y >= 'A' && y <= 'Z')
        y += 32;
    }
    if (x != y)
      return 0;
  }
  return *a == *b;
}
void vfs_path(int index, char* dest) {
  int chain[VFS_NODES], n = 0;
  while (index > 0 && n < VFS_NODES) {
    chain[n++] = index;
    index = nodes[index].parent;
  }
  u32 p = 0;
  dest[p++] = '/';
  while (n) {
    const char* s = nodes[chain[--n]].name;
    while (*s && p < VFS_PATH - 1)
      dest[p++] = *s++;
    if (n && p < VFS_PATH - 1)
      dest[p++] = '/';
  }
  dest[p] = 0;
}
int vfs_canonical(const char* path, char* out) {
  char input[2 * VFS_PATH];
  u32 length = 0;
  if (!path || !path[0])
    path = ".";
  if (path[0] == '~' && (path[1] == 0 || path[1] == '/')) {
    copy(input, "/home/user");
    length = 10;
    path++;
  } else if ((path[0] == 'A' || path[0] == 'a') && path[1] == ':') {
    copy(input, "/boot/");
    length = 6;
    path += 2;
    while (*path == '/' || *path == '\\')
      path++;
  } else if (*path != '/' && *path != '\\') {
    vfs_path(cwd, input);
    length = strlen(input);
    if (length > 1)
      input[length++] = '/';
  }
  for (; *path; path++) {
    if (length >= sizeof(input) - 1)
      return vfs_fail("path too long", VFS_ERROR_PATH_TOO_LONG);
    input[length++] = *path == '\\' ? '/' : *path;
  }
  input[length] = 0;
  u32 pos = 1;
  out[0] = '/';
  for (u32 i = 0; i < length;) {
    while (input[i] == '/')
      i++;
    char part[VFS_NAME];
    u32 n = 0;
    while (input[i] && input[i] != '/') {
      if (n >= VFS_NAME - 1)
        return vfs_fail("file name too long (31 characters max)",
                        VFS_ERROR_NAME_TOO_LONG_31_UTF_8_BYTES_MAXIMUM);
      part[n++] = input[i++];
    }
    part[n] = 0;
    if (!n)
      break;
    if (equal(part, ".", 0))
      continue;
    if (equal(part, "..", 0)) {
      if (pos > 1) {
        while (pos > 1 && out[pos - 1] != '/')
          pos--;
        if (pos > 1)
          pos--;
      }
      continue;
    }
    if (pos > 1) {
      if (pos >= VFS_PATH - 1)
        return vfs_fail("path too long", VFS_ERROR_PATH_TOO_LONG);
      out[pos++] = '/';
    }
    if (pos + n >= VFS_PATH)
      return vfs_fail("path too long", VFS_ERROR_PATH_TOO_LONG);
    for (u32 j = 0; j < n; j++)
      out[pos++] = part[j];
  }
  out[pos] = 0;
  return 0;
}
int vfs_child(int parent, const char* name) {
  for (int i = 1; i < VFS_NODES; i++)
    if (nodes[i].used && nodes[i].parent == parent &&
        equal(name, nodes[i].name, nodes[parent].readonly))
      return i;
  return -1;
}
static int absolute(const char* path) {
  int index = 0;
  u32 i = 1;
  while (path[i]) {
    char part[VFS_NAME];
    u32 n = 0;
    while (path[i] && path[i] != '/')
      part[n++] = path[i++];
    part[n] = 0;
    if (path[i] == '/')
      i++;
    if (!nodes[index].dir)
      return vfs_fail("not a directory", VFS_ERROR_NOT_A_FOLDER);
    index = vfs_child(index, part);
    if (index < 0)
      return vfs_fail("no such file or directory",
                      VFS_ERROR_FILE_OR_FOLDER_NOT_FOUND);
  }
  return index;
}
int vfs_lookup(const char* path) {
  char p[VFS_PATH];
  if (vfs_canonical(path, p) < 0)
    return -1;
  return absolute(p);
}
static int parent_name(const char* path, int* parent, char* name) {
  char p[VFS_PATH];
  if (vfs_canonical(path, p) < 0)
    return -1;
  u32 n = strlen(p), last = n;
  while (last > 0 && p[last - 1] != '/')
    last--;
  if (last == n)
    return vfs_fail("cannot change the root directory",
                    VFS_ERROR_THE_ROOT_FOLDER_CANNOT_BE_CHANGED);
  copy(name, p + last);
  if (last > 1)
    p[last - 1] = 0;
  else
    p[1] = 0;
  *parent = absolute(p);
  if (*parent < 0)
    return -1;
  if (!nodes[*parent].dir)
    return vfs_fail("not a directory", VFS_ERROR_NOT_A_FOLDER);
  if (nodes[*parent].readonly)
    return vfs_fail("read-only file system (/boot)",
                    VFS_ERROR_READ_ONLY_LOCATION_CHOOSE_HOME_OR_DESKTOP);
  return 0;
}
static int allocate(int parent, const char* name, int dir) {
  if (next_generation == ~0u || !revision_room(2))
    return vfs_fail("version limit", VFS_ERROR_NODE_LIMIT);
  for (int i = 1; i < VFS_NODES; i++)
    if (!nodes[i].used) {
      memset(&nodes[i], 0, sizeof(Node));
      nodes[i].used = 1;
      vfs_generation[i] = ++next_generation;
      nodes[i].parent = parent;
      nodes[i].dir = dir;
      nodes[i].boot = -1;
      copy(nodes[i].name, name);
      changed(i);
      return i;
    }
  return vfs_fail("File table is full (128 nodes)", VFS_ERROR_NODE_LIMIT);
}
int vfs_create(const char* path, int dir, int parents) {
  if (parents && dir) {
    char p[VFS_PATH];
    if (vfs_canonical(path, p) < 0)
      return -1;
    int parent = 0;
    for (u32 i = 1; p[i];) {
      char name[VFS_NAME];
      u32 n = 0;
      while (p[i] && p[i] != '/')
        name[n++] = p[i++];
      name[n] = 0;
      if (p[i] == '/')
        i++;
      int next = vfs_child(parent, name);
      if (next < 0) {
        if (nodes[parent].readonly)
          return vfs_fail("read-only file system (/boot)",
                          VFS_ERROR_READ_ONLY_LOCATION_CHOOSE_HOME_OR_DESKTOP);
        next = allocate(parent, name, 1);
      }
      if (next < 0)
        return -1;
      if (!nodes[next].dir)
        return vfs_fail("not a directory", VFS_ERROR_NOT_A_FOLDER);
      parent = next;
    }
    return parent;
  }
  int parent;
  char name[VFS_NAME];
  if (parent_name(path, &parent, name) < 0)
    return -1;
  int old = vfs_child(parent, name);
  if (old >= 0) {
    if (!dir && !nodes[old].dir)
      return old;
    return vfs_fail("file already exists",
                    VFS_ERROR_AN_ITEM_WITH_THAT_NAME_ALREADY_EXISTS);
  }
  return allocate(parent, name, dir);
}
int vfs_read(int index, u8* dest, u32 cap) {
  if (index < 0 || index >= VFS_NODES || !nodes[index].used)
    return vfs_fail("no such file", VFS_ERROR_FILE_NOT_FOUND);
  Node* n = &nodes[index];
  if (n->dir)
    return vfs_fail("is a directory", VFS_ERROR_THIS_ITEM_IS_A_FOLDER);
  if (n->size > cap)
    return vfs_fail("file exceeds read buffer", VFS_ERROR_FILE_IS_TOO_LARGE);
  if (n->boot >= 0)
    return fs_read((u32)n->boot, dest, cap)
               ? (int)n->size
               : vfs_fail("FAT12 read failed", VFS_ERROR_FAT12_READ_FAILED);
  for (u32 off = 0; off < n->size;) {
    u32 part = n->size - off;
    if (part > 4096)
      part = 4096;
    memcpy(dest + off, n->pages[off / 4096], part);
    off += part;
  }
  return (int)n->size;
}
int vfs_write(int index, const u8* data, u32 length, int append) {
  if (index < 0 || index >= VFS_NODES || !nodes[index].used)
    return vfs_fail("no such file", VFS_ERROR_FILE_NOT_FOUND);
  Node* n = &nodes[index];
  if (n->dir)
    return vfs_fail("is a directory", VFS_ERROR_THIS_ITEM_IS_A_FOLDER);
  if (n->readonly)
    return vfs_fail("read-only file system (/boot)",
                    VFS_ERROR_READ_ONLY_LOCATION_CHOOSE_HOME_OR_DESKTOP);
  u32 offset = append ? n->size : 0;
  if (length > MAX_FILE - offset)
    return vfs_fail("RAM file limit is 64 KB",
                    VFS_ERROR_A_FILE_CAN_CONTAIN_AT_MOST_64_KB);
  if (!revision_room(2))
    return vfs_fail("version limit", VFS_ERROR_NODE_LIMIT);
  u32 total = offset + length, old = (n->size + 4095) / 4096,
      needed = (total + 4095) / 4096, allocated = old;
  while (allocated < needed) {
    n->pages[allocated] = page_alloc();
    if (!n->pages[allocated]) {
      while (allocated > old) {
        allocated--;
        page_free(n->pages[allocated]);
        n->pages[allocated] = 0;
      }
      return vfs_fail("out of memory", VFS_ERROR_NOT_ENOUGH_MEMORY);
    }
    allocated++;
  }
  for (u32 i = 0; i < length; i++)
    n->pages[(offset + i) / 4096][(offset + i) % 4096] = data[i];
  while (old > needed) {
    old--;
    page_free(n->pages[old]);
    n->pages[old] = 0;
  }
  n->size = total;
  changed(index);
  return 0;
}
int vfs_remove(const char* path, int dir) {
  int index = vfs_lookup(path);
  if (index < 0)
    return -1;
  Node* n = &nodes[index];
  if (vfs_busy(index)) {
    vfs_fail("open object is busy", VFS_ERROR_THIS_ITEM_IS_PROTECTED_OR_READ_ONLY);
    return S_BUSY;
  }
  if (n->pinned || n->readonly)
    return vfs_fail("protected or read-only path",
                    VFS_ERROR_THIS_ITEM_IS_PROTECTED_OR_READ_ONLY);
  if (n->dir != dir)
    return vfs_fail(
        dir ? "not a directory" : "is a directory; use rmdir",
        dir ? VFS_ERROR_NOT_A_FOLDER : VFS_ERROR_IS_A_DIRECTORY_USE_RMDIR);
  if (index == cwd)
    return vfs_fail("cannot remove current directory",
                    VFS_ERROR_THE_CURRENT_FOLDER_CANNOT_BE_REMOVED);
  for (int i = 1; i < VFS_NODES; i++)
    if (nodes[i].used && nodes[i].parent == index)
      return vfs_fail("directory is not empty",
                      VFS_ERROR_THIS_FOLDER_IS_NOT_EMPTY_MOVE_ITS_FILES_FIRST);
  if (!revision_room(2))
    return vfs_fail("version limit", VFS_ERROR_NODE_LIMIT);
  changed(index);
  for (u32 i = 0; i < VFS_PAGES; i++)
    if (n->pages[i])
      page_free(n->pages[i]);
  memset(n, 0, sizeof(*n));
  storage_changed();
  return 0;
}
static int destination(const char* path, const char* name, int* parent,
                       char* out) {
  int index = vfs_lookup(path);
  if (index >= 0 && nodes[index].dir) {
    if (nodes[index].readonly)
      return vfs_fail("read-only file system (/boot)",
                      VFS_ERROR_READ_ONLY_LOCATION_CHOOSE_HOME_OR_DESKTOP);
    char prefix[VFS_PATH];
    vfs_path(index, prefix);
    if (strlen(prefix) + (strlen(prefix) > 1) + strlen(name) >= VFS_PATH)
      return vfs_fail("path too long", VFS_ERROR_PATH_TOO_LONG);
    *parent = index;
    copy(out, name);
    return 0;
  }
  return parent_name(path, parent, out);
}
int vfs_move(const char* source, const char* dest) {
  int index = vfs_lookup(source);
  if (index < 0)
    return -1;
  Node* n = &nodes[index];
  if (vfs_busy(index)) {
    vfs_fail("open object is busy", VFS_ERROR_THIS_ITEM_IS_PROTECTED_OR_READ_ONLY);
    return S_BUSY;
  }
  if (n->readonly || n->pinned)
    return vfs_fail("protected or read-only path",
                    VFS_ERROR_THIS_ITEM_IS_PROTECTED_OR_READ_ONLY);
  int parent;
  char name[VFS_NAME];
  if (destination(dest, n->name, &parent, name) < 0)
    return -1;
  for (int p = parent; p > 0; p = nodes[p].parent)
    if (p == index)
      return vfs_fail(
          "cannot move a directory into itself",
          VFS_ERROR_A_FOLDER_CANNOT_BE_MOVED_INTO_ITSELF_OR_ITS_DESCENDANTS);
  int existing = vfs_child(parent, name);
  if (existing >= 0 && existing != index)
    return vfs_fail("destination exists",
                    VFS_ERROR_THE_DESTINATION_ALREADY_CONTAINS_THAT_NAME);
  char prefix[VFS_PATH];
  vfs_path(parent, prefix);
  u32 base = strlen(prefix) + (strlen(prefix) > 1) + strlen(name);
  for (int i = 1; i < VFS_NODES; i++)
    if (nodes[i].used) {
      u32 relative = 0;
      int ancestor = i;
      while (ancestor > 0 && ancestor != index) {
        relative += strlen(nodes[ancestor].name) + 1;
        ancestor = nodes[ancestor].parent;
      }
      if (ancestor == index && base + relative >= VFS_PATH)
        return vfs_fail("path too long", VFS_ERROR_PATH_TOO_LONG);
    }
  if (!revision_room(3))
    return vfs_fail("version limit", VFS_ERROR_NODE_LIMIT);
  vfs_revision[n->parent] = ++next_revision;
  n->parent = parent;
  copy(n->name, name);
  changed(index);
  return 0;
}
int vfs_copy(const char* source, const char* dest, u8* buffer) {
  int index = vfs_lookup(source);
  if (index < 0)
    return -1;
  int length = vfs_read(index, buffer, MAX_FILE);
  if (length < 0)
    return -1;
  int parent;
  char name[VFS_NAME];
  if (destination(dest, nodes[index].name, &parent, name) < 0)
    return -1;
  int target = vfs_child(parent, name);
  if (target < 0)
    target = allocate(parent, name, 0);
  if (target < 0)
    return -1;
  return vfs_write(target, buffer, (u32)length, 0);
}
/* Range access never places a whole file on a kernel or user stack. */
int vfs_read_at(int index, u32 offset, u8* dest, u32 length) {
  if (index < 0 || index >= VFS_NODES || !nodes[index].used) return S_NOTFOUND;
  Node* n = &nodes[index];
  if (n->dir || offset > MAX_FILE || length > MAX_FILE - offset) return S_INVALID;
  if (offset >= n->size || !length) return 0;
  if (length > n->size - offset) length = n->size - offset;
  if (n->boot >= 0)
    return fs_read_at((u32)n->boot, offset, dest, length) ? (int)length : S_IO;
  for (u32 i = 0; i < length; i++)
    dest[i] = n->pages[(offset + i) / 4096][(offset + i) % 4096];
  return (int)length;
}
int vfs_buffer_write(u8** pages, u32* size, u32 offset, const u8* data, u32 length) {
  if (offset > MAX_FILE || length > MAX_FILE - offset) return S_INVALID;
  if (!length) return 0;
  u32 total = offset + length;
  if (total < *size) total = *size;
  u32 old = (*size + 4095) / 4096, need = (total + 4095) / 4096, have = old;
  while (have < need) {
    pages[have] = page_alloc();
    if (!pages[have]) {
      while (have > old) { have--; page_free(pages[have]); pages[have] = 0; }
      return S_NOMEM;
    }
    have++;
  }
  /* Zero gaps even when they share the old last page. */
  for (u32 i = *size; i < offset; i++) pages[i / 4096][i % 4096] = 0;
  for (u32 i = 0; i < length; i++)
    pages[(offset + i) / 4096][(offset + i) % 4096] = data[i];
  *size = total;
  return (int)length;
}
int vfs_write_at(int index, u32 offset, const u8* data, u32 length) {
  if (index < 0 || index >= VFS_NODES || !nodes[index].used) return S_NOTFOUND;
  Node* n = &nodes[index];
  if (n->dir) return S_INVALID;
  if (n->readonly) return S_DENIED;
  if (!revision_room(2)) return S_LIMIT;
  int result = vfs_buffer_write(n->pages, &n->size, offset, data, length);
  if (result > 0) changed(index);
  return result;
}
/* All fallible checks precede the page ownership transfer. The kernel is not
   preemptible, so callers observe either the old or the complete new contents. */
int vfs_publish(int parent, const char* name, u32 generation, u32 revision,
                u8** pages, u32 size, int* published) {
  if (parent < 0 || parent >= VFS_NODES || !nodes[parent].used ||
      !nodes[parent].dir || size > MAX_FILE) return S_INVALID;
  if (nodes[parent].readonly) return S_DENIED;
  int index = vfs_child(parent, name);
  if (index < 0) {
    if (generation || revision) return S_CHANGED;
    char path[VFS_PATH]; vfs_path(parent, path);
    if (strlen(path) + (parent != 0) + strlen(name) >= VFS_PATH) return S_LIMIT;
    if (next_generation == ~0u || !revision_room(4)) return S_LIMIT;
    index = allocate(parent, name, 0);
    if (index < 0) return S_LIMIT;
  } else {
    if (!generation || vfs_generation[index] != generation ||
        vfs_revision[index] != revision) return S_CHANGED;
    if (nodes[index].dir) return S_INVALID;
    if (nodes[index].readonly || nodes[index].pinned) return S_DENIED;
    if (vfs_refs[index]) return S_BUSY;
    if (!revision_room(2)) return S_LIMIT;
  }
  Node* n = &nodes[index];
  for (u32 i = 0; i < VFS_PAGES; i++) {
    u8* old = n->pages[i];
    n->pages[i] = pages[i]; pages[i] = 0;
    if (old) page_free(old);
  }
  n->size = size;
  changed(index);
  *published = index;
  return 0;
}
void vfs_init(void) {
  memset(nodes, 0, sizeof(nodes));
  memset(vfs_generation, 0, sizeof(vfs_generation));
  memset(vfs_revision, 0, sizeof(vfs_revision));
  memset(vfs_refs, 0, sizeof(vfs_refs));
  next_generation = next_revision = 1;
  vfs_generation[0] = vfs_revision[0] = 1;
  nodes[0].used = nodes[0].dir = nodes[0].pinned = 1;
  nodes[0].boot = -1;
  cwd = 0;
  int home = vfs_create("/home", 1, 0);
  int user = vfs_create("/home/user", 1, 0);
  int tmp = vfs_create("/tmp", 1, 0);
  int boot = vfs_create("/boot", 1, 0);
  int etc = vfs_create("/etc", 1, 0);
  if (home < 0 || user < 0 || tmp < 0 || boot < 0 || etc < 0)
    panic("VFS setup failed", 0, 0);
  nodes[home].pinned = nodes[user].pinned = nodes[tmp].pinned =
      nodes[etc].pinned = 1;
  nodes[boot].readonly = nodes[boot].pinned = 1;
  for (u32 i = 0; i < file_count; i++) {
    char name[VFS_NAME];
    copy(name, files[i].name);
    for (char* p = name; *p; p++)
      if (*p >= 'A' && *p <= 'Z')
        *p += 32;
    int n = allocate(boot, name, 0);
    if (n < 0)
      panic("Boot mount failed", i, 0);
    nodes[n].readonly = 1;
    nodes[n].boot = (int)i;
    nodes[n].size = files[i].size;
  }
  if (vfs_create("/home/user/Documents", 1, 0) < 0)
    panic("Home documents", 0, 0);
  int desktop = vfs_create("/home/user/Desktop", 1, 0),
      trash = vfs_create("/home/user/Trash", 1, 0),
      apps = vfs_create("/Applications", 1, 0);
  if (desktop < 0 || trash < 0 || apps < 0)
    panic("Desktop directories", 0, 0);
  nodes[desktop].pinned = nodes[trash].pinned = 1;
  /* Paint has been hidden 261103. Do not register its built-in application
   * entry. */
  const char* app_names[] = {"TextEdit", "Calculator", "Clock",  "Terminal",
                             "Files",    "System",     "Install"};
  for (u32 i = 0; i < sizeof(app_names) / sizeof(app_names[0]); i++) {
    char path[VFS_PATH];
    copy(path, "/Applications/");
    copy(path + 14, app_names[i]);
    copy(path + strlen(path), ".app");
    int app = vfs_create(path, 0, 0);
    if (app < 0 ||
        vfs_write(app, (const u8*)app_names[i], strlen(app_names[i]), 0) < 0)
      panic("Application registry", i, 0);
    nodes[app].readonly = nodes[app].pinned = 1;
  }
  nodes[apps].readonly = nodes[apps].pinned = 1;
  const char* welcome =
      "Welcome to SINO Platinum Desktop.\n\nUse the SINO menu to launch "
      "applications.\nDouble-click desktop icons or files to open them.\nFile "
      "> New Folder / New Text Document creates real files.\n\nHome files are "
      "stored on the SINO data disk. /tmp is temporary.\n";
  int n = vfs_create("/home/user/welcome.txt", 0, 0);
  if (n < 0 || vfs_write(n, (const u8*)welcome, strlen(welcome), 0) < 0)
    panic("VFS welcome", 0, 0);
  const char* desktop_readme =
      "欢迎使用 SINO "
      "中文桌面\n\n右键桌面空白处可新建文件夹和文本文件。\n右键文件可打开、重命"
      "名、复制、剪切或移到废纸篓。\n按住文件拖到文件夹、文件窗口或桌面，然后松"
      "开移动。\n拖到废纸篓可收纳文件；文件窗口内双击文件夹可进入。\n\nCtrl+"
      "空格切换英文和拼音，也可点击右上角 EN / "
      "中。\n输入拼音后按空格选第一个候选，1-7 选字，[ ] "
      "翻页。\n候选字也可以用鼠标点击，回车可输入拼音原文。\n支持 "
      "nihao、zhongwen、wenjian、xinjian 等常用词。\n\nCtrl+S 保存，Ctrl+C / X "
      "/ V "
      "复制、剪切和粘贴。\n个人磁盘的文件保存在数据盘，重启后仍然保留。\n\n拖动"
      "标题栏移动窗口，右下角调整文件和文本窗口大小。\nAlt+Tab "
      "切换窗口，在“窗口”菜单恢复最小化窗口。\nF1 文件管理器 / F2 终端 / F3 "
      "文本编辑 / F4 计算器\nF6 时钟 / F7 关于 SINO\n";
  n = vfs_create("/home/user/Desktop/Read Me.txt", 0, 0);
  if (n < 0 ||
      vfs_write(n, (const u8*)desktop_readme, strlen(desktop_readme), 0) < 0)
    panic("Desktop readme", 0, 0);
  const char* release =
      "NAME=SINO\nVERSION=0.10\nKERNEL=SINO32\nSHELL=SINO SH\n";
  n = vfs_create("/etc/os-release", 0, 0);
  if (n < 0 || vfs_write(n, (const u8*)release, strlen(release), 0) < 0)
    panic("VFS release", 0, 0);
  cwd = user;
  pr_debug("VFS ready: read-only /boot, writable RAM home and tmp\n");
}
