/* Stage D regression client for the text gate and cold-boot byte checks. */
#include "sdk.h"
_Static_assert(SYS_DIR_GRANT == 0x13b && SYS_FILE_COMMIT == 0x146, "Stage D wire IDs");
_Static_assert(sizeof(RunSpec) == 48 && sizeof(RunSpecV2) == 64, "launch versions");
_Static_assert(sizeof(FileStat) == 32 && sizeof(CommitInfo) == 32, "file query ABI");
_Static_assert(sizeof(DirPage) == 24 && sizeof(DirEntry) == 44, "directory ABI");
static u8 bytes[8192], output[8192];
static int held[24];
static u32 checks;
static void require(int ok, const char* why) {
  checks++;
  if (!ok) { Log("FILESYS SELFTEST FAIL: "); Log(why); Log("\n"); Exit(1); }
}
static int same(const u8* a, const u8* b, u32 size) {
  for (u32 i = 0; i < size; i++) if (a[i] != b[i]) return 0;
  return 1;
}
static int entries(int dir) {
  DirEntry buffer[2]; DirPage page = {0, 0, (u32)buffer, 2, 0, 0}; int count = 0;
  do { require(DirectoryRead(dir, &page) == 0, "paged enumeration"); count += (int)page.count; }
  while (!page.done);
  return count;
}
static int child_run(int dir, u32 rights, const char* mode, const char* path) {
  RunSpecV2 spec; memset(&spec, 0, sizeof(spec));
  u32 offsets[1] = {0};
  spec.run = (RunSpec){sizeof(spec), 2, 0, (u32)path, 1, strlen(mode) + 1,
                       (u32)mode, (u32)offsets, 0, 0, 500, 0};
  spec.directory = (u32)dir; spec.directory_rights = rights;
  return StartRunWithDirectory(&spec);
}
static void result(int h, u32 reason) {
  require(h > 0, "child created with explicit grant");
  RunResult r; int status; u32 start = (u32)api(SYS_TICKS, 0, 0, 0);
  while ((status = GetRunResult(h, &r)) == S_AGAIN &&
         (u32)api(SYS_TICKS, 0, 0, 0) - start < 600) Yield(1);
  require(status == 0 && r.completion.reason == reason && !r.completion.code,
          "child completed with expected typed result");
  require(Close(h) == 0, "close child observation");
}
static int child(void) {
  CliInfo info; char* argv[2];
  require(CliArguments(&info, argv, 2) == 1, "structured child arguments");
  StartInfo start;
  require(QueryStartInfo(&start) == 0 && start.policy == PROCESS_STRICT && !start.caps,
          "grant does not restore ambient caps");
  int dir = StartDirectory();
  require(dir > 0, "child receives directory context");
  require(GrantDirectory("/home/user", FS_READ | FS_LOOKUP) == S_DENIED &&
          Read("/home/user/stage2-persist.bin", output, sizeof(output)) == S_DENIED &&
          Sync() == S_DENIED && Spawn("/boot/fstest.snx") == S_DENIED,
          "ambient paths, commit and downgrade stay denied");
  require(OpenAt(dir, "../stage2-persist.bin", FS_READ, 0) == S_DENIED,
          "child cannot escape handed-in directory");
  int file = OpenAt(dir, "child", FS_READ, 0);
  require(file > 0 && FileReadAt(file, 0, output, 4) == 4 &&
          same(output, (const u8*)"safe", 4), "strict child reads granted file");
  if (streq(argv[0], "fault")) {
    int stage = FileStage(dir, "unpublished", 0);
    require(stage > 0 && FileWriteAt(stage, 0, "private", 7) == 7, "fault owns private pages");
    __asm__ volatile("ud2");
  }
  require(OpenAt(dir, "child", FS_WRITE, 0) == S_BADHANDLE &&
          RemoveAt(dir, "child", 0) == S_BADHANDLE &&
          FileStage(dir, "forbidden", 0) == S_BADHANDLE, "attenuated grant cannot mutate");
  require(Close(file) == 0 && Close(dir) == 0 && StartDirectory() == S_BADHANDLE,
          "closed startup grant is not recreated");
  return 0;
}
int main(void) {
  if (streq(Arguments(), "reboot")) {
    int home = GrantDirectory("/home/user", FS_READ | FS_LOOKUP);
    int file = OpenAt(home, "stage2-file-save.bin", FS_READ, 0);
    for (u32 i = 0; i < 257; i++) bytes[i] = (u8)(i * 19 + 7);
    require(home > 0 && file > 0 && FileReadAt(file, 0, output, 258) == 257 &&
            same(bytes, output, 257), "cold boot retains the newly published version");
    require(Close(file) == 0 && Close(home) == 0, "cold boot handles released");
    Log("FILESYS REBOOT PASS\n"); return 0;
  }
  if (!streq(Arguments(), "suite")) return child();
  SystemInfo system;
  require(QuerySystemInfo(&system) == 0 && (system.features & FEATURE_FILE_OBJECTS), "file feature");
  require(StartDirectory() == S_NOTFOUND, "no ambient startup directory");
  int home = GrantDirectory("/home/user", FS_RIGHTS);
  require(home > 0 && GrantDirectory("/boot", FS_RIGHTS) == S_DENIED, "write grant restricted to home");
  int dir = OpenAt(home, "stage2-files", FS_RIGHTS, OPEN_CREATE | OPEN_EXCLUSIVE | OPEN_DIRECTORY);
  require(dir > 0, "create exclusive directory");
  require(OpenAt(home, "stage2-files", FS_READ, OPEN_CREATE | OPEN_EXCLUSIVE | OPEN_DIRECTORY) == S_EXISTS,
          "exclusive create refuses existing directory");
  require(OpenAt(dir, "/boot/readme.txt", FS_READ, 0) == S_INVALID &&
          OpenAt(dir, "..", FS_LOOKUP, OPEN_DIRECTORY) == S_DENIED &&
          OpenAt(dir, "a\\b", FS_READ, 0) == S_INVALID &&
          OpenAt(dir, "A:readme.txt", FS_READ, 0) == S_INVALID, "relative path boundaries");
  int file = OpenAt(dir, "range", FS_READ | FS_WRITE, OPEN_CREATE | OPEN_EXCLUSIVE);
  require(file > 0, "file object");
  for (u32 i = 0; i < sizeof(bytes); i++) bytes[i] = (u8)(i * 37 + 11);
  require(FileWriteAt(file, 4090, bytes, 32) == 32 &&
          FileReadAt(file, 4089, output, 34) == 33 && !output[0] &&
          same(output + 1, bytes, 32), "cross-page range and zero-filled gap");
  FileStat before, after;
  require(FileGetStat(file, &before) == 0 && before.length == 4122 && before.revision,
          "stat includes content revision");
  require(FileWriteAt(file, 0, "first", 5) == 5 && FileGetStat(file, &after) == 0 &&
          before.generation == after.generation && before.revision != after.revision,
          "content revision differs from object generation");
  require(FileWriteAt(file, 65535, "X", 1) == 1 && FileReadAt(file, 65535, output, 1) == 1 &&
          output[0] == 'X' && FileReadAt(file, 65536, output, 0) == 0 &&
          FileWriteAt(file, 65535, "XX", 2) == S_INVALID &&
          FileWriteAt(file, 0xffffffffu, "X", 1) == S_INVALID, "64 KiB and overflow boundaries");
  int readonly = api(SYS_DUP, (u32)file, FS_READ, 0);
  require(readonly > 0 && FileWriteAt(readonly, 0, "x", 1) == S_BADHANDLE &&
          RemoveAt(dir, "range", 0) == S_BUSY &&
          RenameAt(dir, "range", dir, "renamed") == S_BUSY &&
          Remove("/home/user/stage2-files/range", 0) < 0 &&
          api(SYS_MOVE, (u32)"/home/user/stage2-files/range", (u32)"/home/user/stage2-files/renamed", 0) < 0,
          "new and old paths respect live references");
  DirEntry buffer[1]; DirPage page = {0, 0, (u32)buffer, 1, 0, 0};
  require(DirectoryRead(dir, &page) == 0 && page.count == 1 && page.done, "enumeration completion");
  int another = OpenAt(dir, "child", FS_READ | FS_WRITE, OPEN_CREATE | OPEN_EXCLUSIVE);
  require(another > 0 && FileWriteAt(another, 0, "safe", 4) == 4 && Close(another) == 0,
          "directory mutation");
  u32 old_cursor = page.cursor;
  require(DirectoryRead(dir, &page) == S_CHANGED && page.cursor == old_cursor,
          "enumeration version rejects mixed snapshots");
  int count = entries(dir), stage = FileStage(dir, "private", 0);
  require(stage > 0 && FileWriteAt(stage, 0, "invisible", 9) == 9 && entries(dir) == count &&
          Sync() == 0 && OpenAt(dir, "private", FS_READ, 0) == S_NOTFOUND,
          "private stage excluded from VFS and global sync");
  require(Close(stage) == 0, "abort stage releases private pages");
  require(FileGetStat(file, &before) == 0, "version for replacement");
  stage = FileStage(dir, "range", before.revision);
  CommitInfo commit;
  require(stage > 0 && FileWriteAt(stage, 0, "replacement", 11) == 11 &&
          FilePublish(stage, &commit) == S_BUSY && !commit.published,
          "publication refuses opened target");
  require(Close(file) == 0 && FilePublish(stage, &commit) == S_BUSY && Close(readonly) == 0,
          "duplicate retains reference");
  require(FilePublish(stage, &commit) == 0 && commit.published && !commit.durable &&
          FileWriteAt(stage, 0, "x", 1) == S_BUSY &&
          FileCommit(stage, &commit) == 0 && commit.durable && commit.generation,
          "publish then confirm global snapshot");
  require(Close(stage) == 0, "release published stage");
  file = OpenAt(dir, "range", FS_READ | FS_WRITE, 0);
  require(file > 0 && FileGetStat(file, &before) == 0 && before.length == 11 &&
          FileReadAt(file, 0, output, 12) == 11 && same(output, (const u8*)"replacement", 11),
          "complete replacement bytes");
  stage = FileStage(dir, "range", before.revision);
  require(stage > 0 && FileWriteAt(stage, 0, "stale", 5) == 5 &&
          FileWriteAt(file, 0, "changed", 7) == 7 && Close(file) == 0 &&
          FilePublish(stage, &commit) == S_CHANGED && !commit.published && Close(stage) == 0,
          "revision conflict preserves current contents");
  for (u32 i = 0; i < 24; i++) held[i] = -1;
  u32 full = 0;
  while (full < 24 && (held[full] = OpenAt(dir, "child", FS_READ, 0)) > 0) full++;
  require(full < 24 && held[full] == S_LIMIT &&
          OpenAt(dir, "no-leak", FS_WRITE, OPEN_CREATE | OPEN_EXCLUSIVE) == S_LIMIT,
          "handle exhaustion precedes create side effect");
  for (u32 i = 0; i < full; i++) require(Close(held[i]) == 0, "release exhausted handles");
  require(OpenAt(dir, "no-leak", FS_READ, 0) == S_NOTFOUND, "failed create absent");
  require(child_run(dir, FS_READ | FS_LOOKUP, "read", "/boot/not-here.snx") == S_NOTFOUND,
          "failed child launch rolls back directory reference");
  int attenuated = api(SYS_DUP, (u32)dir, FS_READ | FS_LOOKUP | FS_ENUM | FS_DELEGATE, 0);
  require(attenuated > 0 && child_run(attenuated, FS_WRITE | FS_LOOKUP, "read", "/boot/fstest.snx") == S_BADHANDLE,
          "launch cannot amplify grant rights");
  result(child_run(attenuated, FS_READ | FS_LOOKUP | FS_ENUM, "read", "/boot/fstest.snx"), PROCESS_EXITED);
  require(Close(attenuated) == 0, "release attenuated grant");
  result(child_run(dir, FS_READ | FS_WRITE | FS_LOOKUP | FS_CREATE, "fault", "/boot/fstest.snx"), PROCESS_FAULTED);
  require(OpenAt(dir, "unpublished", FS_READ, 0) == S_NOTFOUND, "fault does not publish private data");
  require(RemoveAt(dir, "child", 0) == 0 && RemoveAt(dir, "range", 0) == 0 &&
          Close(dir) == 0 && RemoveAt(home, "stage2-files", 1) == 0,
          "child exit and fault release all file and directory pins");
  int boot = GrantDirectory("/boot", FS_READ | FS_LOOKUP);
  file = OpenAt(boot, "README.TXT", FS_READ, 0);
  int length = Read("/boot/readme.txt", bytes, sizeof(bytes));
  require(boot > 0 && file > 0 && length > 48 &&
          FileReadAt(file, 17, output, 31) == 31 && same(output, bytes + 17, 31),
          "FAT range reads retain boot case-folding");
  require(Close(file) == 0 && Close(boot) == 0, "release boot grant");
  /* Save distinct new bytes; host snapshot inspection and a cold guest verify
     them rather than accepting an unchanged preexisting fixture as evidence. */
  require(OpenAt(home, "stage2-file-save.bin", FS_READ, 0) == S_NOTFOUND, "new cold-boot fixture absent");
  stage = FileStage(home, "stage2-file-save.bin", 0);
  for (u32 i = 0; i < 257; i++) bytes[i] = (u8)(i * 19 + 7);
  require(stage > 0 && FileWriteAt(stage, 0, bytes, 257) == 257 &&
          FilePublish(stage, &commit) == 0 && !commit.durable &&
          FileCommit(stage, &commit) == 0 && commit.durable &&
          FileCommit(stage, &commit) == 0 && commit.durable && Close(stage) == 0,
          "versioned save and repeatable durable acknowledgement");
  require(Close(home) == 0 && Sync() == 0, "suite cleanup persisted");
  Log("FILESYS SUITE PASS checks="); char count_text[16]; number(checks, count_text);
  Log(count_text); Log("\n"); return 0;
}
