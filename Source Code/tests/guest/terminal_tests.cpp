#include "stream.h"
#include "handle_fixture.h"
typedef struct {u32 op,handle,buffer,bytes,value,extra;} Request;
static int call(u32 op,u32 h,const void* b,u32 n,u32 v,u32 x){Request r={op,h,(u32)b,n,v,x};return api(0x14a,(u32)&r,24,0);}
static void check(int ok,const char* why){if(!ok){Log("TERMINAL SELFTEST FAIL: ");Log(why);Log("\n");Exit(1);}}
static u8 bytes[256];
static u32 info[8];
static int start_program(const char* path,const char* mode){u32 offset=0;RunSpec s={48,1,0,(u32)path,1,strlen(mode)+1,(u32)mode,(u32)&offset,0,0,500,0};return StartRun(&s);}
static int start(const char* mode){return start_program("/boot/termtest.snx",mode);}
static void done(int h,u32 reason){RunResult r;check(RunWaitFor(h,&r,600)==0&&r.completion.reason==reason&&!r.completion.code,"typed child result");check(Close(h)==0,"close observer");}
static int uaccess_terminal_parent(void) {
  const u32 base = AnonMap(4096);
  check(AnonMapStatus(base) == S_OK, "terminal test page");
  auto* edge = (u8*)base;
  u8* tail = edge + 4092;
  check(api(SYS_TERMINAL, (u32)tail, sizeof(Request), 0) == S_INVALID,
        "bad outer terminal request stops before dispatch");
  u32 offset = 0;
  RunSpec bad = {48, 1, 0, (u32)"/boot/termtest.snx", 1, 8,
                 (u32)tail, (u32)&offset, 0, 0, 100, 0};
  check(StartRun(&bad) == S_INVALID, "bad nested argv cannot start a process");
  int control = call(TERM_CREATE, 0, 0, 0, 80, 25);
  int child = start("uaccess-child");
  check(control > 0 && child > 0 && call(TERM_FOREGROUND, control, 0, 0, child, 0) > 0,
        "terminal probe foreground");
  memset(tail, 0x78, 4);
  check(call(TERM_INFO, control, tail, sizeof(TerminalInfo), 0, 0) == S_INVALID &&
        tail[0] == 0x78, "terminal info bad tail preserves output");
  check(call(TERM_DRAIN, control, tail, 8, 0, 0) == S_INVALID,
        "terminal drain rejects bad destination");
  u32 begin = StreamTicks();
  int n;
  while ((n = call(TERM_DRAIN, control, bytes, 4, 0, 0)) == S_AGAIN &&
         StreamTicks() - begin < 100) Yield(1);
  check(n == 4 && streq((const char*)bytes, "okay"), "valid drain reads the whole queued output");
  check(call(TERM_FEED, control, tail, 8, TERM_TEXT, 0) == S_INVALID &&
        call(TERM_FEED, control, "data", 4, TERM_TEXT, 0) == S_OK,
        "bad nested feed cannot enqueue its mapped prefix");
  done(child, PROCESS_EXITED);
  check(Close(control) == S_OK, "terminal control released");
  check(AnonUnmap(edge) == S_OK, "release terminal test page");
  Log("UACCESS TERMINAL PASS\n");
  return 0;
}
static int uaccess_terminal_child(int term) {
  const u32 base = AnonMap(4096);
  check(AnonMapStatus(base) == S_OK, "terminal child test page");
  auto* edge = (u8*)base;
  u8* tail = edge + 4092;
  tail[0] = 0xa7;
  check(StreamWrite(term, "okay", 4) == 4, "terminal child ready");
  u32 begin = StreamTicks();
  while (!(StreamReady(term) & STREAM_READABLE) && StreamTicks() - begin < 100)
    Yield(1);
  check(StreamRead(term, tail, 8) == S_INVALID && tail[0] == 0xa7 &&
        StreamRead(term, bytes, 4) == 4 && streq((const char*)bytes, "data") &&
        StreamRead(term, bytes, 1) == S_AGAIN,
        "bad terminal read preserves input and invalid feed enqueues nothing");
  check(AnonUnmap(edge) == S_OK, "release terminal child test page");
  return 0;
}
static int anon_fault_parent(void) {
  int control = TerminalCreate(80, 25);
  int child = start_program("/boot/fstest.snx", "anon-fault-oom");
  check(control > 0 && child > 0 && TerminalForeground(control, child) > 0,
        "attach anonymous OOM child");
  RunResult result;
  check(RunWaitFor(child, &result, 600) == S_OK &&
        result.completion.reason == PROCESS_FAULTED && result.completion.vector == 14,
        "anonymous user OOM produces a typed page fault");
  TerminalInfo state;
  check(TerminalGetInfo(control, &state) == S_OK && !state.foreground &&
        state.mode == (TERM_LINE | TERM_ECHO), "OOM fault restores terminal foreground and mode");
  check(Close(child) == S_OK && Close(control) == S_OK, "OOM probe releases handles");
  Log("ANON TERMINAL FAULT PASS\n");
  return 0;
}
static int blocking_read(int term, void* out, u32 length, u32 timeout) {
  return call(TERM_READ, term, out, length, TERM_READ_BLOCK, timeout);
}
static TerminalRequest wait_request;
static volatile int wait_entered, wait_done, wait_status;
static int read_worker(void*) {
  wait_entered = 1;
  wait_status = api(SYS_TERMINAL, (u32)&wait_request, sizeof(wait_request), 0);
  wait_done = 1;
  return S_OK;
}
static void start_worker(int (*entry)(void*), void* argument) {
  check(sino::thread::create(entry, argument, ThreadCreateFlags::Detached) > 0,
        "start terminal waiter");
  u32 start = StreamTicks();
  while (!wait_entered && StreamTicks() - start < 30) Yield(1);
  Yield(4);
  check(wait_entered && !wait_done, "terminal waiter stays blocked");
}
static void start_wait_worker(TerminalRequest request) {
  wait_request = request;
  start_worker(read_worker, nullptr);
}
static void start_read_worker(int term, void* out) {
  start_wait_worker({TERM_READ, (u32)term, (u32)out, 4, TERM_READ_BLOCK, 0});
}
static int read_worker_result(void) {
  u32 start = StreamTicks();
  while (!wait_done && StreamTicks() - start < 100) Yield(1);
  check(wait_done, "terminal waiter finishes");
  return wait_status;
}
struct WriteArgs {
  int terminal;
  const void* source;
};
static int write_worker(void* argument) {
  const auto& request = *static_cast<const WriteArgs*>(argument);
  wait_entered = 1;
  wait_status = StreamWriteBlocking(request.terminal, request.source, 2, 0);
  wait_done = 1;
  return S_OK;
}
static void writing_child(int term, const char* mode) {
  const bool timeout = streq(mode, "timeout");
  const bool unmap = streq(mode, "unmap");
  memset(bytes, 'a', sizeof(bytes));
  for (u32 remaining = timeout ? 1023 : 1024; remaining;) {
    const u32 chunk = remaining < sizeof(bytes) ? remaining : sizeof(bytes);
    check(TerminalWrite(term, bytes, chunk) == (int)chunk, "fill terminal output");
    remaining -= chunk;
  }

  WriteArgs invalidated{term, nullptr};
  if (unmap) {
    const u32 address = AnonMap(4096);
    check(AnonMapStatus(address) == S_OK, "map writer source");
    auto* source = (char*)address;
    source[0] = 'x';
    source[1] = 'x';
    invalidated.source = source;
    start_worker(write_worker, &invalidated);
    check(AnonUnmap(source) == S_OK, "unmap source while writer is parked");
  }
  check(TerminalMode(term, TERM_ECHO) == S_OK, "publish output-full checkpoint");
  u32 written;
  if (streq(mode, "raw")) {
    check(StreamWriteAllFor(term, "ABCD", 4, 200, &written) == S_OK && written == 4,
          "write all resumes from the accepted suffix");
  } else if (unmap) {
    check(StreamWriteBlocking(term, "OK", 2, 100) == 2 && read_worker_result() == S_INVALID,
          "invalidated writer hands available space to the next writer");
  } else if (timeout) {
    const u32 begin = StreamTicks();
    check(StreamWriteAllFor(term, "AB", 2, 10, &written) == S_TIMEOUT && written == 1 &&
          StreamTicks() - begin >= 10 && StreamWriteFor(term, "B", 1, 0) == S_TIMEOUT,
          "terminal write timeout retains partial progress and zero makes one attempt");
  } else {
    const int expected = streq(mode, "foreground") ? S_DENIED : S_CLOSED;
    check(StreamWriteBlocking(term, "x", 1, 0) == expected,
          "terminal state change resolves a parked writer");
  }
}
static void drain_written_output(int control, const char* suffix) {
  const u32 prefix = 1024 - strlen(suffix);
  for (u32 offset = 0; offset < 1024; offset += sizeof(bytes)) {
    check(TerminalDrain(control, bytes, sizeof(bytes)) == (int)sizeof(bytes), "drain remaining output");
    for (u32 i = 0; i < sizeof(bytes); ++i) {
      const u32 position = offset + i;
      check(bytes[i] == (position < prefix ? 'a' : suffix[position - prefix]),
            "terminal output preserves byte order");
    }
  }
  check(TerminalDrain(control, bytes, 1) == S_AGAIN, "no duplicate output");
}
static void writing_parent(const char* mode) {
  char arguments[32];
  copy(arguments, "w:");
  copy(arguments + 2, mode);
  int control = TerminalCreate(80, 25);
  const int child = start(arguments);
  check(control > 0 && child > 0 && TerminalForeground(control, child) > 0,
        "attach blocking terminal writer");
  TerminalInfo info;
  const u32 begin = StreamTicks();
  do {
    check(TerminalGetInfo(control, &info) == S_OK, "read writer checkpoint");
    if (info.mode == TERM_ECHO) {
      break;
    }
    Yield(1);
  } while (StreamTicks() - begin < 100);
  check(info.mode == TERM_ECHO, "writer filled terminal output");
  Yield(4);

  int next = 0;
  const char* suffix = nullptr;
  if (streq(mode, "raw") || streq(mode, "unmap")) {
    RunResult pending;
    check(GetRunResult(child, &pending) == S_AGAIN && TerminalDrain(control, bytes, 2) == 2 &&
          bytes[0] == 'a' && bytes[1] == 'a', "first drain releases two output bytes");
    if (streq(mode, "raw")) {
      Yield(4);
      check(GetRunResult(child, &pending) == S_AGAIN && TerminalDrain(control, bytes, 2) == 2 &&
            bytes[0] == 'a' && bytes[1] == 'a', "second drain releases remaining output space");
      suffix = "ABCD";
    } else {
      suffix = "OK";
    }
  } else if (streq(mode, "hangup")) {
    check(TerminalFeed(control, TERM_HANGUP, nullptr, 0, 0) == S_OK, "hang up blocked writer");
  } else if (streq(mode, "controller")) {
    check(Close(control) == S_OK, "close controller while writer waits");
    control = 0;
  } else if (streq(mode, "foreground")) {
    next = start_program("/boot/spin.snx", "spin");
    check(next > 0 && TerminalForeground(control, next) > 0, "revoke writer foreground");
  } else {
    suffix = "A";
  }
  done(child, PROCESS_EXITED);
  if (suffix) {
    drain_written_output(control, suffix);
  }
  if (next) {
    check(CancelProcess(next) == S_OK, "cancel replacement after writer probe");
    done(next, PROCESS_CANCELED);
  }
  check(!control || Close(control) == S_OK, "release writer controller");
}
static void blocking_child(int term, const char* mode) {
  if (streq(mode, "snapshot") || streq(mode, "unmap") || streq(mode, "local-close")) {
    if (streq(mode, "unmap")) {
      const u32 address = AnonMap(4096);
      check(address >= USER_DYNAMIC_BASE && address < USER_DYNAMIC_END, "map read output");
      start_read_worker(term, (void*)address);
      check(AnonUnmap((void*)address) == S_OK, "unmap read output while blocked");
    } else {
      start_read_worker(term, bytes);
    }
    if (streq(mode, "snapshot")) {
      wait_request.buffer = USER_BASE;
      wait_request.op = TERM_WRITE;
    } else if (streq(mode, "local-close")) {
      int duplicate = api(SYS_DUP, term, 1, 0);
      check(duplicate > 0 && Close(term) == S_OK, "close exact client handle");
      int replacement = handle_fixture::duplicateIntoClosedSlot(duplicate, 1, (u32)term);
      check(replacement > 0 && replacement != term &&
            read_worker_result() == S_BADHANDLE && Close(replacement) == S_OK,
            "reused client slot does not redirect waiter");
      term = duplicate;
    }
    check(TerminalWrite(term, "wait", 4) == 4, "worker checkpoint");
    if (streq(mode, "snapshot"))
      check(read_worker_result() == 4 && streq((const char*)bytes, "data"), "retry uses captured request");
    if (streq(mode, "unmap"))
      check(read_worker_result() == S_INVALID && TerminalRead(term, bytes, 4) == 4 &&
            streq((const char*)bytes, "data"), "invalid retry output leaves input queued");
    return;
  }

  if (streq(mode, "canonical") || streq(mode, "eof"))
    check(TerminalMode(term, TERM_LINE) == S_OK, "canonical mode without echo");
  check(TerminalRead(term, bytes, 1) == S_AGAIN, "legacy read returns immediately when empty");
  if (streq(mode, "raw")) {
    start_read_worker(term, bytes);
  } else if (streq(mode, "eof") || streq(mode, "hangup") ||
             streq(mode, "controller") || streq(mode, "foreground")) {
    start_wait_worker({TERM_WAIT_INPUT, (u32)term, 0, 0, 0, 0});
  }
  check(TerminalWrite(term, "wait", 4) == 4, "reader checkpoint");
  if (streq(mode, "raw") || streq(mode, "sdk-read")) {
    int status;
    if (streq(mode, "raw")) {
      TerminalEvent event;
      check(TerminalWaitInput(term, 100) == S_OK &&
            TerminalNextEvent(term, &event) == S_OK && event.type == TERM_RESIZE &&
            !wait_done && TerminalRead(term, bytes, 1) == S_AGAIN,
            "pure event wakes input waiter while byte read remains pending");
      check(TerminalWrite(term, "size", 4) == 4, "event checkpoint");
      status = read_worker_result();
    } else {
      status = StreamReadFor(term, bytes, 4, 200);
    }
    check(status == 3 && bytes[0] == 'a' &&
          bytes[1] == 0 && bytes[2] == 'b', "feed wakes reader with raw binary input");
  } else if (streq(mode, "canonical")) {
    check(blocking_read(term, bytes, 4, 0) == 3 &&
          streq((const char*)bytes, "xy\n"), "newline wakes reader with canonical line");
  } else if (streq(mode, "eof")) {
    check(blocking_read(term, bytes, 4, 0) == 4 && streq((const char*)bytes, "tail") &&
          blocking_read(term, bytes, 4, 0) == 0, "EOF commits pending input and then reads zero");
    check(read_worker_result() == S_OK, "EOF also wakes combined input wait");
  } else if (streq(mode, "hangup") || streq(mode, "controller")) {
    check(blocking_read(term, bytes, 1, 0) == S_CLOSED, "session closure wakes blocked reader");
    if (streq(mode, "hangup")) {
      TerminalEvent event;
      check(read_worker_result() == S_OK && TerminalNextEvent(term, &event) == S_OK &&
            event.type == TERM_HANGUP && TerminalWaitInput(term, 1) == S_CLOSED,
            "hangup wakes input wait and preserves the queued event");
    } else {
      check(read_worker_result() == S_CLOSED, "controller close wakes input wait");
    }
  } else if (streq(mode, "foreground")) {
    check(blocking_read(term, bytes, 1, 0) == S_DENIED, "foreground switch revokes blocked read");
    check(read_worker_result() == S_DENIED, "foreground switch also revokes input wait");
  } else if (streq(mode, "timeout")) {
    bytes[0] = 0xa7;
    u32 start = StreamTicks();
    check(blocking_read(term, bytes, 1, 10) == S_TIMEOUT && StreamTicks() - start >= 10 &&
          bytes[0] == 0xa7 && TerminalRead(term, bytes, 1) == S_AGAIN, "timeout preserves output and session");
    start = StreamTicks();
    check(TerminalWaitInput(term, 10) == S_TIMEOUT && StreamTicks() - start >= 10,
          "empty input wait respects its deadline");
  } else if (streq(mode, "wire")) {
    check(blocking_read(term, 0, 0, 0) == 0 &&
          blocking_read(term, bytes, 1025, 1) == S_INVALID &&
          blocking_read(term, bytes, 1, STREAM_WAIT_MAX + 1) == S_INVALID &&
          blocking_read(term, (void*)USER_BASE, 1, 1) == S_INVALID &&
          call(TERM_READ, term, bytes, 1, 0, 1) == S_INVALID &&
          call(TERM_READ, term, bytes, 1, 2, 0) == S_INVALID &&
          call(TERM_WRITE, term, bytes, 1, TERM_READ_BLOCK, 0) == S_INVALID,
          "only flagged reads accept timeout with valid handle and buffer");
    check(TerminalWaitInput(term, STREAM_WAIT_MAX + 1) == S_INVALID &&
          call(TERM_WAIT_INPUT, term, bytes, 0, 0, 1) == S_INVALID &&
          call(TERM_WAIT_INPUT, term, 0, 1, 0, 1) == S_INVALID &&
          call(TERM_WAIT_INPUT, term, 0, 0, 1, 1) == S_INVALID,
          "input wait accepts a timeout and no reserved fields");
  } else {
    check(0, "unknown terminal blocking case");
  }
}
static int blocking_parent(const char* args) {
  const char* mode = args + 2;
  int control = TerminalCreate(80, 25), child = start(args);
  check(control > 0 && child > 0 && TerminalForeground(control, child) > 0, "attach blocking terminal child");
  int result;
  u32 begin = StreamTicks();
  while ((result = TerminalDrain(control, bytes, 4)) == S_AGAIN && StreamTicks() - begin < 100) Yield(1);
  check(result == 4 && streq((const char*)bytes, "wait"), "terminal child reached read checkpoint");
  Yield(4);

  int next = 0;
  if (streq(mode, "raw") || streq(mode, "sdk-read")) {
    if (streq(mode, "raw")) {
      check(TerminalFeed(control, TERM_RESIZE, 0, 0, (100u << 16) | 30) == S_OK,
            "publish event without readable bytes");
      begin = StreamTicks();
      while ((result = TerminalDrain(control, bytes, 4)) == S_AGAIN &&
             StreamTicks() - begin < 100) Yield(1);
      check(result == 4 && streq((const char*)bytes, "size"), "combined wait woke on resize");
    }
    check(TerminalFeed(control, TERM_TEXT, "a\0b", 3, 0) == S_OK, "feed raw bytes");
  } else if (streq(mode, "canonical") || streq(mode, "eof")) {
    int eof = streq(mode, "eof");
    check(TerminalFeed(control, TERM_TEXT, eof ? "tail" : "xy", eof ? 4 : 2, 0) == S_OK,
          "feed partial canonical line");
    Yield(4);
    RunResult pending;
    check(GetRunResult(child, &pending) == S_AGAIN, "partial line does not finish a read");
    check((eof ? TerminalFeed(control, TERM_EOF, 0, 0, 0) :
                 TerminalFeed(control, TERM_TEXT, "\n", 1, 0)) == S_OK, "commit canonical input");
  } else if (streq(mode, "hangup")) {
    check(TerminalFeed(control, TERM_HANGUP, 0, 0, 0) == S_OK, "hang up session");
  } else if (streq(mode, "controller")) {
    int duplicate = api(SYS_DUP, control, 1, 0);
    check(duplicate > 0 && Close(control) == S_OK, "close one controller reference");
    Yield(4);
    RunResult pending;
    check(GetRunResult(child, &pending) == S_AGAIN && Close(duplicate) == S_OK, "last controller reference hangs up");
    control = 0;
  } else if (streq(mode, "foreground")) {
    next = start("spin");
    check(next > 0 && TerminalForeground(control, next) > 0, "switch foreground while read waits");
  } else if (streq(mode, "snapshot") || streq(mode, "unmap")) {
    check(TerminalFeed(control, TERM_TEXT, "data", 4, 0) == S_OK, "feed pending worker read");
  } else if (streq(mode, "wire")) {
    check(blocking_read(control, bytes, 1, 1) == S_BADHANDLE, "controller cannot read as a client");
    check(TerminalWaitInput(control, 1) == S_BADHANDLE, "controller cannot wait as a client");
  }
  done(child, PROCESS_EXITED);
  if (next) {
    check(CancelProcess(next) == S_OK, "cancel replacement foreground");
    done(next, PROCESS_CANCELED);
  }
  check(!control || Close(control) == S_OK, "release controller");
  if (streq(mode, "raw") || streq(mode, "unmap") || streq(mode, "hangup") ||
      streq(mode, "controller") || streq(mode, "foreground") || streq(mode, "timeout")) {
    writing_parent(mode);
  }
  Log("TERMINAL BLOCK PASS\n");
  return 0;
}
int main(void){
 if(Arguments()[0]=='b' && Arguments()[1]==':') return blocking_parent(Arguments());
 if(streq(Arguments(),"uaccess-parent")) return uaccess_terminal_parent();
 if(streq(Arguments(),"anon-fault-parent")) return anon_fault_parent();
 if(!streq(Arguments(),"suite")){
  CliInfo cli;check(QueryCliInfo(&cli)==0,"cli");int t;
  u32 begin=StreamTicks();while((t=call(2,0,0,0,0,0))==S_NOTFOUND && StreamTicks()-begin<100)Yield(1);
  check(t>0,"explicit terminal attachment");
  check(call(6,t,0,0,0,0)==0,"raw mode");
  check(call(1,0,0,0,80,25)==S_DENIED&&call(3,t,0,0,0,0)==S_BADHANDLE&&call(6,t,0,0,4,0)==S_INVALID,"client cannot acquire control or invalid mode");
  const char* mode=cli.bytes+cli.offsets[0];
  if(mode[0]=='b' && mode[1]==':'){blocking_child(t,mode+2);return 0;}
  if(mode[0]=='w' && mode[1]==':'){writing_child(t,mode+2);return 0;}
  if(streq(mode,"uaccess-child"))return uaccess_terminal_child(t);
  if(streq(mode,"flood")){
   for(u32 i=0;i<256;i++)bytes[i]=(u8)i;
   for(int i=0;i<4;i++)check(call(8,t,bytes,256,0,0)==256,"bounded output fill");
   check(call(8,t,bytes,1,0,0)==S_AGAIN &&
         StreamWriteFor(t, bytes, 1, 3) == S_TIMEOUT, "output backpressure and bounded wait");return 0;
  }
  check((StreamReady(t)&STREAM_WRITABLE)&&StreamWriteFor(t,"ready",5,10)==5,"shared stream terminal output");
  if(streq(mode,"hangup")||streq(mode,"lost-controller")){
   int status;begin=StreamTicks();while((status=call(7,t,bytes,1,0,0))==S_AGAIN&&StreamTicks()-begin<100)Yield(1);
   check(status==S_CLOSED,"hangup differs from EOF");
   status=call(10,t,info,16,0,0);
   check(streq(mode,"hangup")?(status==0&&info[0]==4):status==S_CLOSED,"typed hangup or controller loss");return 0;
  }
  if(streq(mode,"fault")){__asm__ volatile("ud2");}
  if(streq(mode,"spin")){for(;;)Yield(1);}
  if(streq(mode,"background")){for(int i=0;i<20;i++)Yield(1);check(call(7,t,bytes,1,0,0)==S_DENIED,"background cannot steal input");return 0;}
  int n=StreamReadFor(t,bytes,16,100);
  check(n==3&&bytes[0]=='a'&&!bytes[1]&&bytes[2]=='b',"raw binary input");
  check(call(6,t,0,0,3,0)==0,"restore line echo");
  check(call(8,t,"line",4,0,0)==4,"line checkpoint");
  begin=StreamTicks();while((n=call(7,t,bytes,16,0,0))==S_AGAIN&&StreamTicks()-begin<100)Yield(1);
  check(n==3&&bytes[0]=='x'&&bytes[1]=='z'&&bytes[2]=='\n',"canonical commit and backspace");
  begin=StreamTicks();while((n=call(10,t,info,16,0,0))==S_AGAIN&&StreamTicks()-begin<100)Yield(1);
  check(n==0&&info[0]==6&&info[2]==100&&info[3]==30,"typed resize");
  begin=StreamTicks();while((n=call(7,t,bytes,1,0,0))==S_AGAIN&&StreamTicks()-begin<100)Yield(1);
  check(n==0,"EOF distinct from hangup");return 0;
 }
 int a=call(1,0,0,0,80,25),b=call(1,0,0,0,40,20);
 check(a>0&&b>0,"session creation");check(call(1,0,0,0,80,25)==S_LIMIT,"bounded session quota");
 check(call(99,0,0,0,0,0)==S_INVALID&&call(4,a,0,0,99,0)==S_INVALID,"unknown operation and event rejected before state checks");
 check(call(5,a,info,32,0,0)==0&&info[0]==32&&info[2]==80&&info[3]==25&&info[4]==3,"default mode size");
 check(call(5,a,(void*)USER_BASE,32,0,0)==S_INVALID&&call(5,a,info,31,0,0)==S_INVALID&&call(4,a,0,0,6,0)==S_INVALID,"query memory and resize validation");
 check(call(7,a,bytes,1,0,0)==S_BADHANDLE,"controller cannot impersonate program");
 check(call(4,a,bytes,257,1,0)==S_INVALID,"input bound");
 int p = start("echo");
 check(p > 0, "launch foreground target");
 int wait_only = api(SYS_DUP, p, PROCESS_WAIT, 0);
 int cancel_only = api(SYS_DUP, p, PROCESS_CANCEL, 0);
 check(wait_only > 0 && cancel_only > 0 &&
       TerminalForeground(a, wait_only) == S_BADHANDLE &&
       TerminalForeground(a, cancel_only) == S_BADHANDLE,
       "foreground attach requires both completion rights");
 check(Close(wait_only) == S_OK && Close(cancel_only) == S_OK &&
       TerminalForeground(a, p) > 0, "owned foreground attach");
 u32 begin=StreamTicks();int n;while((n=call(9,a,bytes,5,0,0))==S_AGAIN&&StreamTicks()-begin<100)Yield(1);
 check(n==5,"raw checkpoint");check(call(9,b,bytes,5,0,0)==S_AGAIN,"session output isolation");
 check(call(4,a,"a\0b",3,1,0)==0,"raw input feed");
 begin=StreamTicks();while((n=call(9,a,bytes,4,0,0))==S_AGAIN&&StreamTicks()-begin<100)Yield(1);check(n==4,"line checkpoint");
 check(call(4,a,"xy\bz\n",5,1,0)==0&&call(4,a,0,0,6,(100u<<16)|30)==0&&call(4,a,0,0,3,0)==0,"canonical resize EOF input");
 RunResult previous;
 check(RunWaitFor(p, &previous, 600) == S_OK &&
       previous.completion.reason == PROCESS_EXITED && !previous.completion.code,
       "foreground target completes");
 check(call(5,a,info,32,0,0)==0&&info[4]==3&&!info[5],"exit restores mode and foreground");
 call(9,a,bytes,256,0,0);
 int replacement = start("fault");
 check(replacement > 0 && TerminalForeground(a, p) == S_BADHANDLE &&
       CancelProcess(p) == S_OK, "completed handle cannot target a replacement");
 check(TerminalForeground(a, replacement) > 0 && Close(p) == S_OK,
       "live replacement can take foreground");
 done(replacement, PROCESS_FAULTED);
 check(call(5,a,info,32,0,0)==0&&info[4]==3&&!info[5],"fault restores mode");call(9,a,bytes,256,0,0);
 p=start("spin");check(p>0&&call(3,a,0,0,p,0)>0,"cancel attach");
 begin=StreamTicks();while((n=call(9,a,bytes,5,0,0))==S_AGAIN&&StreamTicks()-begin<100)Yield(1);check(n==5,"raw special key checkpoint");
 for(int i=0;i<8;i++)check(call(4,a,0,0,2,(u32)i)==0,"bounded key events");
 check(call(4,a,0,0,2,9)==S_AGAIN&&call(4,a,0,0,6,(120u<<16)|40)==S_AGAIN&&call(5,a,info,32,0,0)==0&&info[2]==100,"event backpressure preserves size");
 check(call(4,a,0,0,5,0)==0,"session Ctrl C authority");done(p,PROCESS_CANCELED);
 check(call(5,a,info,32,0,0)==0&&!info[5]&&info[4]==3,"cancel resets session");
 int old=start("background"),next=start("spin");check(old>0&&next>0&&call(3,a,0,0,old,0)>0,"background attach");
 begin=StreamTicks();while((n=call(9,a,bytes,5,0,0))==S_AGAIN&&StreamTicks()-begin<100)Yield(1);check(n==5,"background ready");
 check(call(3,a,0,0,next,0)>0,"foreground switch");done(old,PROCESS_EXITED);
 check(call(4,a,0,0,5,0)==0,"cancel only current task");done(next,PROCESS_CANCELED);
 call(9,a,bytes,256,0,0);
 p=start("flood");check(p>0&&call(3,a,0,0,p,0)>0,"output pressure attach");done(p,PROCESS_EXITED);
 for(int j=0;j<4;j++){check(call(9,a,bytes,256,0,0)==256,"output drain");for(u32 i=0;i<256;i++)check(bytes[i]==(u8)i,"output byte integrity");}
 p=start("hangup");check(p>0&&call(3,a,0,0,p,0)>0,"hangup attach");
 begin=StreamTicks();while((n=call(9,a,bytes,5,0,0))==S_AGAIN&&StreamTicks()-begin<100)Yield(1);check(n==5,"hangup ready");
 check(call(4,a,0,0,4,0)==0,"controller hangup");done(p,PROCESS_EXITED);
 check(call(3,a,0,0,0,0)==S_BADHANDLE&&Close(a)==0,"closed session cleanup");
 p=start("lost-controller");check(p>0&&call(3,b,0,0,p,0)>0,"controller loss attach");
 begin=StreamTicks();while((n=call(9,b,bytes,5,0,0))==S_AGAIN&&StreamTicks()-begin<100)Yield(1);check(n==5&&Close(b)==0,"last controller close");done(p,PROCESS_EXITED);
 Log("TERMINAL SUITE PASS\n");return 0;
}
