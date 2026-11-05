/* G1 session client. Strict, explicitly attached, bounded by the C run lease.
 */
#include "stream.h"
static int terminal;
static u32 started;
static int output(const void *bytes, u32 count) {
  u32 written = 0;
  return StreamWriteAllFor(terminal, bytes, count, 300, &written);
}
static int text(const char *s) {
  return output(s, strlen(s));
}
static int numbered(const char *prefix, u32 value) {
  char n[12];
  number(value, n);
  if (text(prefix) < 0 || text(n) < 0)
    return S_IO;
  return text("\n");
}
int terminal_probe(void) {
  CliInfo cli;
  StartInfo info;
  if (QueryCliInfo(&cli) || QueryStartInfo(&info) ||
      info.policy != PROCESS_STRICT || info.caps)
    return 1;
  started = StreamTicks();
  while ((terminal = TerminalCurrent()) == S_NOTFOUND &&
         StreamTicks() - started < 100)
    Yield(1);
  if (terminal < 0 || text("G1 session probe\nLine input; raw / flood / "
                           "unicode / fault. Ctrl+D exits.\n") < 0)
    return 1;
  if (streq(cli.bytes, "ignore-hangup"))
    for (;;)
      Yield(1);
  int raw = 0;
  char line[257];
  u32 length = 0;
  for (;;) {
    TerminalEvent event;
    for (int i = 0; i < 4; i++) {
      int status = TerminalNextEvent(terminal, &event);
      if (status == S_CLOSED)
        return 0;
      if (status == S_AGAIN)
        break;
      if (status < 0)
        return 1;
      if (event.type == TERM_KEY) {
        if (raw && event.value == 27) {
          if (TerminalMode(terminal, TERM_LINE | TERM_ECHO) < 0 ||
              text("LINE ready\n") < 0)
            return 1;
          raw = 0;
        } else if (numbered("KEY ", event.value) < 0)
          return 1;
      } else if (event.type == TERM_RESIZE) {
        char w[12], h[12];
        number(event.width, w);
        number(event.height, h);
        if (text("RESIZE ") < 0 || text(w) < 0 || text(" ") < 0 ||
            text(h) < 0 || text("\n") < 0)
          return 1;
        if (streq(cli.bytes, "qa")) {
          Log("G1 RESIZE ");
          Log(w);
          Log(" ");
          Log(h);
          Log("\n");
        }
      }
    }
    u8 bytes[256];
    int count = TerminalRead(terminal, bytes, sizeof(bytes));
    if (count == S_AGAIN) {
      Yield(1);
      continue;
    }
    if (count == 0 || count == S_CLOSED)
      return 0;
    if (count < 0)
      return 1;
    for (int i = 0; i < count; i++) {
      if (raw) {
        if (numbered("BYTE ", bytes[i]) < 0)
          return 1;
        continue;
      }
      if (length == 256)
        return 1;
      line[length++] = (char)bytes[i];
      if (bytes[i] != '\n')
        continue;
      line[--length] = 0;
      if (streq(line, "raw") || streq(line, "rawpause") ||
          streq(line, "stall")) {
        if (TerminalMode(terminal, 0) < 0 || text("RAW ready\n") < 0)
          return 1;
        raw = 1;
        if (streq(line, "rawpause"))
          Yield(150);
        if (streq(line, "stall"))
          Yield(1000);
      } else if (streq(line, "flood")) {
        u8 block[257];
        for (u32 j = 0; j < sizeof(block); j++)
          block[j] = j % 64 == 63 ? '\n' : 'f';
        u32 total = 0;
        while (total < 12289) {
          u32 n = 12289 - total;
          if (n > sizeof(block))
            n = sizeof(block);
          if (output(block, n) < 0)
            return 1;
          total += n;
        }
        if (text("\nFLOOD DONE 12289\n") < 0)
          return 1;
        if (streq(cli.bytes, "qa"))
          Log("G1 FLOOD PASS bytes=12289\n");
      } else if (streq(line, "unicode")) {
        const u8 first[] = {0xe4, 0xbd};
        const u8 last[] = {0xa0, 0xe5, 0xa5, 0xbd, 0, 'X', 27, 'Y', '\n'};
        if (output(first, sizeof(first)) < 0)
          return 1;
        Yield(3);
        if (output(last, sizeof(last)) < 0)
          return 1;
      } else if (streq(line, "fault")) {
        __asm__ volatile("ud2");
      } else if (text("LINE ") < 0 || text(line) < 0 || text("\n") < 0)
        return 1;
      length = 0;
    }
  }
}
