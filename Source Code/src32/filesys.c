/* Stage D: explicit directory authority and private, version-checked saves.
   All operations run synchronously in the non-preemptible kernel. */
#include "filesys.h"
#include "mm/uaccess.h"
#include "vfs.h"
#include "storage.h"
#include "gui.h"
#include "lib/kstring.h"
#include "mm/page.h"

typedef struct {
  int node, parent;
  u32 generation, parent_generation, expected_generation, expected_revision;
  u32 size, published, revision, durable, commit_generation;
  char name[VFS_NAME];
  u8* pages[VFS_PAGES];
} FileObject;
_Static_assert(sizeof(FileObject) <= 4096, "file object fits one page");

static FileObject* value(Object* o) { return (FileObject*)o->value; }
static int live(int n, u32 generation) {
  return n >= 0 && n < VFS_NODES && nodes[n].used && vfs_generation[n] == generation;
}
int filesys_is_boot_directory(Object* object) {
  FileObject* dir = value(object);
  return live(dir->node, dir->generation) && dir->node == vfs_lookup("/boot");
}
static FileObject* get(Handle* owner, u32 h, u32 type, u32 rights) {
  Object* o = handle_get(owner, h, type, rights);
  if (!o) return 0;
  FileObject* f = value(o);
  if (f->node >= 0 && !live(f->node, f->generation)) return 0;
  if (type == OBJ_STAGING && !live(f->parent, f->parent_generation)) return 0;
  return f;
}
void filesys_release(int pointer) {
  FileObject* f = (FileObject*)pointer;
  if (f->node >= 0 && live(f->node, f->generation)) vfs_refs[f->node]--;
  if (f->parent >= 0 && live(f->parent, f->parent_generation)) vfs_refs[f->parent]--;
  for (u32 i = 0; i < VFS_PAGES; i++) if (f->pages[i]) page_free(f->pages[i]);
  page_free(f);
}
/* Reserve resources before a create/truncate can become visible. */
static int reserve(Handle* owner, u32 type, u32 rights, FileObject** out) {
  FileObject* f = page_alloc();
  if (!f) return S_NOMEM;
  f->node = f->parent = -1;
  int object = object_new(type, (int)f);
  if (object < 0) { page_free(f); return object; }
  int h = handle_open(owner, object, rights);
  if (h < 0) { object_unref(object); return h; }
  *out = f;
  return h;
}
static void bind(FileObject* f, int n) {
  f->node = n; f->generation = vfs_generation[n]; vfs_refs[n]++;
}
int filesys_acquire_directory(Handle* owner, u32 h, u32 rights) {
  if (!rights || (rights & ~FS_RIGHTS)) return S_INVALID;
  if (!get(owner, h, OBJ_DIRECTORY, rights | FS_DELEGATE)) return S_BADHANDLE;
  Object* o = handle_get(owner, h, OBJ_DIRECTORY, rights | FS_DELEGATE);
  o->refs++;
  return (int)(o - objects);
}

/* Resolve against this handle only. No global cwd, drive aliases or symlinks.
   A '..' above the anchor is denied even if a later component reenters it. */
