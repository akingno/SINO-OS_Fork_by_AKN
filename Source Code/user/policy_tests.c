/* Fixed wire contracts deliberately independent of the new SDK wrappers. */
#include "sdk.h"
_Static_assert(SYS_EXIT == 0x100, "ABI 1 syscall number");
_Static_assert(SYS_YIELD == 0x101, "ABI 1 syscall number");
_Static_assert(SYS_LOG == 0x102, "ABI 1 syscall number");
_Static_assert(SYS_TICKS == 0x103, "ABI 1 syscall number");
_Static_assert(SYS_WINDOW == 0x104, "ABI 1 syscall number");
_Static_assert(SYS_SCENE == 0x105, "ABI 1 syscall number");
_Static_assert(SYS_EVENT == 0x106, "ABI 1 syscall number");
_Static_assert(SYS_CLOSE == 0x107, "ABI 1 syscall number");
_Static_assert(SYS_READ == 0x108, "ABI 1 syscall number");
_Static_assert(SYS_WRITE == 0x109, "ABI 1 syscall number");
_Static_assert(SYS_MKDIR == 0x10a, "ABI 1 syscall number");
_Static_assert(SYS_REMOVE == 0x10b, "ABI 1 syscall number");
_Static_assert(SYS_LIST == 0x10c, "ABI 1 syscall number");
_Static_assert(SYS_SPAWN == 0x10d, "ABI 1 syscall number");
_Static_assert(SYS_KILL == 0x10e, "ABI 1 syscall number");
_Static_assert(SYS_PROCESSES == 0x10f, "ABI 1 syscall number");
_Static_assert(SYS_SYNC == 0x110, "ABI 1 syscall number");
_Static_assert(SYS_THREAD == 0x111, "ABI 1 syscall number");
_Static_assert(SYS_THREAD_EXIT == 0x112, "ABI 1 syscall number");
_Static_assert(SYS_EVENT_CREATE == 0x113, "ABI 1 syscall number");
_Static_assert(SYS_EVENT_SET == 0x114, "ABI 1 syscall number");
_Static_assert(SYS_EVENT_WAIT == 0x115, "ABI 1 syscall number");
_Static_assert(SYS_LEASE == 0x116, "ABI 1 syscall number");
_Static_assert(SYS_SOCKET == 0x117, "ABI 1 syscall number");
_Static_assert(SYS_BIND == 0x118, "ABI 1 syscall number");
_Static_assert(SYS_LISTEN == 0x119, "ABI 1 syscall number");
_Static_assert(SYS_CONNECT == 0x11a, "ABI 1 syscall number");
_Static_assert(SYS_ACCEPT == 0x11b, "ABI 1 syscall number");
_Static_assert(SYS_SEND == 0x11c, "ABI 1 syscall number");
_Static_assert(SYS_RECV == 0x11d, "ABI 1 syscall number");
_Static_assert(SYS_NET_READY == 0x11e, "ABI 1 syscall number");
_Static_assert(SYS_NET_INFO == 0x11f, "ABI 1 syscall number");
_Static_assert(SYS_PING == 0x120, "ABI 1 syscall number");
_Static_assert(SYS_SENDTO == 0x121, "ABI 1 syscall number");
_Static_assert(SYS_RECVFROM == 0x122, "ABI 1 syscall number");
_Static_assert(SYS_MOVE == 0x123, "ABI 1 syscall number");
_Static_assert(SYS_CLIP_GET == 0x124, "ABI 1 syscall number");
_Static_assert(SYS_CLIP_SET == 0x125, "ABI 1 syscall number");
_Static_assert(SYS_WINDOW_POLICY == 0x126, "ABI 1 syscall number");
_Static_assert(SYS_DUP == 0x127, "ABI 1 syscall number");
_Static_assert(SYS_UI_LANGUAGE == 0x128, "ABI 1 syscall number");
_Static_assert(SYS_WINDOW_TITLE == 0x129, "ABI 1 syscall number");
_Static_assert(SYS_TIME == 0x12a, "ABI 1 syscall number");
_Static_assert(SYS_OPEN_PATH == 0x12b, "ABI 1 syscall number");
_Static_assert(SYS_FILE_CLIP == 0x12c, "ABI 1 syscall number");
_Static_assert(SYS_DROP_ZONES == 0x12d, "ABI 1 syscall number");
_Static_assert(SYS_WINDOW_SIZE == 0x12e, "ABI 1 syscall number");
#define QUERY_SYSTEM 0x12fu
#define QUERY_START 0x130u
static u32 buffer[16];
static volatile int done;
static void require(int ok, const char *reason) {
  if (!ok) { Log("POLICY SELFTEST FAIL: "); Log(reason); Log("\n"); Exit(1); }
}
static void worker(void) {
  done = 1;
  api(SYS_THREAD_EXIT, 0, 0, 0);
  for (;;) {}
}
static void query_tests(int strict) {
  SystemInfo system;
  StartInfo launch;
  require(sizeof(system)==48 && sizeof(launch)==24 &&
          QuerySystemInfo(&system)==0 && QueryStartInfo(&launch)==0 &&
          (system.features&7)==7 && launch.policy==(u32)strict,"SDK query wrappers");
  for (u32 i=0;i<16;i++) buffer[i]=0xccccccccu;
  require(api(QUERY_SYSTEM,(u32)buffer,47,0)==S_INVALID && buffer[0]==0xccccccccu,
          "short query preserves output");
  require(api(QUERY_SYSTEM,(u32)buffer,64,1)==S_INVALID && buffer[0]==0xccccccccu,
          "unknown query flags");
  require(api(QUERY_SYSTEM,USER_BASE,48,0)==S_INVALID &&
          api(QUERY_SYSTEM,USER_STACK(0)-4,48,0)==S_INVALID &&
          api(QUERY_START,0xffffffffu,24,0)==S_INVALID,"query output rights");
  require(api(QUERY_SYSTEM,(u32)buffer,64,0)==0,"system query available");
  require(buffer[0]==48 && buffer[1]==1 && (buffer[2]&7)==7 && buffer[3]==100 &&
          buffer[4]==8 && buffer[5]==4 && buffer[6]==24 && buffer[7]==64 &&
          buffer[8]==65536 && buffer[9]==196608 && buffer[10]==16384 &&
          buffer[11]==0 && buffer[12]==0xccccccccu,"system wire layout and tail");
  for(u32 i=0;i<16;i++) buffer[i]=0xccccccccu;
  require(api(QUERY_START,(u32)buffer,23,0)==S_INVALID && buffer[0]==0xccccccccu,
          "short start query");
  require(api(QUERY_START,(u32)buffer,64,0)==0 && buffer[0]==24 && buffer[1]==1 &&
          buffer[2]==(u32)strict && buffer[3]==(strict?0:CAP_HOME) &&
          (buffer[4]&7)==7 && buffer[5]==0 && buffer[6]==0xccccccccu,"start policy immutable");
  _Static_assert(SYS_CALENDAR==0x14b && sizeof(CalendarTime)==32,"calendar ABI");
  CalendarTime date; int calendar=Calendar(&date);
  for(u32 i=0;i<16;i++) buffer[i]=0xccccccccu;
  require(api(SYS_CALENDAR,(u32)buffer,31,0)==S_INVALID && buffer[0]==0xccccccccu,"calendar short output");
  require(api(SYS_CALENDAR,(u32)buffer,64,1)==S_INVALID && buffer[0]==0xccccccccu,"calendar reserved flags");
  require(api(SYS_CALENDAR,USER_BASE,32,0)==S_INVALID,"calendar output rights");
  require((system.features&FEATURE_CALENDAR) && calendar==0 && date.size==32 && date.version==1 && date.year>=2000 && date.month>=1 && date.month<=12 && date.day>=1 && date.day<=31 && date.hour<24 && date.minute<60 && date.second<60,"calendar strict read-only query");
  require(api(SYS_CALENDAR,(u32)buffer,64,0)==0 && buffer[8]==0xccccccccu,"calendar query preserves tail");
  require(WorkspaceDirectory()==S_DENIED,"workspace denied to untrusted compat and strict programs");
  for(u32 i=0;i<16;i++)buffer[i]=0xccccccccu;
  require(api(SYS_DIRECTORY_COMMIT,0,(u32)buffer,sizeof(CommitInfo))==S_BADHANDLE && buffer[0]==0xccccccccu,"directory commit requires owned authorization and preserves output");
  require(api(0xffffffffu,0,0,0)==S_INVALID,"unknown syscall");
}
int main(void) {
  int strict=streq(Arguments(),"strict");
  query_tests(strict);
  if (!strict) {
    FileInfo items[1];
    require(List("/home/user",items,1)>=0,"legacy file reads remain available");
    Log("POLICY LEGACY PASS\n"); return 0;
  }
  const u32 denied[]={SYS_WINDOW,SYS_SCENE,SYS_EVENT,SYS_READ,SYS_WRITE,
    SYS_MKDIR,SYS_REMOVE,SYS_LIST,SYS_SPAWN,SYS_KILL,SYS_PROCESSES,SYS_SYNC,
    SYS_LEASE,SYS_SOCKET,SYS_BIND,SYS_LISTEN,SYS_CONNECT,SYS_ACCEPT,SYS_SEND,
    SYS_RECV,SYS_NET_READY,SYS_NET_INFO,SYS_PING,SYS_SENDTO,SYS_RECVFROM,
    SYS_MOVE,SYS_CLIP_GET,SYS_CLIP_SET,SYS_WINDOW_POLICY,SYS_WINDOW_TITLE,
    SYS_OPEN_PATH,SYS_FILE_CLIP,SYS_DROP_ZONES,SYS_WINDOW_SIZE,SYS_SPAWN_TRACKED,SYS_RUN_START};
  for(u32 i=0;i<sizeof(denied)/sizeof(denied[0]);i++)
    require(api(denied[i],0,0,0)==S_DENIED,"old entry is denied before effects");
  require(Read("/home/user/stage2-persist.bin",buffer,sizeof(buffer))==S_DENIED &&
          Read("/boot/readme.txt",buffer,sizeof(buffer))==S_DENIED,"no ambient file read");
  require(Write("/home/user/policy-escape","bad",3)==S_DENIED &&
          OpenPath("/Applications/Files.app")==S_DENIED,"write and deputy denied");
  require(SpawnArgs("/boot/poltest.snx","legacy")==S_DENIED &&
          SpawnArgs("/BOOT/../boot/POLTEST.SNX","legacy")==S_DENIED &&
          SpawnArgs("/home/user/poltest.snx","legacy")==S_DENIED,"spawn cannot downgrade");
  int event=api(SYS_EVENT_CREATE,0,0,0), duplicate;
  require(event>0 && api(SYS_EVENT_WAIT,event,0,0)==S_AGAIN,"local event allowed");
  duplicate=api(SYS_DUP,event,1,0);
  require(duplicate>0 && api(SYS_EVENT_SET,duplicate,1,0)==S_BADHANDLE &&
          api(SYS_EVENT_SET,event,1,0)==0 && Close(event)==0 &&
          api(SYS_EVENT_WAIT,duplicate,0,0)==0 && Close(duplicate)==0,"local handle rights");
  require(api(SYS_THREAD,(u32)worker,0,0)>0,"local thread allowed");
  u32 start=(u32)api(SYS_TICKS,0,0,0);
  while(!done && (u32)api(SYS_TICKS,0,0,0)-start<100) Yield(1);
  require(done,"local thread scheduled");
  TimeOfDay time;
  require(Time(&time)==0 && UiLanguage()>=0,"public time and language");
  query_tests(1);
  Log("POLICY STRICT PASS\n"); return 0;
}
