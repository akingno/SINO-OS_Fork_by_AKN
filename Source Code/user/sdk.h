#ifndef SINO_SDK_H
#define SINO_SDK_H
#include "../src32/api.h"
void *memcpy(void *, const void *, u32);
void *memset(void *, int, u32);
u32 strlen(const char *);
int streq(const char *, const char *);
void number(u32, char *);
static inline int api(u32 n, u32 a, u32 b, u32 c) {
  __asm__ volatile("int $0x80"
                   : "+a"(n)
                   : "b"(a), "c"(b), "d"(c)
                   : "memory", "cc");
  return (int)n;
}
static inline void Exit(int n) {
  api(SYS_EXIT, (u32)n, 0, 0);
  for (;;) {
  }
}
static inline void Yield(u32 ticks) {
  api(SYS_YIELD, ticks, 0, 0);
}
/* Query output is unchanged on failure; older kernels return S_INVALID. */
static inline int QuerySystemInfo(SystemInfo *out) {
  return api(SYS_SYSTEM_INFO, (u32)out, sizeof(*out), 0);
}
static inline int QueryStartInfo(StartInfo *out) {
  return api(SYS_START_INFO, (u32)out, sizeof(*out), 0);
}
/* The child is strict; this call returns an owned observation/cancel handle. */
static inline int SpawnTracked(const char *path, const char *args) {
  return api(SYS_SPAWN_TRACKED, (u32)path, (u32)args, 0);
}
static inline int GetProcessResult(int handle, ProcessCompletion *out) {
  return api(SYS_PROCESS_RESULT, (u32)handle, (u32)out, sizeof(*out));
}
static inline int CancelProcess(int handle) {
  return api(SYS_PROCESS_CANCEL, (u32)handle, 0, 0);
}
static inline int StartRun(const RunSpec *spec) {
  return api(SYS_RUN_START, (u32)spec, sizeof(*spec), 0);
}
static inline int StartRunWithDirectory(const RunSpecV2 *spec) {
  return api(SYS_RUN_START, (u32)spec, sizeof(*spec), 0);
}
static inline int StartRunWithStreams(const RunSpecV3 *spec) {
  return api(SYS_RUN_START, (u32)spec, sizeof(*spec), 0);
}
static inline int PipeCreate(u32 capacity, PipePair *out) {
  return api(SYS_PIPE_CREATE, capacity, (u32)out, sizeof(*out));
}
static inline int StreamReady(int handle) {
  return api(SYS_STREAM_READY, (u32)handle, 0, 0);
}
static inline int QueryPipeLimits(PipeLimits *out) {
  return api(SYS_PIPE_LIMITS, (u32)out, sizeof(*out), 0);
}
static inline int TerminalCall(u32 op,int handle,void* buffer,u32 bytes,u32 value,u32 extra) {
  TerminalRequest r={op,(u32)handle,(u32)buffer,bytes,value,extra};
  return api(SYS_TERMINAL,(u32)&r,sizeof(r),0);
}
static inline int TerminalCreate(u32 w,u32 h){return TerminalCall(TERM_CREATE,0,0,0,w,h);}
static inline int TerminalCurrent(void){return TerminalCall(TERM_CURRENT,0,0,0,0,0);}
static inline int TerminalForeground(int t,int task){return TerminalCall(TERM_FOREGROUND,t,0,0,(u32)task,0);}
static inline int TerminalGetInfo(int t,TerminalInfo* out){return TerminalCall(TERM_INFO,t,out,sizeof(*out),0,0);}
static inline int TerminalMode(int t,u32 flags){return TerminalCall(TERM_MODE,t,0,0,flags,0);}
static inline int TerminalRead(int t,void* b,u32 n){return TerminalCall(TERM_READ,t,b,n,0,0);}
static inline int TerminalWrite(int t,const void* b,u32 n){return TerminalCall(TERM_WRITE,t,(void*)b,n,0,0);}
static inline int TerminalDrain(int t,void* b,u32 n){return TerminalCall(TERM_DRAIN,t,b,n,0,0);}
static inline int TerminalNextEvent(int t,TerminalEvent* e){return TerminalCall(TERM_EVENT,t,e,sizeof(*e),0,0);}
static inline int TerminalFeed(int t,u32 type,const void* b,u32 n,u32 value){return TerminalCall(TERM_FEED,t,(void*)b,n,type,value);}
/* Compatibility launchers acquire grants; strict children use only handed-in
   directory handles. Close/Dup retain their existing ownership rules. */