static int resolve(int root, const char* path, int missing, int* parent, char* name) {
  if (!path[0] || path[0] == '/') return S_INVALID;
  int n = root;
  u32 pos = 0;
  name[0] = 0; *parent = root;
  while (path[pos]) {
    while (path[pos] == '/') pos++;
    if (!path[pos]) break;
    if (!nodes[n].dir) return S_INVALID;
    char part[VFS_NAME]; u32 count = 0;
    while (path[pos] && path[pos] != '/') {
      u8 ch = (u8)path[pos++];
      if (ch < 32 || ch == 127 || ch == '\\' || ch == ':') return S_INVALID;
      if (count >= VFS_NAME - 1) return S_LIMIT;
      part[count++] = (char)ch;
    }
    part[count] = 0;
    if (streq(part, ".")) continue;
    if (streq(part, "..")) {
      if (n == root) return S_DENIED;
      n = nodes[n].parent; continue;
    }
    *parent = n; memcpy(name, part, count + 1);
    int child = vfs_child(n, part);
    if (child < 0) {
      /* A trailing slash still requires a real directory. */
      return missing && !path[pos] ? S_NOTFOUND : S_INVALID;
    }
    n = child;
  }
  if (path[pos - 1] == '/' && !nodes[n].dir) return S_INVALID;
  return n;
}
static int fullpath(int parent, const char* name, char* out) {
  vfs_path(parent, out);
  u32 size = strlen(out), length = strlen(name);
  if (size + (parent != 0) + length >= VFS_PATH) return S_LIMIT;
  if (parent) out[size++] = '/';
  memcpy(out + size, name, length + 1);
  return 0;
}
static int grant(Handle* owner, u32 caps, const char* path, u32 rights) {
  if (path[0] != '/' || !rights || (rights & ~FS_RIGHTS)) return S_INVALID;
  int n = vfs_lookup(path);
  if (n < 0) return S_NOTFOUND;
  if (!nodes[n].dir) return S_INVALID;
  if (rights & (FS_WRITE | FS_CREATE | FS_REMOVE | FS_COMMIT)) {
    int home = vfs_lookup("/home/user");
    if (!(caps & CAP_HOME) || (n != home && !storage_tracks_node(n)) ||
        nodes[n].readonly) return S_DENIED;
  }
  FileObject* f;
  int h = reserve(owner, OBJ_DIRECTORY, rights, &f);
  if (h >= 0) bind(f, n);
  return h;
}
int filesys_boot_reader(Handle* owner) {
  return grant(owner, 0, "/boot", FS_READ | FS_LOOKUP);
}
static int open_at(Handle* owner, u32 h, const char* path, const OpenRequest* r) {
  if (!r->rights || (r->rights & ~FS_RIGHTS) || (r->flags & ~15u) ||
      ((r->flags & OPEN_EXCLUSIVE) && !(r->flags & OPEN_CREATE))) return S_INVALID;
  int dir = (r->flags & OPEN_DIRECTORY) != 0;
  if ((!dir && (r->rights & ~(FS_READ | FS_WRITE))) ||
      ((r->flags & OPEN_TRUNCATE) && (dir || !(r->rights & FS_WRITE)))) return S_INVALID;
  u32 need = FS_LOOKUP | r->rights;
  if (r->flags & OPEN_CREATE) need |= FS_CREATE;
  FileObject* root = get(owner, h, OBJ_DIRECTORY, need);
  if (!root) return S_BADHANDLE;
  int parent; char name[VFS_NAME], absolute[VFS_PATH];
  int n = resolve(root->node, path, 1, &parent, name);
  if (n < 0 && n != S_NOTFOUND) return n;
  if (n == S_NOTFOUND && !(r->flags & OPEN_CREATE)) return S_NOTFOUND;
  if (n >= 0 && (r->flags & OPEN_EXCLUSIVE)) return S_EXISTS;
  if (n >= 0 && nodes[n].dir != dir) return S_INVALID;
  if ((n >= 0 ? nodes[n].readonly : nodes[parent].readonly) &&
      ((r->rights & (FS_WRITE | FS_CREATE | FS_REMOVE | FS_COMMIT)) ||
       (n < 0) || (r->flags & OPEN_TRUNCATE))) return S_DENIED;
  int status = fullpath(parent, name, absolute);
  if (n < 0 && status) return status;
  FileObject* f;
  int result = reserve(owner, dir ? OBJ_DIRECTORY : OBJ_FILE, r->rights, &f);
  if (result < 0) return result;
  int created = n < 0;
  if (n < 0) {
    n = vfs_create(absolute, dir, 0);
    if (n < 0) { handle_close(owner, (u32)result); return S_LIMIT; }
  }
  if (!created && (r->flags & OPEN_TRUNCATE) && vfs_write(n, 0, 0, 0) < 0) {
    handle_close(owner, (u32)result); return S_LIMIT;
  }
  bind(f, n);
  if (r->flags & (OPEN_CREATE | OPEN_TRUNCATE)) gui_files_changed();
  return result;
}
static int stat(Handle* owner, u32 h, FileStat* out) {
  Object* o = handle_get(owner, h, OBJ_FILE, 0);
  if (!o) o = handle_get(owner, h, OBJ_DIRECTORY, 0);
  if (!o) o = handle_get(owner, h, OBJ_STAGING, 0);
  if (!o || !get(owner, h, o->type, 0)) return S_BADHANDLE;
  FileObject* f = value(o);
  if (o->type == OBJ_STAGING && !f->published) {
    *out = (FileStat){sizeof(*out), 1, FILE_STAGING, f->size, 0, 0, 0, 0};
  } else {
    Node* n = &nodes[f->node];
    *out = (FileStat){sizeof(*out), 1, n->dir ? FILE_DIRECTORY : FILE_REGULAR,
                     n->size, f->generation, vfs_revision[f->node], (u32)n->readonly, 0};
  }
  return 0;
}
static int dir_read(Handle* owner, u32 h, DirPage* page) {
  FileObject* f = get(owner, h, OBJ_DIRECTORY, FS_ENUM);
  if (!f) return S_BADHANDLE;
  if (!page->capacity || page->capacity > VFS_NODES || page->cursor > VFS_NODES ||
      (!page->version && page->cursor)) return S_INVALID;
  if (page->version && page->version != vfs_revision[f->node]) return S_CHANGED;
  page->version = vfs_revision[f->node]; page->count = 0;
  u32 i = page->cursor;
  for (; i < VFS_NODES; i++) {
    if (!nodes[i].used || i == 0 || nodes[i].parent != f->node) continue;
    if (page->count == page->capacity) break;
    DirEntry entry; memset(&entry, 0, sizeof(entry));
    memcpy(entry.name, nodes[i].name, strlen(nodes[i].name) + 1);
    entry.length = nodes[i].size; entry.dir = (u32)nodes[i].dir;
    entry.revision = vfs_revision[i];
    memcpy((DirEntry*)page->buffer + page->count++, &entry, sizeof(entry));
  }
  page->cursor = i; page->done = i == VFS_NODES;
  return 0;
}
static int remove_at(Handle* owner, u32 h, const char* path, u32 directory) {
  if (directory > 1) return S_INVALID;
  FileObject* root = get(owner, h, OBJ_DIRECTORY, FS_LOOKUP | FS_REMOVE);
  if (!root) return S_BADHANDLE;
  int parent; char name[VFS_NAME], absolute[VFS_PATH];
  int n = resolve(root->node, path, 1, &parent, name);
  if (n < 0) return n;
  if (vfs_busy(n)) return S_BUSY;
  if (nodes[n].readonly || nodes[n].pinned) return S_DENIED;
  if (nodes[n].dir != (int)directory) return S_INVALID;
  vfs_path(n, absolute);
  if (vfs_remove(absolute, (int)directory) < 0) return S_BUSY;
  gui_files_changed(); return 0;
}
static int rename_at(Handle* owner, u32 sh, const char* source, u32 dh, const char* dest) {
  FileObject* s = get(owner, sh, OBJ_DIRECTORY, FS_LOOKUP | FS_REMOVE);
  FileObject* d = get(owner, dh, OBJ_DIRECTORY, FS_LOOKUP | FS_CREATE);
  if (!s || !d) return S_BADHANDLE;
  int sp, dp; char sn[VFS_NAME], dn[VFS_NAME], from[VFS_PATH], to[VFS_PATH];
  int n = resolve(s->node, source, 1, &sp, sn);
  if (n < 0) return n;
  if (vfs_busy(n)) return S_BUSY;
  if (nodes[n].readonly || nodes[n].pinned) return S_DENIED;
  int target = resolve(d->node, dest, 1, &dp, dn);
  if (target >= 0) return S_EXISTS; /* Exact destination, unlike legacy mv. */
  if (target != S_NOTFOUND) return target;
  if (nodes[dp].readonly) return S_DENIED;
  int status = fullpath(dp, dn, to);
  if (status) return status;
  vfs_path(n, from);
  if (vfs_move(from, to) < 0) return S_INVALID;
  gui_files_changed(); return 0;
}
static int stage(Handle* owner, u32 h, const char* path, u32 revision) {
  u32 need = FS_LOOKUP | FS_CREATE | FS_WRITE;
  FileObject* root = get(owner, h, OBJ_DIRECTORY, need);
  if (!root) return S_BADHANDLE;
  int parent; char name[VFS_NAME], absolute[VFS_PATH];
  int n = resolve(root->node, path, 1, &parent, name);
  if (n < 0 && n != S_NOTFOUND) return n;
  if ((n < 0 && revision) || (n >= 0 && vfs_revision[n] != revision)) return S_CHANGED;
  if (n >= 0 && nodes[n].dir) return S_INVALID;
  if (n >= 0 && (nodes[n].readonly || nodes[n].pinned)) return S_DENIED;
  if (nodes[parent].readonly) return S_DENIED;
  int status = fullpath(parent, name, absolute);
  if (status || !name[0]) return status ? status : S_INVALID;
  u32 rights = FS_READ | FS_WRITE;
  if (handle_get(owner, h, OBJ_DIRECTORY, FS_COMMIT)) rights |= FS_COMMIT;
  FileObject* f;
  int result = reserve(owner, OBJ_STAGING, rights, &f);
  if (result < 0) return result;
  f->parent = parent; f->parent_generation = vfs_generation[parent]; vfs_refs[parent]++;
  f->expected_generation = n >= 0 ? vfs_generation[n] : 0;
  f->expected_revision = revision;
  memcpy(f->name, name, strlen(name) + 1);
  return result;
}
static int finish(Handle* owner, u32 h, int commit, CommitInfo* out) {
  FileObject* f = get(owner, h, OBJ_STAGING, commit ? FS_COMMIT : FS_WRITE);
  if (!f) return S_BADHANDLE;
  int status = 0;
  if (!commit && !f->published) {
    int n;
    status = vfs_publish(f->parent, f->name, f->expected_generation,
                          f->expected_revision, f->pages, f->size, &n);
    if (!status) {
      bind(f, n); f->published = 1; f->revision = vfs_revision[n];
      gui_files_changed();
    }
  }
  if (!status && !f->published) status = S_AGAIN;
  if (!status && vfs_revision[f->node] != f->revision) status = S_CHANGED;
  if (!status && commit && !f->durable) {
    if (!storage_tracks_node(f->node)) status = S_DENIED;
    else if (!storage_sync()) status = S_IO;
    else { f->durable = 1; f->commit_generation = storage_generation; }
  }
  *out = (CommitInfo){sizeof(*out), 1, f->published, f->revision,
                       f->durable, f->commit_generation, status, 0};
  return status;
}

