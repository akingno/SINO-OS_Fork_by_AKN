/* Small strict text tools used to exercise G3 argv and final-state contracts.
 */
#include "stream.h"
#include "gsh_parse.h"
static CliInfo cli;
static int put(const char *text) {
  u32 written;
  return StreamWriteAllFor(cli.out, text, strlen(text), 300, &written);
}
int main(void) {
  StartInfo start;
  if (QueryStartInfo(&start) || start.policy != PROCESS_STRICT || start.caps ||
      QueryCliInfo(&cli) || !cli.argc || TerminalCurrent() < 0 ||
      StartDirectory() != S_NOTFOUND)
    return 2;
  const char *command = cli.bytes + cli.offsets[0];
  if (streq(command, "spin")) {
    if (put("worker waiting\n") < 0)
      return 1;
    for (;;)
      Yield(1);
  }
  if (streq(command, "fault"))
    __asm__ volatile("ud2");
  if (streq(command, "exitcode")) {
    i32 code;
    if (cli.argc != 2 || shell_integer(cli.bytes + cli.offsets[1], &code))
      return 2;
    return code;
  }
  if (streq(command, "echo")) {
    for (u32 i = 1; i < cli.argc; i++) {
      if ((i > 1 && put(" ") < 0) || put(cli.bytes + cli.offsets[i]) < 0)
        return 1;
    }
    return put("\n") < 0;
  }
  if (streq(command, "args")) {
    char n[12];
    number(cli.argc, n);
    if (put("argc=") < 0 || put(n) < 0 || put("\n") < 0)
      return 1;
    for (u32 i = 0; i < cli.argc; i++) {
      const char *arg = cli.bytes + cli.offsets[i];
      number(i, n);
      if (put("arg[") < 0 || put(n) < 0 || put("]=") < 0)
        return 1;
      number(strlen(arg), n);
      if (put(n) < 0 || put(":") < 0 || put(arg) < 0 || put("\n") < 0)
        return 1;
    }
    return 0;
  }
  return 127;
}
