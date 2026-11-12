/* Fixed wire tests for Stage C, buildable before the implementation. */
#include "sdk.h"
#define RUN_START 0x134u
#define CLI_INFO 0x135u
#define STREAM_READ 0x136u
#define STREAM_WRITE 0x137u
#define RUN_RESULT 0x138u
#define RUN_READ 0x139u
#define RUN_LIMITS 0x13au
typedef struct {u32 size,version,flags,path,argc,argv_bytes,argv,offsets,input_bytes,input,timeout_ticks,reserved;} Spec;
typedef struct {u32 size,version,argc,in,out,err,argv_bytes,reserved,offsets[16];char bytes[1024];} Cli;
typedef struct {ProcessCompletion completion;u32 input_bytes,out_bytes,err_bytes,incomplete;} Result;
static Cli cli;
static Result result;
static u8 data[4097],copy_data[4097];
static u32 offsets[16],limits[10];
static char args[1024];
static void require(int ok,const char *why){if(!ok){Log("CLI SELFTEST FAIL: ");Log(why);Log("\n");Exit(1);}}
static int transfer(u32 call,int handle,const void *buffer,u32 bytes){return api(call,handle,(u32)buffer,bytes);}
static int start(const char *mode,u32 bytes){
  copy(args,mode);offsets[0]=0;
  Spec spec={48,1,0,(u32)"/boot/clitest.snx",1,strlen(args)+1,(u32)args,(u32)offsets,bytes,(u32)data,0,0};
  return api(RUN_START,(u32)&spec,sizeof(spec),0);
}
static void wait_for(int handle,u32 reason,int code){
  u32 begin=(u32)api(SYS_TICKS,0,0,0);int status;
  while((status=api(RUN_RESULT,handle,(u32)&result,sizeof(result)))==S_AGAIN &&
        (u32)api(SYS_TICKS,0,0,0)-begin<500)Yield(1);
  require(status==0 && result.completion.size==32 && result.completion.reason==reason &&
          result.completion.code==code,"final run result");
}
static int capture(int handle,int role,u32 offset,void *out,u32 capacity){
  u32 request[4]={(u32)role,offset,(u32)out,capacity};
  return api(RUN_READ,handle,(u32)request,sizeof(request));
}
int main(void){
  if(!streq(Arguments(),"unit")){
    require(api(CLI_INFO,(u32)&cli,sizeof(cli),0)==0 && cli.size==sizeof(cli) &&
            cli.version==1 && cli.argc<=16 && !cli.reserved,"structured startup");
    require(cli.in>2 && cli.out>2 && cli.err>2 && cli.in!=cli.out,"opaque standard handles");
    require(api(RUN_START,0,0,0)==S_DENIED && Spawn("/boot/probe.snx")==S_DENIED &&
            Read("/home/user/contracts-persist.bin",data,1)==S_DENIED,"no ambient authority");
    const char *mode=cli.argc ? cli.bytes+cli.offsets[0] : "echo";
    CliInfo info;char *argv[17];
    require(CliArguments(&info,argv,17)==(int)cli.argc && !argv[cli.argc] &&
            info.in==cli.in && info.out==cli.out,"SDK argv and handles");
    if(streq(mode,"spin")){for(;;)__asm__ volatile("nop");}
    if(streq(mode,"wait-echo"))Yield(20);
    if(streq(mode,"exit"))return -17;
    if(streq(mode,"args")){
      transfer(STREAM_WRITE,cli.out,&cli.argc,4);
      for(u32 i=0;i<cli.argc;i++){u32 n=strlen(cli.bytes+cli.offsets[i]);transfer(STREAM_WRITE,cli.out,&n,4);transfer(STREAM_WRITE,cli.out,cli.bytes+cli.offsets[i],n);}
      return 0;
    }
    if(streq(mode,"overflow")||streq(mode,"overflow-err")){
      int h=streq(mode,"overflow")?cli.out:cli.err;
      require(transfer(STREAM_WRITE,h,data,4096)==4096,"exact capture bound");
      transfer(STREAM_WRITE,h,data,1);require(0,"overflow must terminate");
    }
    if(streq(mode,"fault")){transfer(STREAM_WRITE,cli.out,"before fault",12);__asm__ volatile("ud2");}
    require(transfer(STREAM_WRITE,cli.out,0,0)==0 && transfer(STREAM_READ,cli.in,0,0)==0,"zero byte operations");
    require(transfer(STREAM_READ,cli.out,data,1)==S_BADHANDLE &&
            transfer(STREAM_WRITE,cli.in,data,1)==S_BADHANDLE,"stream directions");
    require(api(CLI_INFO,USER_BASE,sizeof(cli),0)==S_INVALID &&
            api(CLI_INFO,(u32)&cli,sizeof(cli)-1,0)==S_INVALID,"startup output bounds");
    require(transfer(STREAM_WRITE,cli.out,(void*)0x100000,1)==S_INVALID &&
            transfer(STREAM_READ,cli.in,(void*)USER_BASE,1)==S_INVALID,"stream pointer rights");
    for(;;){int n=StreamRead(cli.in,data,7);require(n>=0,"stdin read");if(!n)break;require(CliWriteAll(cli.out,data,(u32)n)==n,"stdout bytes");}
    require(transfer(STREAM_READ,cli.in,data,1)==0,"repeat EOF");
    const u8 error[4]={'e',0,'r','\n'};require(transfer(STREAM_WRITE,cli.err,(void*)error,4)==4,"separate stderr");
    return 0;
  }
  for(u32 i=0;i<4097;i++)data[i]=(u8)(i*37+11);
  int handle=start("echo",257);require(handle>0,"bounded run launch available");
  require(api(RUN_LIMITS,(u32)limits,sizeof(limits),0)==0 && limits[0]==40 && limits[2]==2 &&
          limits[3]==16 && limits[4]==1024 && limits[5]==4096,"discover bounded limits");
  limits[0]=0xccccccccu;
  require(api(RUN_LIMITS,(u32)limits,39,0)==S_INVALID && limits[0]==0xccccccccu &&
          api(RUN_LIMITS,(u32)limits,40,1)==S_INVALID && limits[0]==0xccccccccu &&
          api(RUN_LIMITS,USER_BASE,40,0)==S_INVALID,"limit query bounds and flags");
  wait_for(handle,1,0);
  result.out_bytes=0xccccccccu;
  require(api(RUN_RESULT,handle,(u32)&result,sizeof(result)-1)==S_INVALID &&
          result.out_bytes==0xccccccccu && api(RUN_RESULT,handle,USER_BASE,sizeof(result))==S_INVALID,
          "run result bounds and output canary");
  wait_for(handle,1,0);
  require(result.out_bytes==257 && result.err_bytes==4 && !result.incomplete,"capture counts");
  require(capture(handle,1,0,copy_data,sizeof(copy_data))==257,"capture read");
  for(u32 i=0;i<257;i++)require(copy_data[i]==data[i],"binary input/output");
  require(capture(handle,1,257,copy_data,1)==0 && capture(handle,1,258,copy_data,1)==S_INVALID,"capture EOF and offset");
  require(capture(handle,2,0,copy_data,4)==4 && copy_data[1]==0,"stderr binary preserved");
  require(capture(handle,0,0,copy_data,4)==S_INVALID &&
          capture(handle,1,0,(void*)USER_BASE,4)==S_INVALID &&
          capture(handle,1,0,0,0)==0,"capture role and buffer bounds");
  int read_only=api(SYS_DUP,handle,PROCESS_WAIT,0),cancel_only=api(SYS_DUP,handle,PROCESS_CANCEL,0);
  require(read_only>0 && cancel_only>0 && api(RUN_RESULT,cancel_only,(u32)&result,sizeof(result))==S_BADHANDLE,
          "capture observation rights");
  require(Close(handle)==0 && capture(read_only,1,0,copy_data,1)==1 &&
          api(RUN_RESULT,handle,(u32)&result,sizeof(result))==S_BADHANDLE,"capture duplicate retains data");
  Close(read_only);Close(cancel_only);
  handle=start("overflow",0);require(handle>0,"overflow launch");wait_for(handle,5,0);
  require(result.out_bytes==4096 && result.incomplete,"truncated output marked");Close(handle);
  handle=start("fault",0);require(handle>0,"fault launch");wait_for(handle,2,0);
  require(result.completion.vector==6 && result.out_bytes==12 && result.incomplete,"fault output retained");Close(handle);
  int first=start("spin",0),second=start("spin",0);require(first>0 && second>0,"two run quota");
  result.input_bytes=0xccccccccu;
  require(api(RUN_RESULT,first,(u32)&result,sizeof(result))==S_AGAIN &&
          result.input_bytes==0xccccccccu,"pending run result untouched");
  require(start("echo",0)==S_LIMIT,"run quota full");CancelProcess(first);wait_for(first,3,0);
  require(start("echo",0)==S_LIMIT,"completed capture still owns quota");Close(first);
  handle=start("exit",0);require(handle>0,"quota release");wait_for(handle,1,-17);Close(handle);
  CancelProcess(second);wait_for(second,3,0);Close(second);
  Spec invalid={48,1,1,(u32)"/boot/clitest.snx",0,0,0,0,0,0,0,0};
  for(int i=0;i<40;i++)require(api(RUN_START,(u32)&invalid,48,0)==S_INVALID,"invalid start rollback");
  require(api(RUN_START,USER_STACK(0)-4,48,0)==S_INVALID &&
          api(RUN_START,(u32)&invalid,47,0)==S_INVALID &&
          api(RUN_START,(u32)&invalid,48,1)==S_INVALID,"start control range and flags");
  invalid.flags=0;invalid.argc=1;invalid.argv_bytes=5;invalid.argv=(u32)"echo";
  invalid.offsets=0x100000;
  require(api(RUN_START,(u32)&invalid,48,0)==S_INVALID,"argument offset pointer");
  invalid.offsets=(u32)offsets;invalid.input_bytes=1;invalid.input=0x100000;
  require(api(RUN_START,(u32)&invalid,48,0)==S_INVALID,"stdin input pointer");
  invalid.input_bytes=0;invalid.path=0x100000;
  require(api(RUN_START,(u32)&invalid,48,0)==S_INVALID,"program path pointer");
  handle=start("exit",0);require(handle>0,"quota survives failed starts");wait_for(handle,1,-17);Close(handle);
  RunLimits public_limits;require(QueryRunLimits(&public_limits)==0 && public_limits.runs==2,"SDK limits");
  RunResult public_result;RunRead public_read={1,0,(u32)copy_data,4};
  RunSpec public_spec={48,1,0,(u32)"/boot/clitest.snx",1,5,(u32)"echo",(u32)offsets,0,0,0,0};
  handle=StartRun(&public_spec);require(handle>0,"SDK start");wait_for(handle,1,0);
  require(GetRunResult(handle,&public_result)==0 && !public_result.out_bytes &&
          ReadRunOutput(handle,&public_read)==0,"SDK result and capture");Close(handle);
  Log("CLI SUITE PASS\n");return 0;
}