int filesys_workspace(Handle* owner) {
  return grant(owner,CAP_HOME,"/home/user",FS_RIGHTS);
}
int filesys_call(Handle* owner, u32 caps, u32 call, u32 a, u32 b, u32 c,
                 u32* user_pd) {
  char path[VFS_PATH];
  switch (call) {
    case SYS_DIRECTORY_COMMIT: {
      if (c < sizeof(CommitInfo)) return S_INVALID;
      UACCESS_TRY(uaccess_prepare(user_pd, b, sizeof(CommitInfo), USER_WRITE));
      FileObject* f=get(owner,a,OBJ_DIRECTORY,FS_COMMIT);
      if(!f)return S_BADHANDLE;
      int home=vfs_lookup("/home/user");
      if(f->node!=home && !storage_tracks_node(f->node))return S_DENIED;
      int status=storage_sync()?0:S_IO;
      CommitInfo report={sizeof(report),1,0,0,status==0,status==0?storage_generation:0,status,0};
      memcpy((void*)b,&report,sizeof(report));return status;
    }
    case SYS_DIR_GRANT:
      if (c) return S_INVALID;
      UACCESS_TRY(uaccess_copy_string(user_pd, path, a, sizeof(path)));
      return grant(owner, caps, path, b);
    case SYS_OPEN_AT: {
      if (c != sizeof(OpenRequest)) return S_INVALID;
      OpenRequest request;
      UACCESS_TRY(uaccess_copy_in(user_pd, &request, b, sizeof(request)));
      UACCESS_TRY(uaccess_copy_string(user_pd, path, request.path, sizeof(path)));
      return open_at(owner, a, path, &request);
    }
    case SYS_FILE_READ_AT:
    case SYS_FILE_WRITE_AT: {
      if (c != sizeof(FileIo)) return S_INVALID;
      FileIo io;
      UACCESS_TRY(uaccess_copy_in(user_pd, &io, b, sizeof(io)));
      int read = call == SYS_FILE_READ_AT;
      if (io.offset > MAX_FILE || io.bytes > MAX_FILE - io.offset) return S_INVALID;
      if (io.bytes) UACCESS_TRY(uaccess_prepare(user_pd, io.buffer, io.bytes, (read) ? USER_WRITE : USER_READ));
      FileObject* f = get(owner, a, OBJ_FILE, read ? FS_READ : FS_WRITE);
      if (f) {
        int result = read ? vfs_read_at(f->node, io.offset, (u8*)io.buffer, io.bytes)
                          : vfs_write_at(f->node, io.offset, (const u8*)io.buffer, io.bytes);
        if (!read && result > 0) gui_files_changed();
        return result;
      }
      f = get(owner, a, OBJ_STAGING, read ? FS_READ : FS_WRITE);
      if (!f) return S_BADHANDLE;
      if (f->published) return S_BUSY;
      if (!read) return vfs_buffer_write(f->pages, &f->size, io.offset, (const u8*)io.buffer, io.bytes);
      u32 length = io.offset >= f->size ? 0 : f->size - io.offset;
      if (length > io.bytes) length = io.bytes;
      for (u32 i = 0; i < length; i++)
        ((u8*)io.buffer)[i] = f->pages[(io.offset + i) / 4096][(io.offset + i) % 4096];
      return (int)length;
    }
    case SYS_FILE_STAT: {
      if (c < sizeof(FileStat)) return S_INVALID;
      UACCESS_TRY(uaccess_prepare(user_pd, b, sizeof(FileStat), USER_WRITE));
      FileStat result; int status = stat(owner, a, &result);
      if (!status) memcpy((void*)b, &result, sizeof(result));
      return status;
    }
    case SYS_DIR_READ: {
      if (c != sizeof(DirPage)) return S_INVALID;
      UACCESS_TRY(uaccess_prepare(user_pd, b, sizeof(DirPage), USER_WRITE));
      DirPage page; memcpy(&page, (void*)b, sizeof(page));
      if (!page.capacity || page.capacity > VFS_NODES) return S_INVALID;
      UACCESS_TRY(uaccess_prepare(user_pd, page.buffer, page.capacity * sizeof(DirEntry), USER_WRITE));
      /* Keep response header separate from the entry buffer. */
      if (page.buffer < b + sizeof(DirPage) && b < page.buffer + page.capacity * sizeof(DirEntry))
        return S_INVALID;
      int status = dir_read(owner, a, &page);
      if (!status) memcpy((void*)b, &page, sizeof(page));
      return status;
    }
    case SYS_REMOVE_AT:
      UACCESS_TRY(uaccess_copy_string(user_pd, path, b, sizeof(path)));
      return remove_at(owner, a, path, c);
    case SYS_RENAME_AT: {
      if (b != sizeof(RenameSpec) || c) return S_INVALID;
      RenameSpec spec;
      UACCESS_TRY(uaccess_copy_in(user_pd, &spec, a, sizeof(spec)));
      char dest[VFS_PATH];
      UACCESS_TRY(uaccess_copy_string(user_pd, path, spec.source_path, sizeof(path)));
      UACCESS_TRY(uaccess_copy_string(user_pd, dest, spec.target_path, sizeof(dest)));
      return rename_at(owner, spec.source_dir, path, spec.target_dir, dest);
    }
    case SYS_FILE_STAGE: {
      if (c != sizeof(StageSpec)) return S_INVALID;
      StageSpec spec;
      UACCESS_TRY(uaccess_copy_in(user_pd, &spec, b, sizeof(spec)));
      UACCESS_TRY(uaccess_copy_string(user_pd, path, spec.path, sizeof(path)));
      return stage(owner, a, path, spec.expected_revision);
    }
    case SYS_FILE_PUBLISH:
    case SYS_FILE_COMMIT: {
      if (c < sizeof(CommitInfo)) return S_INVALID;
      UACCESS_TRY(uaccess_prepare(user_pd, b, sizeof(CommitInfo), USER_WRITE));
      CommitInfo result = {0};
      int status = finish(owner, a, call == SYS_FILE_COMMIT, &result);
      if (status != S_BADHANDLE) memcpy((void*)b, &result, sizeof(result));
      return status;
    }
    default: return S_INVALID;
  }
}
