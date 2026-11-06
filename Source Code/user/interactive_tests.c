#include "interactive.h"
#include "stream.h"
static void require(int yes, const char* why) {
  if (!yes) {
    Log("INTERACTIVE SELFTEST FAIL: ");
    Log(why);
    Log("\n");
    Exit(1);
  }
}
static InteractiveSpec spec(u32 program, u32 id, const char* arg) {
  InteractiveSpec s;
  memset(&s, 0, sizeof(s));
  s.size = sizeof(s);
  s.version = 1;
  s.program = program;
  s.id = id;
  s.argc = 1;
  s.bytes = strlen(arg) + 1;
  copy(s.argv, arg);
  return s;
}
static void output(CliInfo* c, const char* text) {
  u32 n;
  require(StreamWriteAllFor(c->out, text, strlen(text), 200, &n) == 0,
          "session output");
}
enum { GSH_SMOKE, GSH_HOLD_RESULT, GSH_REJECT_START, GSH_HANGUP };
static int gsh_smoke(int scenario) {
  int term = TerminalCreate(120, 25);
  u32 rights = FS_READ | FS_LOOKUP | FS_ENUM;
  int dir = GrantDirectory("/home/user", rights | FS_DELEGATE);
  InteractiveSpec root = spec(IX_SHELL, 0, "shell");
  root.rights = rights;
  int shell = InteractiveStart(term, &root, dir, 0);
  require(term > 0 && dir > 0 && shell > 0, "headless gsh launch");
  const char command[] = "echo stage3-g\n";
  const char* marker = scenario == GSH_REJECT_START ? "start failed " : "stage3-g\n";
  u32 marker_bytes = strlen(marker);
  u32 sent = 0, matched = 0, begin = StreamTicks();
  int ready = 0, child = 0, finished = 0, prompt_after = 0, hangup_observed = 0;
  while (StreamTicks() - begin < 500 && !prompt_after) {
    char bytes[128];
    int n = TerminalDrain(term, bytes, sizeof(bytes));
    require(n >= 0 || n == S_AGAIN, "headless drain");
    for (int i = 0; i < n && matched < marker_bytes; i++)
      matched = bytes[i] == marker[matched] ? matched + 1 : (bytes[i] == marker[0]);
    TerminalEdit edit;
    int status = TerminalCall(TERM_EDIT_VIEW, term, &edit, sizeof(edit), 0, 0);
    if (status == S_OK) {
      require(TerminalCall(TERM_EDIT_ACK, term, 0, 0, edit.serial, 0) == S_OK,
              "headless edit acknowledgement");
      ready = 1;
      prompt_after = finished && matched == marker_bytes && streq(edit.text, "gsh:/ $ ");
    } else require(status == S_AGAIN, "headless edit query");
    if (ready && sent < sizeof(command) - 1) {
      status = TerminalFeed(term, TERM_TEXT, command + sent, 1, 0);
      if (status == S_OK) sent++;
      else require(status == S_AGAIN, "headless input backpressure");
    }
    if (child > 0) {
      if (scenario == GSH_HOLD_RESULT) {
        ProcessCompletion completed;
        require(ProcessWaitFor(child, &completed, 300) == S_OK &&
                completed.reason == PROCESS_EXITED && !completed.code, "wait for echo worker");
        Log("STAGE4 GSH HOLD\n");
        Yield(100);
        Log("STAGE4 GSH RELEASE\n");
        scenario = GSH_SMOKE;
      }
      status = InteractiveFinish(term, child);
      if (status == S_OK) {
        require(Close(child) == S_OK, "headless worker cleanup");
        child = 0;
        finished = 1;
      } else require(status == S_AGAIN, "headless worker result");
    } else if (!finished) {
      InteractiveSpec request;
      status = InteractiveRequest(term, &request);
      if (status == S_OK) {
        require(request.program == IX_COMMAND && request.argc == 2 && !request.rights,
                "headless gsh routes echo to strict worker");
        if (scenario == GSH_HANGUP) {
          Yield(4);
          require(TerminalFeed(term, TERM_HANGUP, 0, 0, 0) == S_OK, "hang up a pending broker request");
          ProcessCompletion completed;
          require(ProcessWaitFor(shell, &completed, 100) == S_OK &&
                  completed.reason == PROCESS_EXITED && completed.code == 1,
                  "hangup wakes the Shell and returns its error exit");
          hangup_observed = 1;
          break;
        }
        child = InteractiveStart(term, &request, scenario == GSH_REJECT_START ? dir : 0, 1);
        if (scenario == GSH_REJECT_START) {
          require(child == S_DENIED, "headless broker rejects unauthorized grant");
          finished = 1;
        } else {
          require(child > 0, "headless echo worker launch");
        }
      } else require(status == S_AGAIN, "headless worker request");
    }
    Yield(1);
  }
  require(hangup_observed || prompt_after, "gsh reaches the expected final state");
  require(Close(term) == S_OK && Close(shell) == S_OK && Close(dir) == S_OK,
          "headless gsh resources released");
  Log(scenario == GSH_HANGUP ? "STAGE4 GSH HANGUP PASS\n" : "STAGE3 GSH PASS\n");
  return 0;
}
int main(void) {
  StartInfo start;
  require(QueryStartInfo(&start) == 0, "startup policy");
  if (start.policy == PROCESS_COMPAT && streq(Arguments(), "stage3-shell"))
    return gsh_smoke(GSH_SMOKE);
  if (start.policy == PROCESS_COMPAT && streq(Arguments(), "stage4-shell"))
    return gsh_smoke(GSH_HOLD_RESULT);
  if (start.policy == PROCESS_COMPAT && streq(Arguments(), "stage4-shell-error"))
    return gsh_smoke(GSH_REJECT_START);
  if (start.policy == PROCESS_COMPAT && streq(Arguments(), "stage4-shell-hangup"))
    return gsh_smoke(GSH_HANGUP);
  if (start.policy == PROCESS_STRICT) {
    CliInfo cli;
    require(QueryCliInfo(&cli) == 0, "atomic cli before first instruction");
    int t = TerminalCurrent();
    require(t > 0, "atomic attachment");
    require(StreamWrite(cli.in, "x", 1) == S_DENIED &&
                StreamRead(cli.out, &start, 1) == S_DENIED,
            "role rights");
    require(Spawn("/boot/brokerqa.snx") == S_DENIED, "strict spawn denied");
    require(api(SYS_DUP, t, 0, 0) == S_BADHANDLE,
            "zero rights duplicate rejected");
    int reduced = api(SYS_DUP, cli.out, 8, 0);
    require(reduced > 0, "reduced stream duplicate");
    require(TerminalWrite(reduced, "x", 1) == S_DENIED,
            "reduced rights cannot write");
    Close(reduced);
    require(TerminalMode(cli.out, 0) == S_DENIED,
            "stream cannot change terminal mode");

    int grant = StartDirectory();
    require(grant > 0, "startup directory bound atomically");
    require(OpenAt(grant, "../home/user", FS_READ, OPEN_DIRECTORY) < 0,
            "cannot escape root");
    require(OpenAt(grant, "x", FS_WRITE, OPEN_CREATE) == S_BADHANDLE ||
                OpenAt(grant, "x", FS_WRITE, OPEN_CREATE) == S_DENIED,
            "read-only directory attenuation");
    if (streq(cli.bytes, "worker")) {
      require(cli.argc == 3 && !cli.bytes[cli.offsets[1]] &&
                  streq(cli.bytes + cli.offsets[2], "你好"),
              "argv boundaries and UTF-8");
      output(&cli, "G2 child output\n");
      return 7;
    }
    if (streq(cli.bytes, "fault")) {
      __asm__ volatile("ud2");
    }
    if (streq(cli.bytes, "spin")) {
      output(&cli, "G2 worker waiting\n");
      for (;;)
        Yield(1);
    }
    int automatic = streq(cli.bytes, "auto") || streq(cli.bytes, "auto-fail") ||
                    streq(cli.bytes, "auto-spin");
    u32 id = 0;
    output(&cli, "G2 ready\n");
    for (;;) {
      char line[32];
      int n = 3;
      if (!automatic) {
        for (;;) {
          TerminalEvent event;
          while (TerminalNextEvent(t, &event) == 0)
            if (event.type == TERM_INTERRUPT) {
              output(&cli, "\nG2 interrupt\nG2 ready\n");
            }
          n = StreamRead(cli.in, line, 31);
          if (n != S_AGAIN)
            break;
          Yield(1);
        }
      }
      if (n == 0 || n == S_CLOSED)
        return 0;
      require(n > 0, "shell input");
      line[n] = 0;
      TerminalEvent e;
      while (TerminalNextEvent(t, &e) == 0)
        if (e.type == TERM_INTERRUPT)
          output(&cli, "G2 interrupt\n");
      const char* mode =
          (automatic && !streq(cli.bytes, "auto-spin")) || streq(line, "go\n")
              ? "worker"
          : streq(line, "fault\n") ? "fault"
                                   : "spin";
      InteractiveSpec s = spec(2, ++id, mode);
      s.rights = FS_READ | FS_LOOKUP;
      s.argc = 3;
      s.offsets[1] = s.bytes;
      s.argv[s.bytes++] = 0;
      s.offsets[2] = s.bytes;
      copy(s.argv + s.bytes, "你好");
      s.bytes += 7;
      require(InteractiveSubmit(t, &s) == 0, "typed intent");
      require(InteractiveSubmit(t, &s) == S_BUSY, "single request quota");
      InteractiveResult result;
      u32 began = StreamTicks();
      int status;
      while ((status = InteractiveResultRead(t, id, &result)) == S_AGAIN &&
             StreamTicks() - began < 1000) {
        char byte;
        int read = TerminalRead(t, &byte, 1);
        if (read == S_DENIED) {
          require(TerminalWrite(t, "x", 1) == S_DENIED,
                  "background output denied");
          require(TerminalMode(t, 0) == S_DENIED, "background mode denied");
        }
        Yield(1);
      }
      require(status == 0 && result.id == id,
              "request identity and completion");
      require(InteractiveResultRead(t, id, &result) == S_NOTFOUND,
              "consume result once");
      if (!result.status)
        require(result.completion.pid > 0, "typed final completion");
      output(&cli, "G2 restored\n");
      if (automatic)
        return streq(cli.bytes, "auto-fail")
                   ? (result.status == S_LIMIT ? 0 : 1)
                   : (result.status ? 1 : 0);
      output(&cli, "G2 ready\n");
    }
  }
  int a = TerminalCreate(80, 25), b = TerminalCreate(80, 25);
  require(a > 0 && b > 0, "two controls");
  int dir =
      GrantDirectory("/boot", FS_READ | FS_LOOKUP | FS_ENUM | FS_DELEGATE);
  require(dir > 0, "directory grant");
  InteractiveSpec s = spec(1, 0, "auto");
  s.rights = FS_READ | FS_LOOKUP;
  int shell[2] = {InteractiveStart(a, &s, dir, 0),
                  InteractiveStart(b, &s, dir, 0)};
  require(shell[0] > 0 && shell[1] > 0, "atomic interactive start");
  InteractiveSpec bad = s;
  bad.offsets[0] = 1;
  require(InteractiveStart(a, &bad, dir, 0) < 0, "second root rejected");
  u32 offset = 0;
  RunSpec finite = {
      48, 1, 0, (u32) "/boot/spin.snx", 0, 0, 0, (u32)&offset, 0, 0, 5000, 0};
  int captures[2] = {StartRun(&finite), StartRun(&finite)};
  require(captures[0] > 0 && captures[1] > 0 && StartRun(&finite) == S_LIMIT,
          "independent finite capture quota");
  int control[2] = {a, b}, child[2] = {0, 0};
  u32 begin = StreamTicks();
  while ((!child[0] || !child[1]) && StreamTicks() - begin < 300) {
    for (int i = 0; i < 2; i++) {
      InteractiveSpec q;
      int r = InteractiveRequest(control[i], &q);
      if (r == 0) {
        require(!child[i] && q.id == 1, "owned request");
        require(InteractiveRequest(control[1 - i], &bad) == 0 ||
                    InteractiveRequest(control[1 - i], &bad) == S_AGAIN,
                "other request independent");
        InteractiveSpec altered = q;
        altered.argv[0] = 'x';
        require(InteractiveStart(control[i], &altered, dir, 1) == S_INVALID,
                "tampered intent rejected");
        require(InteractiveResultRead(control[1 - i], 1,
                                      (InteractiveResult*)&altered) == S_DENIED,
                "controller cannot consume shell result");
        child[i] = InteractiveStart(control[i], &q, dir, 1);
        require(child[i] > 0, "broker launch");
        require(InteractiveStart(control[i], &q, dir, 1) == S_BUSY,
                "replay rejected");
      }
    }
    Yield(1);
  }
  require(child[0] > 0 && child[1] > 0, "two active jobs");
  for (int i = 0; i < 2; i++) {
    int r;
    begin = StreamTicks();
    while ((r = InteractiveFinish(control[i], child[i])) == S_AGAIN &&
           StreamTicks() - begin < 300)
      Yield(1);
    require(r == 0, "foreground restored before completion delivery");
    Close(child[i]);
    ProcessCompletion done;
    require(ProcessWaitFor(shell[i], &done, 300) == 0 &&
                done.reason == PROCESS_EXITED && !done.code,
            "root completed");
    Close(shell[i]);
    Close(control[i]);
  }
  for (int i = 0; i < 2; i++) {
    CancelProcess(captures[i]);
    ProcessCompletion done;
    require(ProcessWaitFor(captures[i], &done, 300) == 0,
            "finite task cleanup");
    Close(captures[i]);
  }
  int t = TerminalCreate(80, 25);
  s = spec(1, 0, "auto-fail");
  s.rights = FS_READ | FS_LOOKUP;
  int root = InteractiveStart(t, &s, dir, 0);
  require(root > 0, "rollback root");
  int blockers[6];
  for (int i = 0; i < 6; i++) {
    blockers[i] = SpawnTracked("/boot/spin.snx", "");
    require(blockers[i] > 0, "fill process table");
  }
  InteractiveSpec request;
  begin = StreamTicks();
  while (InteractiveRequest(t, &request) == S_AGAIN &&
         StreamTicks() - begin < 300)
    Yield(1);
  TerminalInfo before, after;
  require(TerminalGetInfo(t, &before) == 0, "before failed start");
  require(InteractiveStart(t, &request, dir, 1) == S_LIMIT,
          "failed child start reports quota");
  require(TerminalGetInfo(t, &after) == 0 &&
              after.foreground == before.foreground &&
              after.mode == before.mode,
          "failed launch preserves foreground");
  ProcessCompletion result;
  require(ProcessWaitFor(root, &result, 300) == 0 && !result.code,
          "failed intent delivered once");
  Close(root);
  for (int i = 0; i < 6; i++) {
    CancelProcess(blockers[i]);
    require(ProcessWaitFor(blockers[i], &result, 300) == 0, "blocker cleanup");
    Close(blockers[i]);
  }
  Close(t);
  t = TerminalCreate(80, 25);
  s = spec(1, 0, "auto-fail");
  s.rights = FS_READ | FS_LOOKUP;
  root = InteractiveStart(t, &s, dir, 0);
  require(root > 0, "handle quota root");
  begin = StreamTicks();
  while (InteractiveRequest(t, &request) == S_AGAIN &&
         StreamTicks() - begin < 300)
    Yield(1);
  int held[24], used = 0;
  int event = api(SYS_EVENT_CREATE, 0, 0, 0);
  require(event > 0, "quota event");
  held[used++] = event;
  int duplicate;
  while ((duplicate = api(SYS_DUP, event, 1, 0)) > 0)
    held[used++] = duplicate;
  require(InteractiveStart(t, &request, dir, 1) == S_LIMIT,
          "parent handle exhaustion rollback");
  while (used)
    Close(held[--used]);
  require(ProcessWaitFor(root, &result, 300) == 0 && !result.code,
          "handle failure delivered");
  Close(root);
  Close(t);
  t = TerminalCreate(80, 25);
  s = spec(1, 0, "auto-spin");
  s.rights = FS_READ | FS_LOOKUP;
  root = InteractiveStart(t, &s, dir, 0);
  require(root > 0, "owned group root");
  begin = StreamTicks();
  while (InteractiveRequest(t, &request) == S_AGAIN &&
         StreamTicks() - begin < 300)
    Yield(1);
  int job = InteractiveStart(t, &request, dir, 1);
  require(job > 0, "owned group child");
  int copy = api(SYS_DUP, t, 1, 0);
  require(copy > 0, "duplicate controller");
  Close(t);
  require(GetProcessResult(root, &result) == S_AGAIN,
          "remaining controller retains group");
  Close(copy);
  require(ProcessWaitFor(root, &result, 300) == 0 &&
              result.reason == PROCESS_CANCELED,
          "last controller cancels root");
  Close(root);
  require(ProcessWaitFor(job, &result, 300) == 0 &&
              result.reason == PROCESS_CANCELED,
          "last controller cancels child");
  Close(job);
  Close(dir);
  Log("INTERACTIVE SUITE PASS\n");
  return 0;
}
