/* Wire-level tests also build before Stage B is implemented. */
#include "sdk.h"
#define TRACK 0x131u
#define RESULT 0x132u
#define CANCEL 0x133u
typedef struct { u32 size,version,pid,reason; i32 code; u32 vector,r0,r1; } Result;
static Result output;
static int held[24];
static char long_args[257];
static void require(int ok,const char *why) {
  if(!ok){Log("COMPLETION SELFTEST FAIL: ");Log(why);Log("\n");Exit(1);}
}
static int spawn(const char *arg) { return api(TRACK,(u32)"/boot/comptest.snx",(u32)arg,0); }
static int poll(int h,Result *r) { return api(RESULT,h,(u32)r,sizeof(*r)); }
static void wait_result(int h,u32 reason,int code,u32 vector) {
  u32 begin=(u32)api(SYS_TICKS,0,0,0); int status;
  while((status=poll(h,&output))==S_AGAIN &&
        (u32)api(SYS_TICKS,0,0,0)-begin<500) Yield(1);
  require(status==0 && output.size==32 && output.version==1 && output.pid>0 &&
          output.reason==reason && output.code==code && output.vector==vector &&
          !output.r0 && !output.r1,"durable typed result");
  Result again;
  require(poll(h,&again)==0 && again.pid==output.pid && again.code==output.code &&
          again.reason==output.reason && again.vector==output.vector,"repeat result");
}
static void normal(const char *arg,u32 reason,int code,u32 vector) {
  int h=spawn(arg); require(h>0,"tracked launch available");
  wait_result(h,reason,code,vector); require(Close(h)==0,"close completion");
  require(poll(h,&output)==S_BADHANDLE,"stale completion rejected");
}
#ifdef RESULT_BLOCK_FLAG
static int blocking_result(u32 call, int handle, void* out, u32 timeout) {
  __asm__ volatile("int $0x80" : "+a"(call)
                   : "b"(handle), "c"(out), "d"(RESULT_BLOCK_FLAG), "S"(timeout)
                   : "memory", "cc");
  return (int)call;
}
static u32 ticks(void) { return (u32)api(SYS_TICKS, 0, 0, 0); }
typedef struct {
  int handle;
  ProcessCompletion* out;
  volatile int entered, done, status;
} WaitProbe;
static WaitProbe probes[2];
static void completion_worker(WaitProbe* probe) {
  probe->entered = 1;
  probe->status = blocking_result(SYS_PROCESS_RESULT, probe->handle, probe->out, 0);
  probe->done = 1;
  api(SYS_THREAD_EXIT, 0, 0, 0);
}
static void first_waiter(void) { completion_worker(&probes[0]); }
static void second_waiter(void) { completion_worker(&probes[1]); }
static void begin_wait(int index, int handle, ProcessCompletion* out) {
  WaitProbe* probe = &probes[index];
  *probe = (WaitProbe){.handle=handle, .out=out};
  require(api(SYS_THREAD, (u32)(index ? second_waiter : first_waiter), 0, 0) > 0, "start completion waiter");
  u32 start = ticks();
  while (!probe->entered && ticks() - start < 30) Yield(1);
  Yield(4);
  require(probe->entered && !probe->done, "completion call stays pending");
}
static void finish_wait(int index, int status) {
  WaitProbe* probe = &probes[index];
  u32 start = ticks();
  while (!probe->done && ticks() - start < 100) Yield(1);
  require(probe->done && probe->status == status, "waiter receives expected status");
}
static void blocking_run_case(int timed) {
  const char* mode = timed ? "spin" : "wait-echo";
  const char payload[] = "abc";
  u32 offset = 0;
  RunSpec spec = {sizeof(spec), 1, 0, (u32)"/boot/clitest.snx", 1, strlen(mode) + 1,
                  (u32)mode, (u32)&offset, timed ? 0 : sizeof(payload), (u32)payload,
                  (u32)(timed ? 20 : 0), 0};
  int handle = StartRun(&spec);
  require(handle > 0, "start bounded run");
  struct { RunResult result; u32 tail; } reply;
  memset(&reply, 0xcc, sizeof(reply));
  require(api(SYS_RUN_RESULT, handle, (u32)&reply, 0xffffffffu) == S_AGAIN &&
          reply.result.completion.size == 0xccccccccu, "legacy run query remains nonblocking");
  require(blocking_result(SYS_RUN_RESULT, handle, &reply, 200) == 0 &&
          reply.result.completion.size == 32 && reply.result.completion.pid &&
          reply.result.completion.code == 0 && reply.tail == 0xccccccccu, "fixed run result layout");
  if (timed) {
    require(reply.result.completion.reason == PROCESS_TIMEOUT && reply.result.incomplete &&
            !reply.result.out_bytes && !reply.result.err_bytes, "child timeout publishes incomplete result");
  } else {
    char readback[sizeof(payload)];
    RunRead request = {1, 0, (u32)readback, sizeof(readback)};
    require(reply.result.completion.reason == PROCESS_EXITED && !reply.result.incomplete &&
            reply.result.input_bytes == sizeof(payload) && reply.result.out_bytes == sizeof(payload) &&
            reply.result.err_bytes == 4 && ReadRunOutput(handle, &request) == sizeof(payload) &&
            streq(readback, payload), "run exit wakes with final capture");
  }
  require(Close(handle) == 0, "release bounded run result");
}
static void blocking_case(const char* mode) {
  if (streq(mode, "run-exit") || streq(mode, "run-timeout")) {
    blocking_run_case(streq(mode, "run-timeout"));
    return;
  }
  int exiting = streq(mode, "exit");
  int handle = spawn(exiting ? "linger" : "spin");
  require(handle > 0, "start tracked wait target");
  struct { ProcessCompletion result; u32 tail; } reply;
  memset(&reply, 0xcc, sizeof(reply));
  if (exiting) {
    require(api(SYS_PROCESS_RESULT, handle, (u32)&reply, 0xffffffffu) == S_AGAIN &&
            reply.result.size == 0xccccccccu, "legacy process query remains nonblocking");
    require(blocking_result(SYS_PROCESS_RESULT, handle, &reply, 0) == 0 &&
            reply.result.size == 32 && reply.result.version == 1 && reply.result.pid &&
            reply.result.reason == PROCESS_EXITED && reply.result.code == 7 &&
            reply.tail == 0xccccccccu, "exit wakes waiter with typed result");
    ProcessCompletion again;
    require(GetProcessResult(handle, &again) == 0 && again.pid == reply.result.pid &&
            again.reason == PROCESS_EXITED && again.code == 7, "result stays repeatable");
  } else if (streq(mode, "timeout")) {
    u32 start = ticks();
    require(blocking_result(SYS_PROCESS_RESULT, handle, &reply, 5) == S_TIMEOUT &&
            ticks() - start >= 5 && reply.result.size == 0xccccccccu &&
            GetProcessResult(handle, &reply.result) == S_AGAIN, "wait timeout preserves output and child");
  } else if (streq(mode, "multi-cancel")) {
    ProcessCompletion other;
    int observer = api(SYS_DUP, handle, PROCESS_WAIT, 0);
    require(observer > 0, "duplicate completion observer");
    begin_wait(0, handle, &reply.result);
    begin_wait(1, observer, &other);
    require(CancelProcess(handle) == 0, "cancel target with two waiters");
    finish_wait(0, S_OK);
    finish_wait(1, S_OK);
    require(reply.result.reason == PROCESS_CANCELED && other.reason == PROCESS_CANCELED &&
            reply.result.pid && other.pid == reply.result.pid && Close(observer) == 0,
            "both waiters observe the same cancellation");
  } else if (streq(mode, "local-close")) {
    int observer = api(SYS_DUP, handle, PROCESS_WAIT | PROCESS_CANCEL, 0);
    require(observer > 0, "retain alternate observer");
    begin_wait(0, handle, &reply.result);
    require(Close(handle) == 0, "close exact waited handle");
    int replacement = api(SYS_DUP, observer, PROCESS_WAIT, 0);
    require(replacement > 0 && (replacement & 255) == (handle & 255), "reuse closed handle slot");
    finish_wait(0, S_BADHANDLE);
    require(reply.result.size == 0xccccccccu && Close(replacement) == 0, "closed wait leaves output untouched");
    handle = observer;
  } else if (streq(mode, "unmap")) {
    u32 address = (u32)AnonMap(4096);
    require(address >= USER_DYNAMIC_BASE && address < USER_DYNAMIC_END, "map result output");
    begin_wait(0, handle, (ProcessCompletion*)address);
    require(AnonUnmap((void*)address) == 0 && CancelProcess(handle) == 0, "unmap before result publication");
    finish_wait(0, S_INVALID);
  } else if (streq(mode, "wire")) {
    int cancel_only = api(SYS_DUP, handle, PROCESS_CANCEL, 0);
    require(cancel_only > 0 &&
            blocking_result(SYS_PROCESS_RESULT, cancel_only, &reply, 1) == S_BADHANDLE &&
            blocking_result(SYS_PROCESS_RESULT, handle, (void*)USER_BASE, 1) == S_INVALID &&
            blocking_result(SYS_PROCESS_RESULT, handle, &reply, STREAM_WAIT_MAX + 1) == S_INVALID &&
            api(SYS_PROCESS_RESULT, handle, (u32)&reply, 31) == S_INVALID &&
            blocking_result(SYS_RUN_RESULT, handle, &reply.result, 1) == S_INVALID &&
            reply.result.size == 0xccccccccu && Close(cancel_only) == 0, "blocking query validates its arguments");
  } else {
    require(0, "unknown blocking result case");
  }
  if (!exiting) {
    require(CancelProcess(handle) == 0 &&
            blocking_result(SYS_PROCESS_RESULT, handle, &reply, 100) == 0 &&
            reply.result.reason == PROCESS_CANCELED, "reap canceled wait target");
  }
  require(Close(handle) == 0, "release tracked result");
}
#endif
int main(void) {
  const char *arg=Arguments();
#ifdef RESULT_BLOCK_FLAG
  if (arg[0] == 'w' && arg[1] == ':') {
    blocking_case(arg + 2);
    Log("COMPLETION BLOCK PASS\n");
    return 0;
  }
#endif
  if(!streq(arg,"suite") && !streq(arg,"orphan") && !streq(arg,"abandon") &&
     !streq(arg,"observer-fault") && !streq(arg,"external")) {
    StartInfo info;
    require(QueryStartInfo(&info)==0 && info.policy==1 && !info.caps,"child strict policy");
    require(spawn("exit")==S_DENIED && Spawn("/boot/comptest.snx")==S_DENIED,
            "child cannot spawn or downgrade");
    require(Read("/home/user/contracts-persist.bin",&output,sizeof(output))==S_DENIED,
            "child has no ambient read");
    const char *prefix="foreign:";u32 pos=0;
    while(prefix[pos] && prefix[pos]==arg[pos])pos++;
    if(!prefix[pos]) {
      u32 handle=0;for(;arg[pos];pos++)handle=handle*10+(u32)(arg[pos]-'0');
      require(poll((int)handle,&output)==S_BADHANDLE &&
              api(CANCEL,handle,0,0)==S_BADHANDLE,"foreign observer denied");
    }
    if(streq(arg,"negative")) return -14;
    if(streq(arg,"pagefault")) *(volatile u32*)0x100000=1;
    if(streq(arg,"iofault")) __asm__ volatile("outb %%al,%%dx"::"a"(1),"d"(0x21));
    if(streq(arg,"udfault")) __asm__ volatile("ud2");
    if(streq(arg,"spin")) for(;;) Yield(1);
    if(streq(arg,"linger")){Yield(30);Log("COMPLETION ORPHAN CHILD PASS\n");return 7;}
    return 42;
  }
  if(streq(arg,"external")) {
    int target=spawn("spin");require(target>0,"external kill target");
    Log("COMPLETION KILL TARGET READY\n");
    wait_result(target,4,0,0);
    require(CancelProcess(target)==0,"late cancel after external kill");
    wait_result(target,4,0,0);Close(target);
    Log("COMPLETION KILLED RESULT PASS\n");return 0;
  }
  if(!streq(arg,"suite")) {
    int h=spawn("linger");require(h>0,"orphan child launch");
    if(streq(arg,"abandon")) require(Close(h)==0,"close running observer");
    if(streq(arg,"observer-fault")) __asm__ volatile("ud2");
    Log("COMPLETION OBSERVER EXIT PASS\n");return 0;
  }
  normal("exit",1,42,0); normal("negative",1,-14,0);
  normal("pagefault",2,0,14); normal("iofault",2,0,13); normal("udfault",2,0,6);
  int h=spawn("spin");require(h>0,"running child");
  output.size=0xccccccccu;
  require(poll(h,&output)==S_AGAIN && output.size==0xccccccccu,"pending output untouched");
  require(api(RESULT,h,(u32)&output,31)==S_INVALID && output.size==0xccccccccu,
          "short result untouched");
  require(api(RESULT,h,USER_BASE,32)==S_INVALID &&
          api(RESULT,h,USER_STACK(0)-4,32)==S_INVALID,"result pointer rights");
  char foreign[32];copy(foreign,"foreign:");number((u32)h,foreign+8);
  normal(foreign,1,42,0);
  int ro=api(SYS_DUP,h,1,0), cancel_only=api(SYS_DUP,h,2,0);
  require(ro>0 && cancel_only>0 && api(CANCEL,ro,0,0)==S_BADHANDLE &&
          poll(cancel_only,&output)==S_BADHANDLE,"completion rights");
  require(api(CANCEL,h,1,0)==S_INVALID && api(CANCEL,h,0,0)==0 &&
          api(CANCEL,h,0,0)==0,"idempotent targeted cancel");
  wait_result(ro,3,0,0); require(api(CANCEL,h,0,0)==0,"late cancel preserves result");
  wait_result(h,3,0,0);Close(h);Close(ro);Close(cancel_only);
  int e=api(SYS_EVENT_CREATE,0,0,0);
  require(poll(e,&output)==S_BADHANDLE && api(CANCEL,e,0,0)==S_BADHANDLE,"wrong object type");
  Close(e);
  for(int i=0;i<16;i++){held[i]=spawn("exit");require(held[i]>0,"reserve completion record");wait_result(held[i],1,42,0);}
  require(spawn("exit")==S_LIMIT,"completed record limit");
  int old=held[0];Close(old);h=spawn("negative");require(h>0 && h!=old,"slot reuse gets new handle");
  require(poll(old,&output)==S_BADHANDLE,"old cookie cannot observe new task");
  wait_result(held[1],1,42,0);wait_result(h,1,-14,0);Close(h);
  for(int i=1;i<16;i++) Close(held[i]);
  for(int i=0;i<24;i++){held[i]=api(SYS_EVENT_CREATE,0,0,0);require(held[i]>0,"fill handle table");}
  require(spawn("exit")==S_LIMIT,"launch handle reservation rollback");
  for(int i=0;i<24;i++)Close(held[i]);
  require(Write("/home/user/completion-bad.snx","bad",3)==0,"invalid image fixture");
  for(int i=0;i<40;i++){
    require(api(TRACK,(u32)"/home/user/completion-bad.snx",0,0)==S_INVALID,"bad image rollback");
    require(api(TRACK,(u32)"/boot/absent.snx",0,0)==S_NOTFOUND,"missing image rollback");
  }
  require(Remove("/home/user/completion-bad.snx",0)==0,"fixture cleanup");
  for(int i=0;i<256;i++)long_args[i]='x';long_args[256]=0;
  require(api(TRACK,(u32)"/boot/comptest.snx",(u32)long_args,0)==S_INVALID &&
          api(TRACK,(u32)"/boot/comptest.snx",0,1)==S_INVALID,"argument and flags validation");
  for(int i=0;i<7;i++){held[i]=spawn("spin");require(held[i]>0,"fill process slots");}
  require(spawn("exit")==S_LIMIT,"process slot rollback");
  for(int i=0;i<7;i++){require(api(CANCEL,held[i],0,0)==0,"cancel full table child");wait_result(held[i],3,0,0);Close(held[i]);}
  normal("exit",1,42,0);
  h=SpawnTracked("/boot/comptest.snx","negative");require(h>0,"SDK tracked spawn");
  wait_result(h,1,-14,0);ProcessCompletion typed;
  require(GetProcessResult(h,&typed)==0 && typed.reason==PROCESS_EXITED &&
          typed.code==-14 && sizeof(typed)==32 && CancelProcess(h)==0,"SDK result and cancel");
  Close(h);
  Log("COMPLETION SUITE PASS\n");return 0;
}