static inline int GrantDirectory(const char *path, u32 rights) {
  return api(SYS_DIR_GRANT, (u32)path, rights, 0);
}
static inline int StartDirectory(void) {
  return api(SYS_START_DIRECTORY, 0, 0, 0);
}
static inline int OpenAt(int directory, const char *path, u32 rights, u32 flags) {
  OpenRequest request = {(u32)path, rights, flags};
  return api(SYS_OPEN_AT, (u32)directory, (u32)&request, sizeof(request));
}
static inline int FileReadAt(int file, u32 offset, void *buffer, u32 bytes) {
  FileIo io = {offset, (u32)buffer, bytes};
  return api(SYS_FILE_READ_AT, (u32)file, (u32)&io, sizeof(io));
}
static inline int FileWriteAt(int file, u32 offset, const void *data, u32 bytes) {
  FileIo io = {offset, (u32)data, bytes};
  return api(SYS_FILE_WRITE_AT, (u32)file, (u32)&io, sizeof(io));
}
static inline int FileGetStat(int file, FileStat *out) {
  return api(SYS_FILE_STAT, (u32)file, (u32)out, sizeof(*out));
}
static inline int DirectoryRead(int directory, DirPage *page) {
  return api(SYS_DIR_READ, (u32)directory, (u32)page, sizeof(*page));
}
static inline int RemoveAt(int directory, const char *path, int is_directory) {
  return api(SYS_REMOVE_AT, (u32)directory, (u32)path, (u32)is_directory);
}
static inline int RenameAt(int source_dir, const char *source, int target_dir, const char *target) {
  RenameSpec spec = {(u32)source_dir, (u32)source, (u32)target_dir, (u32)target};
  return api(SYS_RENAME_AT, (u32)&spec, sizeof(spec), 0);
}
static inline int FileStage(int directory, const char *path, u32 expected_revision) {
  StageSpec spec = {(u32)path, expected_revision};
  return api(SYS_FILE_STAGE, (u32)directory, (u32)&spec, sizeof(spec));
}
/* Once the stage handle is valid, result is filled even on publish/commit
   failure. Always inspect published/durable as well as the return code. */
static inline int FilePublish(int stage, CommitInfo *result) {
  return api(SYS_FILE_PUBLISH, (u32)stage, (u32)result, sizeof(*result));
}
static inline int FileCommit(int stage, CommitInfo *result) {
  return api(SYS_FILE_COMMIT, (u32)stage, (u32)result, sizeof(*result));
}
static inline int QueryCliInfo(CliInfo *out) {
  return api(SYS_CLI_INFO, (u32)out, sizeof(*out), 0);
}
static inline int StreamRead(int handle, void *out, u32 capacity) {
  return api(SYS_STREAM_READ, (u32)handle, (u32)out, capacity);
}
static inline int StreamWrite(int handle, const void *data, u32 bytes) {
  return api(SYS_STREAM_WRITE, (u32)handle, (u32)data, bytes);
}
static inline int GetRunResult(int handle, RunResult *out) {
  return api(SYS_RUN_RESULT, (u32)handle, (u32)out, sizeof(*out));
}
static inline int ReadRunOutput(int handle, const RunRead *request) {
  return api(SYS_RUN_READ, (u32)handle, (u32)request, sizeof(*request));
}
static inline int QueryRunLimits(RunLimits *out) {
  return api(SYS_RUN_LIMITS, (u32)out, sizeof(*out), 0);
}
/* The caller owns info/argv storage; pointers remain valid while info does. */
static inline int CliArguments(CliInfo *info, char **argv, u32 capacity) {
  if (!info || !argv) return S_INVALID;
  int result = QueryCliInfo(info);
  if (result < 0) return result;
  if (info->size != sizeof(*info) || info->version != 1 || info->argc > RUN_ARGC ||
      info->argv_bytes > RUN_ARGS) return S_INVALID;
  if (capacity <= info->argc) return S_LIMIT;
  for (u32 i = 0; i < info->argc; i++) {
    if (info->offsets[i] >= info->argv_bytes) return S_INVALID;
    argv[i] = info->bytes + info->offsets[i];
  }
  argv[info->argc] = 0;
  return (int)info->argc;
}
static inline int CliWriteAll(int handle, const void *data, u32 bytes) {
  if (!bytes) return StreamWrite(handle, data, 0);
  const u8 *p = data; u32 written = 0;
  while (written < bytes) {
    int count = StreamWrite(handle, p + written, bytes - written);
    if (count <= 0) return count ? count : S_IO;
    written += (u32)count;
  }
  return (int)written;
}
static inline int Read(const char *p, void *b, u32 n) {
  return api(SYS_READ, (u32)p, (u32)b, n);
}
static inline int Write(const char *p, const void *b, u32 n) {
  return api(SYS_WRITE, (u32)p, (u32)b, n);
}
static inline int Mkdir(const char *p) {
  return api(SYS_MKDIR, (u32)p, 0, 0);
}
static inline int Remove(const char *p, int dir) {
  return api(SYS_REMOVE, (u32)p, (u32)dir, 0);
}
static inline int List(const char *p, FileInfo *b, u32 n) {
  return api(SYS_LIST, (u32)p, (u32)b, n);
}
static inline int Spawn(const char *p) {
  return api(SYS_SPAWN, (u32)p, 0, 0);
}
static inline int SpawnArgs(const char *p, const char *args) {
  return api(SYS_SPAWN, (u32)p, (u32)args, 0);
}
static inline const char *Executable(void) {
  return (const char *)(USER_ARGUMENTS + 256);
}
static inline const char *Arguments(void) {
  return (const char *)USER_ARGUMENTS;
}
/* A failed snapshot leaves the previous committed bank intact. Retry transient
   device completion failures, never acknowledge until SYS_SYNC confirms. */
