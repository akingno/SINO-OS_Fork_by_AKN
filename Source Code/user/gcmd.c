/* Small strict text tools used to exercise G3 argv and final-state contracts.
 */
#include "stream.h"
#include "gsh_parse.h"
#include "shell_echo.h"
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
  if (streq(command, "sleep")) {
    i32 seconds;
    if (cli.argc != 2 || shell_integer(cli.bytes + cli.offsets[1], &seconds) ||
        seconds < 0 || seconds > 3600) {
      put("sleep: expected 0..3600 seconds\n");
      return 2;
    }
    SystemInfo info;
    if (QuerySystemInfo(&info))
      return 1;
    u32 begin = StreamTicks(), duration = (u32)seconds * info.ticks_hz;
    while (!StreamTimeoutExpired(StreamTicks(), begin, duration))
      Yield(1);
    return 0;
  }
  if (streq(command, "exitcode")) {
    i32 code;
    if (cli.argc != 2 || shell_integer(cli.bytes + cli.offsets[1], &code))
      return 2;
    return code;
  }
  if (streq(command, "echo")) {
    char out[1024];
    int bytes = shell_echo(cli.bytes, cli.offsets + 1, cli.argc - 1,
                           out, sizeof(out));
    if (bytes < 0) {
      put("echo: unsupported escape\n");
      return 2;
    }
    u32 written;
    return StreamWriteAllFor(cli.out, out, (u32)bytes, 300, &written) < 0;
  }
  if ((streq(command, "date") || streq(command, "time") ||
       streq(command, "uptime") || streq(command, "about") ||
       streq(command, "credits")) &&
      cli.argc != 1)
    return 2;
  if (streq(command, "date") || streq(command, "time")) {
    CalendarTime date;
    int error = Calendar(&date);
    if (error < 0) {
      put("RTC unavailable\n");
      return 1;
    }
    char out[32];
    if (streq(command, "date")) {
      number(date.year, out);
      if (put(out) < 0 || put("-") < 0)
        return 1;
    }
    u32 values[3] = {date.hour, date.minute, date.second};
    int count = 3;
    if (streq(command, "date")) {
      values[0] = date.month;
      values[1] = date.day;
      count = 2;
    }
    for (int i = 0; i < count; i++) {
      out[0] = (char)('0' + values[i] / 10);
      out[1] = (char)('0' + values[i] % 10);
      out[2] = 0;
      if (put(out) < 0 || (i + 1 < count && put(count == 2 ? "-" : ":") < 0))
        return 1;
    }
    return put("\n") < 0;
  }
  if (streq(command, "uptime")) {
    SystemInfo info;
    if (QuerySystemInfo(&info) || !info.ticks_hz)
      return 1;
    char n[12];
    number(StreamTicks() / info.ticks_hz, n);
    return put("up ") < 0 || put(n) < 0 || put(" seconds\n") < 0;
  }
  if (streq(command, "uname") || streq(command, "ver")) {
    if (cli.argc > 2 ||
        (cli.argc == 2 && (!streq(command, "uname") ||
                           !streq(cli.bytes + cli.offsets[1], "-a"))))
      return 2;
    return put(streq(command, "ver") || cli.argc == 2
                   ? "SINO 0.10 SINO32 i386\n"
                   : "SINO\n") < 0;
  }
  if (streq(command, "about") || streq(command, "credits"))
    return put("SINO 0.10 + SINO user Shell\nProject-specific C/assembly; not "
               "a FreeDOS or Linux kernel fork.\nTools: NASM, GCC/Binutils, "
               "Node.js, QEMU; firmware: SeaBIOS/VGA BIOS.\nReferences: Intel "
               "architecture manuals and QEMU documentation.\n") < 0;
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
