/* G5b strict workspace commands. All writes use explicit directory objects. */
#include "stream.h"
static CliInfo cli;
static int root;
static char target[128];
static u8 buffer[512];
static int put(const char *s) {
  u32 n;
  return StreamWriteAllFor(cli.out, s, strlen(s), 300, &n);
}
static int error(int status) {
  char n[12];
  put("write error ");
  if (status < 0) {
    put("-");
    number(0u - (u32)status, n);
  } else
    number(status, n);
  put(n);
  put("\n");
  return 1;
}
static const char *arg(u32 i) {
  return cli.bytes + cli.offsets[i];
}
static int stat_path(const char *path, FileStat *out) {
  int h = OpenAt(root, path, FS_READ, 0);
  if (h < 0)
    return h;
  int s = FileGetStat(h, out);
  Close(h);
  return s;
}
static int same(const FileStat *a, const FileStat *b) {
  return a->generation == b->generation && a->revision == b->revision &&
         a->length == b->length;
}
static int copy_range(int input, int stage, u32 bytes,
                      const FileStat *version) {
  for (u32 offset = 0; offset < bytes;) {
    u32 n = bytes - offset;
    if (n > sizeof(buffer))
      n = sizeof(buffer);
    FileStat now;
    int s = FileGetStat(input, &now);
    if (s < 0)
      return s;
    if (!same(version, &now))
      return S_CHANGED;
    s = FileReadAt(input, offset, buffer, n);
    if (s < 0)
      return s;
    if ((u32)s != n)
      return S_CHANGED;
    s = FileGetStat(input, &now);
    if (s < 0)
      return s;
    if (!same(version, &now))
      return S_CHANGED;
    s = FileWriteAt(stage, offset, buffer, n);
    if (s < 0)
      return s;
    if ((u32)s != n)
      return S_IO;
    offset += n;
    Yield(1); /* Bound each copy turn and keep cancellation responsive. */
  }
  return 0;
}
static int destination(const char *source, const char *dest) {
  int dir = OpenAt(root, dest, FS_LOOKUP, OPEN_DIRECTORY);
  if (strlen(dest) >= sizeof(target))
    return S_LIMIT;
  copy(target, dest);
  if (dir < 0)
    return 0;
  Close(dir);
  const char *name = source;
  for (const char *p = source; *p; p++)
    if (*p == '/')
      name = p + 1;
  u32 at = strlen(target), n = strlen(name);
  if (at + 1 + n >= sizeof(target))
    return S_LIMIT;
  if (at && target[at - 1] != '/')
    target[at++] = '/';
  copy(target + at, name);
  return 0;
}
static int replace(const char *path, int append, int copying) {
  FileStat old = {0};
  int s = stat_path(path, &old);
  if (s < 0 && s != S_NOTFOUND)
    return error(s);
  int stage = FileStage(root, path, s == S_NOTFOUND ? 0 : old.revision);
  if (stage < 0)
    return error(stage);
  u32 used = 0;
  int source = 0;
  FileStat version = {0};
  if (copying || (append && s != S_NOTFOUND)) {
    source = OpenAt(root, copying ? arg(1) : path, FS_READ, 0);
    if (source < 0) {
      Close(stage);
      return error(source);
    }
    s = FileGetStat(source, &version);
    if (!s)
      s = copy_range(source, stage, version.length, &version);
    if (!s) {
      FileStat after;
      s = FileGetStat(source, &after);
      if (!s && !same(&version, &after))
        s = S_CHANGED;
    }
    if (append)
      used = version.length;
    Close(source);
    if (s < 0) {
      Close(stage);
      return error(s);
    }
  }
  if (!copying) {
    for (u32 i = 2; i < cli.argc; i++) {
      if (i > 2) {
        s = FileWriteAt(stage, used, " ", 1);
        if (s != 1) {
          Close(stage);
          return error(s < 0 ? s : S_IO);
        }
        used++;
      }
      u32 n = strlen(arg(i));
      s = FileWriteAt(stage, used, arg(i), n);
      if (s < 0 || (u32)s != n) {
        Close(stage);
        return error(s < 0 ? s : S_IO);
      }
      used += n;
    }
    s = FileWriteAt(stage, used, "\n", 1);
    if (s != 1) {
      Close(stage);
      return error(s < 0 ? s : S_IO);
    }
  }
  CommitInfo report;
  s = FilePublish(stage, &report);
  Close(stage);
  if (s < 0)
    return error(s);
  return put("published in RAM; sync confirms disk\n") < 0;
}
int main(void) {
  StartInfo start;
  root = StartDirectory();
  if (QueryStartInfo(&start) || start.policy != PROCESS_STRICT || start.caps ||
      QueryCliInfo(&cli) || TerminalCurrent() < 0 || root < 0 || !cli.argc)
    return 2;
  const char *cmd = arg(0);
  if (streq(cmd, "sync")) {
    if (cli.argc != 1)
      return 2;
    CommitInfo r;
    int s = DirectoryCommit(root, &r);
    if (s < 0)
      return error(s);
    return put("home snapshot durable\n") < 0;
  }
  if (streq(cmd, "touch") || streq(cmd, "mkdir") || streq(cmd, "rm") ||
      streq(cmd, "rmdir")) {
    if (cli.argc != 2)
      return 2;
    int s;
    if (streq(cmd, "rm") || streq(cmd, "rmdir"))
      s = RemoveAt(root, arg(1), streq(cmd, "rmdir"));
    else {
      int dir = streq(cmd, "mkdir");
      s = OpenAt(root, arg(1), dir ? FS_LOOKUP : FS_WRITE,
                 OPEN_CREATE | (dir ? OPEN_DIRECTORY | OPEN_EXCLUSIVE : 0));
      if (s >= 0) {
        Close(s);
        s = 0;
      }
    }
    return s < 0 ? error(s) : 0;
  }
  if (streq(cmd, "cp") || streq(cmd, "mv")) {
    if (cli.argc != 3)
      return 2;
    int s = destination(arg(1), arg(2));
    if (s < 0)
      return error(s);
    if (streq(arg(1), target))
      return error(S_INVALID);
    if (streq(cmd, "mv")) {
      s = RenameAt(root, arg(1), root, target);
      return s < 0 ? error(s) : 0;
    }
    return replace(target, 0, 1);
  }
  if (streq(cmd, "write") || streq(cmd, "append")) {
    if (cli.argc < 3)
      return 2;
    return replace(arg(1), streq(cmd, "append"), 0);
  }
  return 2;
}