static inline int Sync(void) {
  int r = S_IO;
  for (int i = 0; i < 3; i++) {
    r = api(SYS_SYNC, 0, 0, 0);
    if (r != S_IO)
      return r;
    if (i < 2)
      Yield(1);
  }
  return r;
}
static inline int Window(const char *p, int w, int h) {
  return api(SYS_WINDOW, (u32)p, (u32)w, (u32)h);
}
static inline int Calendar(CalendarTime *value) {
  return api(SYS_CALENDAR, (u32)value, sizeof(*value), 0);
}
static inline int Time(TimeOfDay *value) {
  return api(SYS_TIME, (u32)value, sizeof(*value), 0);
}
static inline int UiLanguage(void) {
  return api(SYS_UI_LANGUAGE, 0, 0, 0);
}
static inline int WindowTitle(int h, const char *title) {
  return api(SYS_WINDOW_TITLE, (u32)h, (u32)title, 0);
}
static inline int Scene(int h, const UiScene *s) {
  return api(SYS_SCENE, (u32)h, (u32)s, 0);
}
static inline int Event(UiEvent *e) {
  return api(SYS_EVENT, (u32)e, 0, 0);
}
static inline int Close(int h) {
  return api(SYS_CLOSE, (u32)h, 0, 0);
}
static inline int OpenPath(const char *p) { return api(SYS_OPEN_PATH,(u32)p,0,0); }
static inline int FileClip(int set,FileClipboard *p) { return api(SYS_FILE_CLIP,(u32)set,(u32)p,0); }
static inline int DropZones(int h,const UiDropZone *z,u32 n) { return api(SYS_DROP_ZONES,(u32)h,(u32)z,n); }
static inline int GetWindowSize(int h,WindowSize *s) { return api(SYS_WINDOW_SIZE,(u32)h,(u32)s,sizeof(*s)); }
void copy(char *, const char *);
int text_encode(u32, char *);
u32 text_previous(const char *, u32);
void Log(const char *);
void ui_begin(UiScene *);
void ui_canvas_begin(UiCanvas *);
int ui_canvas_fill(UiCanvas *, int, int, int, int, u32);
int ui_canvas_text(UiCanvas *, int, int, int, const char *, u32);
int ui_canvas_line(UiCanvas *, int, int, int, int, u32);
int ui_canvas_ellipse(UiCanvas *, int, int, int, int, u32);
void ui_text(UiScene *, int, int, const char *);
void ui_mono(UiScene *, int, int, const char *, u32);
void ui_mono_clip(UiScene *, int, int, int, const char *, u32);
int ui_dialog(int, UiScene *, const char *, char *, int);
void ui_button(UiScene *, int, int, int, const char *);
void ui_fill(UiScene *, int, int, int, int, u32);
void ui_panel(UiScene *, int, int, int, int, u32, int);
int ui_hit(UiEvent *, int, int, int, int);
#endif
