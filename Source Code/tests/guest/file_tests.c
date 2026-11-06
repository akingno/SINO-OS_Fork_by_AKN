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
static int anon_io_test(void);
static int uaccess_io_test(void);
static int anon_oom_test(int fault);
static int child(void) {
  CliInfo info; char* argv[2];
  require(CliArguments(&info, argv, 2) == 1, "structured child arguments");
  StartInfo start;
  require(QueryStartInfo(&start) == 0 && start.policy == PROCESS_STRICT && !start.caps,
          "grant does not restore ambient caps");
  if (streq(argv[0], "anon-io")) return anon_io_test();
  if (streq(argv[0], "uaccess-io")) return uaccess_io_test();
  if (streq(argv[0], "anon-fault-oom")) {
    int terminal;
    for (int i = 0; (terminal = TerminalCurrent()) == S_NOTFOUND && i < 100; i++)
      Yield(1);
    require(terminal > 0 && TerminalMode(terminal, 0) == S_OK,
            "OOM probe owns the foreground in raw mode");
    return anon_oom_test(1);
  }
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
static int anon_mapping_test(void) {
  _Static_assert(SYS_ANON_MAP == 0x14e && SYS_ANON_UNMAP == 0x14f,
                 "anonymous memory wire IDs");
  SystemInfo system;
  StartInfo start;
  require(QuerySystemInfo(&system) == S_OK &&
          (system.features & FEATURE_ANON_MEMORY) &&
          QueryStartInfo(&start) == S_OK && start.policy == PROCESS_STRICT,
          "anonymous memory advertised to strict processes");
  require(AnonMap(0) == S_INVALID && AnonMap(0xffffffffu) == S_INVALID &&
          AnonMap(ANON_MAX_BYTES + 1) == S_LIMIT &&
          api(SYS_ANON_MAP, 1, 1, 0) == S_INVALID && !malloc(0),
          "invalid sizes, overflow, quota and reserved map arguments");

  u8* first = malloc(8193);
  int second_base = AnonMap(1);
  require(first && !((u32)first & 4095) && (u32)first >= USER_DYNAMIC_BASE &&
          second_base > 0 && (u32)second_base >= (u32)first + 12288,
          "independent aligned regions and rounded extent");
  volatile u8* second = (volatile u8*)(u32)second_base;
  require(!first[0] && !first[4095] && !first[4096] && !first[8192] && !*second,
          "all three pages and second region start zeroed");
  first[4095] = 0x56;
  first[4096] = 0x78;
  *second = 0x91;
  require(AnonMap(ANON_MAX_BYTES - 4096) == S_LIMIT &&
          AnonUnmap(first + 4096) == S_INVALID &&
          api(SYS_ANON_UNMAP, (u32)first, 1, 0) == S_INVALID &&
          first[4095] == 0x56 && first[4096] == 0x78 && *second == 0x91,
          "partial or malformed unmap leaves both regions intact");
  u32 first_base = (u32)first;
  free(first);
  free(0);
  require(api(SYS_LOG, first_base, 0, 1) == S_INVALID &&
          AnonUnmap((void*)first_base) == S_INVALID && *second == 0x91,
          "released address rejects access and double unmap");
  u8* replacement = malloc(8193);
  require((u32)replacement == first_base && !replacement[4095] &&
          !replacement[4096] && *second == 0x91, "hole reused with clean pages");
  free(replacement);
  require(AnonUnmap((void*)(u32)second_base) == S_OK, "release independent region");

  u32 regions[ANON_MAX_REGIONS];
  u32 i = 0;
  for (; i < ANON_MAX_REGIONS; i++) {
    int base = AnonMap(1);
    if (base <= 0) break;
    regions[i] = (u32)base;
  }
  require(i == ANON_MAX_REGIONS && AnonMap(1) == S_LIMIT &&
          AnonUnmap((void*)regions[0]) == S_OK &&
          AnonMap(1) == (int)regions[0], "region limit and slot reuse");
  /* Exit with the regions still mapped to exercise process-wide reclamation. */
  Log("ANON MAPPING PASS\n");
  return 0;
}
static int anon_io_test(void) {
  int dir = StartDirectory();
  int file = OpenAt(dir, "phase-d-anon.bin", FS_READ | FS_WRITE, OPEN_CREATE | OPEN_EXCLUSIVE);
  u8* in = malloc(8192);
  u8* out = malloc(8192);
  require(dir > 0 && file > 0 && in && out, "explicit grant and anonymous buffers");
  for (u32 i = 0; i < 32; i++) in[4080 + i] = (u8)(i * 13);
  require(FileWriteAt(file, 0, in + 4080, 32) == 32 &&
          FileReadAt(file, 0, out + 4080, 32) == 32 &&
          same(in + 4080, out + 4080, 32), "cross-page anonymous file I/O");
  PipePair pipe;
  require(PipeCreate(32, &pipe) == S_OK &&
          StreamWrite(pipe.write, out + 4080, 32) == 32 &&
          StreamRead(pipe.read, out + 4112, 32) == 32 &&
          same(in + 4080, out + 4112, 32), "anonymous buffers work with streams");
  require(Close(pipe.read) == S_OK && Close(pipe.write) == S_OK &&
          Close(file) == S_OK && RemoveAt(dir, "phase-d-anon.bin", 0) == S_OK &&
          Close(dir) == S_OK, "file and pipe resources released");
  free(out);
  free(in);
  Log("ANON I/O PASS\n");
  return 0;
}
static int uaccess_io_test(void) {
  int dir = StartDirectory();
  u8* buffer = malloc(8192);
  u8* edge = malloc(4096);
  require(dir > 0 && buffer && edge, "uaccess workspace and test pages");

  StartInfo* cross = (StartInfo*)(buffer + 4092);
  require(QueryStartInfo(cross) == S_OK && cross->policy == PROCESS_STRICT,
          "query writes across mapped pages");
  u8* tail = edge + 4092;
  memset(tail, 0x9b, 4);
  require(QueryStartInfo((StartInfo*)tail) == S_INVALID && tail[0] == 0x9b &&
          QueryStartInfo((StartInfo*)USER_ARGUMENTS) == S_INVALID &&
          QueryStartInfo((StartInfo*)0xfffffff8u) == S_INVALID,
          "bad tail, read-only output and overflow rejected before writing");

  OpenRequest* request = (OpenRequest*)(buffer + 4092);
  memcpy(tail, "uae", 4);
  *request = (OpenRequest){(u32)tail, FS_READ | FS_WRITE, OPEN_CREATE | OPEN_EXCLUSIVE};
  int file = api(SYS_OPEN_AT, dir, (u32)request, sizeof(*request));
  require(file > 0, "cross-page request with path NUL at page end");
  tail[3] = 'x';
  require(api(SYS_OPEN_AT, dir, (u32)request, sizeof(*request)) == S_INVALID &&
          OpenAt(dir, "uaex", FS_READ, 0) == S_NOTFOUND,
          "unterminated nested path stops at the unmapped page");

  require(FileWriteAt(file, 0, "seed", 4) == 4 &&
          FileWriteAt(file, 0, tail, 8) == S_INVALID &&
          FileReadAt(file, 0, buffer + 4094, 4) == 4 &&
          same(buffer + 4094, (const u8*)"seed", 4),
          "bad nested write leaves file intact and cross-page read works");
  tail[0] = 0x7a;
  require(FileReadAt(file, 0, tail, 8) == S_INVALID && tail[0] == 0x7a,
          "bad nested read preserves destination even for a short file");
  DirPage page = {0, 0, (u32)tail, 1, 0x1234, 0x5678};
  require(DirectoryRead(dir, &page) == S_INVALID && !page.cursor &&
          page.count == 0x1234 && page.done == 0x5678,
          "directory output failure leaves cursor and response intact");

  PipePair pipe;
  require(PipeCreate(16, &pipe) == S_OK && StreamWrite(pipe.write, "pipe", 4) == 4 &&
          StreamRead(pipe.read, tail, 8) == S_INVALID && tail[0] == 0x7a &&
          StreamRead(pipe.read, buffer + 4094, 4) == 4 &&
          same(buffer + 4094, (const u8*)"pipe", 4), "bad pipe read consumes no input");
  require(StreamWrite(pipe.write, tail, 8) == S_INVALID &&
          StreamRead(pipe.read, buffer, 8) == S_AGAIN,
          "bad pipe write produces no output");
  require(Close(pipe.read) == S_OK && Close(pipe.write) == S_OK && Close(file) == S_OK &&
          RemoveAt(dir, "uae", 0) == S_OK && Close(dir) == S_OK, "uaccess fixture cleanup");
  free(edge);
  free(buffer);
  Log("UACCESS FILE AND PIPE PASS\n");
  return 0;
}
static int anon_lazy_test(void) {
  SystemInfo info;
  require(QuerySystemInfo(&info) == S_OK && (info.features & FEATURE_ANON_LAZY),
          "on-demand mapping is discoverable");
  volatile u8* first = malloc(8192);
  volatile u8* second = malloc(8192);
  require(first && second && (u32)second >= (u32)first + 8192,
          "untouched reservations cannot overlap");
  require(QueryStartInfo((StartInfo*)((u32)second + 8188)) == S_INVALID,
          "unreserved tail rejected before preparing the anonymous prefix");
  Log("ANON LAZY RESERVED\n");
  Yield(80);
  require(first[0] == 0, "first direct read receives a zero page");
  first[0] = 0x5a;
  Log("ANON LAZY TOUCHED\n");
  Yield(80);
  require(second[0] == 0 && first[0] == 0x5a, "separate regions keep independent contents");
  PipePair pipe;
  require(PipeCreate(16, &pipe) == S_OK &&
          StreamWrite(pipe.write, (const void*)(first + 4096), 2) == 2,
          "syscall read materializes an untouched page");
  u8 data[2] = {1, 1};
  require(StreamRead(pipe.read, data, 2) == 2 && !data[0] && !data[1],
          "syscall input receives the zero-filled contents");
  Log("ANON LAZY PASS\n");
  return 0;
}

static int anon_oom_test(int fault) {
  u8* edge = malloc(3 * 4096);
  u8* pressure = malloc(ANON_MAX_BYTES - 3 * 4096);
  PipePair pipe;
  require(edge && pressure && PipeCreate(16, &pipe) == S_OK &&
          StreamWrite(pipe.write, "queued!!", 8) == 8, "prepare OOM fixtures");
  u8* tail = edge + 4092;
  memset(tail, 0x5a, 4);
  int status = S_OK;
  for (u32 i = 0; i < ANON_MAX_BYTES - 3 * 4096 && status == S_OK; i += 4096)
    status = QueryStartInfo((StartInfo*)(pressure + i));
  require(status == S_NOMEM, "64 MiB test VM reaches physical OOM");
  /* A table allocation may have left one page free. Consume it in a PDE
     already used by the mapped prefix, without preparing the target page. */
  status = QueryStartInfo((StartInfo*)(edge + 8192));
  require(status == S_OK || status == S_NOMEM, "exhaust final spare page");
  if (fault) {
    Log("ANON EXPECT FIRST-TOUCH OOM FAULT\n");
    ((volatile u8*)edge)[4096] = 1;
    require(0, "user fault returned after physical OOM");
  }
  require(StreamRead(pipe.read, tail, 8) == S_NOMEM && tail[0] == 0x5a && tail[3] == 0x5a,
          "syscall OOM leaves mapped output prefix unchanged");
  u8 data[8];
  require(StreamRead(pipe.read, data, 8) == 8 && same(data, (const u8*)"queued!!", 8),
          "syscall OOM consumes no queued bytes");
  Log("ANON OOM PASS\n");
  return 0;
}

int main(void) {
  if (streq(Arguments(), "stage3-save") || streq(Arguments(), "stage3-read")) {
    static const u8 expected[] = {0, 0x7f, 0xff, '\n', 'G', 0, '3'};
    const char* path = "/home/user/stage3-mm.bin";
    if (streq(Arguments(), "stage3-save")) {
      require(Write(path, expected, sizeof(expected)) == S_OK && Sync() == S_OK,
              "commit Stage 3 fixture bytes");
      Log("STAGE3 SAVE PASS\n");
    } else {
      u8 data[8];
      require(Read(path, data, sizeof(data)) == sizeof(expected) &&
              same(data, expected, sizeof(expected)), "cold boot bytes match");
      Log("STAGE3 READ PASS\n");
    }
    return 0;
  }
  if (streq(Arguments(), "anon-d-quota")) {
    int base = AnonMap(ANON_MAX_BYTES);
    require(base > 0 && AnonMap(1) == S_LIMIT, "exact total quota and one-byte excess");
    require(AnonUnmap((void*)((u32)base + 1)) == S_INVALID &&
            AnonUnmap((void*)USER_V1_ARGUMENTS) == S_INVALID &&
            AnonUnmap((void*)USER_DYNAMIC_END) == S_INVALID, "unaligned and external unmap rejected");
    volatile u8* tail = (volatile u8*)((u32)base + ANON_MAX_BYTES - 1);
    require(*tail == 0, "last byte of exact extent is accessible");
    *tail = 0x83;
    require(AnonUnmap((void*)(u32)base) == S_OK && AnonMap(1) == base,
            "full release returns quota and virtual space");
    Log("ANON D QUOTA PASS\n");
    return 0;
  }
  if (streq(Arguments(), "anon-map")) return anon_mapping_test();
  if (streq(Arguments(), "anon-lazy")) return anon_lazy_test();
  if (streq(Arguments(), "anon-oom")) return anon_oom_test(0);
  if (streq(Arguments(), "anon-fault-oom")) return anon_oom_test(1);
  if (streq(Arguments(), "anon-fault")) {
    int base = AnonMap(4096);
    require(base > 0 && malloc(8192), "fault fixture has two regions");
    *(volatile u8*)(u32)base = 0x5a;
    require(AnonUnmap((void*)(u32)base) == S_OK,
            "release one region while keeping another live");
    Log("ANON EXPECT RELEASED PAGE FAULT\n");
    *(volatile u8*)(u32)base = 1;
    require(0, "released mapping still writable");
  }
  if (streq(Arguments(), "anon-parent") || streq(Arguments(), "uaccess-parent")) {
    int uaccess = streq(Arguments(), "uaccess-parent");
    int dir = GrantDirectory("/home/user", FS_RIGHTS);
    require(dir > 0, "grant private test workspace");
    u32 rights = FS_READ | FS_WRITE | FS_LOOKUP | FS_CREATE | FS_REMOVE;
    result(child_run(dir, rights | (uaccess ? FS_ENUM : 0),
                     uaccess ? "uaccess-io" : "anon-io", "/boot/fstest.snx"), PROCESS_EXITED);
    require(Close(dir) == S_OK, "parent closes directory grant");
    Log(uaccess ? "UACCESS FILE PARENT PASS\n" : "ANON PARENT PASS\n");
    return 0;
  }
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
