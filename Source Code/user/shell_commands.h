#ifndef SINO_SHELL_COMMANDS_H
#define SINO_SHELL_COMMANDS_H
#include "../src32/types.h"
typedef struct {
  const char *name;
  u32 kind;
  const char *usage;
} ShellCommand;
static const ShellCommand shell_commands[] = {
    {"run", 6, "run [-s] program.snx [argument]"},
    {"kill", 6, "kill pid (applications started by this Shell)"},
    {"install", 6, "install package.sxp | /quiet:path | /remove:id"},
    {"nettest", 6, "nettest"},
    {"whoami", 0, "whoami"},
    {"hostname", 0, "hostname [name]"},
    {"env", 0, "env"},
    {"sleep", 1, "sleep seconds (0..3600)"},
 {"clear",0,"clear"},
    {"touch", 5, "touch path"},
    {"mkdir", 5, "mkdir path"},
    {"rm", 5, "rm path"},
    {"rmdir", 5, "rmdir path"},
    {"write", 5, "write path text..."},
    {"append", 5, "append path text..."},
    {"cp", 5, "cp source target"},
    {"mv", 5, "mv source target"},
    {"sync", 5, "sync"},
    {"help", 0, "help [command]"},
    {"pwd", 0, "pwd"},
    {"cd", 0, "cd [path]"},
    {"history", 0, "history"},
    {"status", 0, "status"},
    {"exit", 0, "exit [0..255]"},
    {"which", 0, "which command"},
    {"echo", 1, "echo [-n|-e] [text...]"},
    {"date", 1, "date"},
    {"time", 1, "time"},
    {"uptime", 1, "uptime"},
    {"uname", 1, "uname [-a]"},
    {"ver", 1, "ver"},
    {"about", 1, "about"},
    {"credits", 1, "credits"},
    {"ls", 2, "ls [-a|-l|-la|-al] [path]"},
    {"stat", 2, "stat path"},
    {"cat", 2, "cat path"},
    {"view", 2, "view path"},
    {"head", 2, "head [-n N] path"},
    {"tail", 2, "tail [-n N] path"},
    {"wc", 2, "wc [-l|-w|-c] path"},
    {"grep", 2, "grep [-n] text path"},
    {"hexdump", 2, "hexdump path"},
    {"tree", 2, "tree [path]"},
    {"find", 2, "find [path] [text]"},
    {"args", 3, "args [text...]"},
    {"exitcode", 3, "exitcode signed-i32"},
    {"spin", 3, "spin"},
    {"fault", 3, "fault"},
    {"mem", 4, "mem: needs a public live-memory query"},
    {"free", 4, "free: needs a public live-memory query"},
    {"df", 4, "df: needs a public storage query"},
    {"ps", 4, "ps: needs an authorized process query"},
};
static int shell_equal(const char *a, const char *b) {
  while (*a && *a == *b) {
    a++;
    b++;
  }
  return *a == *b;
}
static const char *shell_alias(const char *s) {
  const char *from[] = {"DIR", "dir", "TYPE", "type", "VER", "LS", "CAT",
                        "cls", "md", "del", "copy", "ren", 0};
  const char *to[] = {"ls", "ls", "cat", "cat", "ver", "ls", "cat",
                      "clear", "mkdir", "rm", "cp", "mv"};
  for (u32 i = 0; from[i]; i++)
    if (shell_equal(s, from[i]))
      return to[i];
  return s;
}
static const ShellCommand *shell_command(const char *s) {
  s = shell_alias(s);
  for (u32 i = 0; i < sizeof(shell_commands) / sizeof(*shell_commands); i++)
    if (shell_equal(s, shell_commands[i].name))
      return shell_commands + i;
  return 0;
}
#endif
