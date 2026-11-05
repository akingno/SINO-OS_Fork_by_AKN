/* Private-VM G4 fixtures. Never used on the persistent development disk. */
#include "sdk.h"
static char path[128];
static u8 bytes[512];
static const char *names[] = {"tiny.txt",   "empty.txt", ".hidden",
                              "binary.txt", "split.txt", "big.txt",
                              "pages.txt",  "你好.txt",  0};
static void require(int yes) {
  if (!yes) {
    Log("G4 FIXTURE FAIL\n");
    Exit(1);
  }
}
int main(void) {
  if (streq(Arguments(), "clip") || streq(Arguments(), "clip-bad")) {
    const char *text =
        streq(Arguments(), "clip") ? "echo pasted" : "echo one\necho two";
    require(api(SYS_CLIP_SET, (u32)text, strlen(text), 0) == 0);
    return 0;
  }
  int cleanup = streq(Arguments(), "cleanup");
  if (!cleanup)
    require(Mkdir("/home/user/Documents/g4") == 0);
  for (u32 i = 0; names[i]; i++) {
    copy(path, "/home/user/Documents/g4/");
    copy(path + strlen(path), names[i]);
    if (cleanup) {
      require(Remove(path, 0) == 0);
      continue;
    }
    require(Write(path, "", 0) == 0);
    if (i == 0)
      require(Write(path, "hello", 5) == 0);
    if (i == 3) {
      const u8 binary[] = {'A', 0, 27, 'B'};
      require(Write(path, binary, sizeof(binary)) == 0);
    }
    if (i == 4) {
      memset(bytes, 'x', 511);
      bytes[511] = 0xe4;
      require(Write(path, bytes, 512) == 0);
      int root =
          GrantDirectory("/home/user/Documents/g4", FS_LOOKUP | FS_WRITE);
      require(root > 0);
      int file = OpenAt(root, names[i], FS_WRITE, 0);
      require(file > 0);
      const u8 rest[] = {0xbd, 0xa0, 'Z'};
      require(FileWriteAt(file, 512, rest, 3) == 3);
      Close(file);
      Close(root);
    }
    if (i == 5) {
      int root =
          GrantDirectory("/home/user/Documents/g4", FS_LOOKUP | FS_WRITE);
      require(root > 0);
      int file = OpenAt(root, names[i], FS_WRITE, 0);
      require(file > 0);
      memset(bytes, 'x', sizeof(bytes));
      bytes[511] = '\n';
      for (u32 offset = 0; offset < 65536; offset += 512) {
        if (offset == 65024)
          memcpy(bytes + 504, "G4-END\n", 7);
        require(FileWriteAt(file, offset, bytes, sizeof(bytes)) ==
                sizeof(bytes));
      }
      Close(file);
      Close(root);
    }
    if (i == 7)
      require(Write(path, "Chinese path", 12) == 0);
    if (i == 6) {
      u32 n = 0;
      for (u32 row = 0; row < 30; row++) {
        memcpy(bytes + n, "page line\n", 10);
        n += 10;
      }
      memcpy(bytes + n, "PAGE-END\n", 9);
      n += 9;
      require(Write(path, bytes, n) == 0);
    }
  }
  if (cleanup)
    require(Remove("/home/user/Documents/g4", 1) == 0);
  Log("G4 FIXTURE PASS\n");
  return 0;
}
