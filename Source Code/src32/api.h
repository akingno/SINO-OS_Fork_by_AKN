#ifndef SINO_API_H
#define SINO_API_H
/* Native ABI 1. Values are shared with separately linked ring-3 programs. */
#include "types.h"
#define MAX_FILE 65536u
enum {
  S_OK = 0,
  S_INVALID = -1,
  S_NOMEM = -2,
  S_DENIED = -3,
  S_NOTFOUND = -4,
  S_BUSY = -5,
  S_AGAIN = -6,
  S_IO = -7,
  S_LIMIT = -8,
  S_BADHANDLE = -9,
  S_EXISTS = -10,
  S_CHANGED = -11,
  S_CLOSED = -12,
  S_TIMEOUT = -13
};
enum {
  SYS_EXIT = 0x100,
  SYS_YIELD,
  SYS_LOG,
  SYS_TICKS,
  SYS_WINDOW,
  SYS_SCENE,
  SYS_EVENT,
  SYS_CLOSE,
  SYS_READ,
  SYS_WRITE,
  SYS_MKDIR,
  SYS_REMOVE,
  SYS_LIST,
  SYS_SPAWN,
  SYS_KILL,
  SYS_PROCESSES,
  SYS_SYNC,
  SYS_THREAD,
  SYS_THREAD_EXIT,
  SYS_EVENT_CREATE,
  SYS_EVENT_SET,
  SYS_EVENT_WAIT,
  SYS_LEASE,
  SYS_SOCKET,
  SYS_BIND,
  SYS_LISTEN,
  SYS_CONNECT,
  SYS_ACCEPT,
  SYS_SEND,
  SYS_RECV,
  SYS_NET_READY,
  SYS_NET_INFO,
  SYS_PING,
  SYS_SENDTO,
  SYS_RECVFROM,
  SYS_MOVE,
  SYS_CLIP_GET,
  SYS_CLIP_SET,
  SYS_WINDOW_POLICY,
  SYS_DUP,
  SYS_UI_LANGUAGE,
  SYS_WINDOW_TITLE,
  SYS_TIME,
  SYS_OPEN_PATH,
  SYS_FILE_CLIP,
  SYS_DROP_ZONES,
  SYS_WINDOW_SIZE,
  SYS_SYSTEM_INFO,
  SYS_START_INFO,
  SYS_SPAWN_TRACKED,
  SYS_PROCESS_RESULT,
  SYS_PROCESS_CANCEL,
  SYS_RUN_START,
  SYS_CLI_INFO,
  SYS_STREAM_READ,
  SYS_STREAM_WRITE,
  SYS_RUN_RESULT,
  SYS_RUN_READ,
  SYS_RUN_LIMITS,
  SYS_DIR_GRANT,
  SYS_START_DIRECTORY,
  SYS_OPEN_AT,
  SYS_FILE_READ_AT,
  SYS_FILE_WRITE_AT,
  SYS_FILE_STAT,
  SYS_DIR_READ,
  SYS_REMOVE_AT,
  SYS_RENAME_AT,
  SYS_FILE_STAGE,
  SYS_FILE_PUBLISH,
  SYS_FILE_COMMIT,
  SYS_PIPE_CREATE,
  SYS_STREAM_READY,
  SYS_PIPE_LIMITS,
  SYS_TERMINAL,
  SYS_CALENDAR,
  SYS_WORKSPACE,
  SYS_DIRECTORY_COMMIT,
  SYS_ANON_MAP,
  SYS_ANON_UNMAP,
  SYS_CALL_END /* Append future calls before this sentinel. */
};

enum { PROCESS_COMPAT = 0, PROCESS_STRICT = 1 };
enum { FEATURE_SYSTEM_INFO = 1, FEATURE_STRICT_POLICY = 2, FEATURE_START_INFO = 4 };
#define FEATURE_COMPLETION 8u
#define FEATURE_BOUNDED_RUN 16u
#define FEATURE_FILE_OBJECTS 32u
#define FEATURE_PIPES 64u
#define FEATURE_TERMINALS 128u
#define FEATURE_CALENDAR 256u
#define FEATURE_WORKSPACE 512u
#define FEATURE_LINE_EDIT 1024u
#define FEATURE_ANON_MEMORY 2048u
#define FEATURE_ANON_LAZY 4096u
#define FEATURE_SNX_V2 8192u
#define SYSTEM_FEATURES (FEATURE_SYSTEM_INFO | FEATURE_STRICT_POLICY | FEATURE_START_INFO | FEATURE_COMPLETION | FEATURE_BOUNDED_RUN | FEATURE_FILE_OBJECTS | FEATURE_PIPES | FEATURE_TERMINALS | FEATURE_CALENDAR | FEATURE_WORKSPACE | FEATURE_LINE_EDIT | FEATURE_ANON_MEMORY | FEATURE_ANON_LAZY | FEATURE_SNX_V2)
enum { TERM_CREATE=1, TERM_CURRENT, TERM_FOREGROUND, TERM_FEED, TERM_INFO,
       TERM_MODE, TERM_READ, TERM_WRITE, TERM_DRAIN, TERM_EVENT, TERM_READY };
enum { TERM_TEXT=1, TERM_KEY, TERM_EOF, TERM_HANGUP, TERM_CANCEL, TERM_RESIZE };
/* TERM_READ uses value=TERM_READ_BLOCK and extra=timeout for blocking reads. */
#define TERM_READ_BLOCK 1u
enum { TERM_LINE=1, TERM_ECHO=2 };
enum { TERM_EDIT=17, TERM_EDIT_VIEW, TERM_EDIT_STATUS, TERM_EDIT_ACK };
enum { TERM_DESKTOP_RUN=21 };
enum { DESKTOP_RUN_APP=1, DESKTOP_RUN_STRICT, DESKTOP_INSTALL, DESKTOP_NETTEST,
       DESKTOP_KILL };
typedef struct { char path[128], argument[256]; } DesktopRun;
typedef struct { u32 bytes,cursor,action,serial; char text[400]; } TerminalEdit;
typedef struct { u32 op, handle, buffer, bytes, value, extra; } TerminalRequest;
typedef struct { u32 size, version, width, height, mode, foreground, closed, reserved; } TerminalInfo;
typedef struct { u32 type, value, width, height; } TerminalEvent;
#define RUN_COUNT 2u
#define RUN_ARGC 16u
#define RUN_ARGS 1024u
#define RUN_BYTES 4096u
#define RUN_IO_MAX 65536u
#define RUN_TIMEOUT_MAX 60000u
typedef struct {
  u32 size, version, flags, path, argc, argv_bytes, argv, offsets;
  u32 input_bytes, input, timeout_ticks, reserved;
} RunSpec;
/* v1 stays 48 bytes. v2 adds one explicitly attenuated directory grant. */
typedef struct {
  RunSpec run;
  u32 directory, directory_rights, reserved[2];
} RunSpecV2;
/* v3 preserves v1/v2 and binds only explicitly named pipe endpoints to stdio. */
typedef struct {
  RunSpecV2 context;
  u32 in, out, err, reserved;
} RunSpecV3;
#define PIPE_COUNT 8u
#define PIPE_BYTES 4096u
#define STREAM_WAIT_MAX 60000u
/* Pipe stream calls: set in EDX alongside the byte count; ESI is the timeout. */
#define STREAM_BLOCK_FLAG 0x80000000u
enum { STREAM_READABLE = 1, STREAM_WRITABLE = 2, STREAM_EOF = 4,
       STREAM_HANGUP = 8, STREAM_LIMIT = 16 };
typedef struct { u32 read, write; } PipePair;
typedef struct {
  u32 size, version, pipes, capacity, io_bytes, timeout_ticks, ready_flags, reserved;
} PipeLimits;

enum {
  FS_READ = 1, FS_WRITE = 2, FS_LOOKUP = 4, FS_ENUM = 8,
  FS_CREATE = 16, FS_REMOVE = 32, FS_COMMIT = 64, FS_DELEGATE = 128
};
#define FS_RIGHTS 255u
enum { OPEN_CREATE = 1, OPEN_EXCLUSIVE = 2, OPEN_TRUNCATE = 4,
       OPEN_DIRECTORY = 8 };
enum { FILE_REGULAR = 1, FILE_DIRECTORY = 2, FILE_STAGING = 3 };
typedef struct { u32 path, rights, flags; } OpenRequest;
typedef struct { u32 offset, buffer, bytes; } FileIo;
typedef struct {
  u32 size, version, kind, length, generation, revision, readonly, reserved;
} FileStat;
typedef struct { char name[32]; u32 length, dir, revision; } DirEntry;
/* version=0,cursor=0 begins enumeration. A changed version requires restart. */
typedef struct { u32 version, cursor, buffer, capacity, count, done; } DirPage;
typedef struct { u32 source_dir, source_path, target_dir, target_path; } RenameSpec;
/* expected_revision=0 requires an absent target. Staging starts empty. */
typedef struct { u32 path, expected_revision; } StageSpec;
typedef struct {
  u32 size, version, published, revision, durable, generation;
  i32 status;
  u32 reserved;
} CommitInfo;
typedef struct {
  u32 size, version, argc, in, out, err, argv_bytes, reserved;
  u32 offsets[RUN_ARGC];
  char bytes[RUN_ARGS];
} CliInfo;
typedef struct { u32 role, offset, buffer, capacity; } RunRead;
typedef struct {
  u32 size, version, runs, argc, argv_bytes, input_bytes, out_bytes, err_bytes;
  u32 io_bytes, timeout_ticks;
} RunLimits;
#define PROCESS_COMPLETIONS 16
/* RESULT calls select blocking with EDX=RESULT_BLOCK_FLAG and ESI=timeout. */
#define RESULT_BLOCK_FLAG 1u
enum { PROCESS_EXITED = 1, PROCESS_FAULTED, PROCESS_CANCELED, PROCESS_KILLED,
       PROCESS_OUTPUT_LIMIT, PROCESS_TIMEOUT };
enum { PROCESS_WAIT = 1, PROCESS_CANCEL = 2 };
typedef struct {
  u32 size, version, pid, reason;
  i32 code;
  u32 vector, reserved[2];
} ProcessCompletion;
typedef struct {
  ProcessCompletion completion;
  u32 input_bytes, out_bytes, err_bytes, incomplete;
} RunResult;
typedef struct {
  u32 size, version, features, ticks_hz;
  u32 max_processes, max_threads, max_handles, max_objects;
  u32 max_file, image_bytes, stack_bytes, reserved;
} SystemInfo;
typedef struct {
  u32 size, version, policy, caps, features, reserved;
} StartInfo;
enum { CAP_HOME = 1, CAP_NETWORK = 2, CAP_MANAGE = 4 };
enum {
  UI_FILL = 1,
  UI_TEXT,
  UI_BUTTON,
  UI_LINE,
  UI_PANEL,
  UI_GRID,
  UI_SELECTION,
  UI_ELLIPSE,
  UI_CELL_TEXT /* Font 2, seven-pixel cells, non-ASCII occupies three cells. */
};
enum {
  WINDOW_CLOSE_REQUEST = 1,
  WINDOW_APP_KEYS = 2,
  WINDOW_NO_IME = 4,
  WINDOW_DESKTOP_COMMANDS = 8
};
enum {
  MSG_CLICK = 1,
  MSG_KEY,
  MSG_CLOSE,
  MSG_MOUSE_MOVE,
  MSG_MOUSE_UP,
  MSG_TEXT,
  MSG_CONTEXT,
  MSG_COMMAND,
  MSG_FILES_CHANGED,
  MSG_SCROLL, MSG_INPUT_REJECTED
};
#define UI_COMMANDS 64
#define UI_CANVAS_COMMANDS 256
typedef struct {
  i32 kind, x, y, w, h;
  u32 color;
  char text[80];
} UiCommand;
typedef struct {
  u32 count;
  UiCommand commands[UI_COMMANDS];
} UiScene;
/* Variable-length SYS_SCENE keeps original 64-command ABI 1 clients valid. */
typedef struct {
  u32 count;
  UiCommand commands[UI_CANVAS_COMMANDS];
} UiCanvas;
typedef struct {
  u32 type, window;
  i32 x, y, key;
} UiEvent;
typedef struct {
  u32 hour, minute, second;
} TimeOfDay;
typedef struct { u32 size, version, year, month, day, hour, minute, second; } CalendarTime;
typedef struct {
  u32 pid, state, threads, pages;
  char name[32];
  i32 exit_code;
} ProcessInfo;
typedef struct {
  char name[32];
  u32 size, dir;
} FileInfo;
/* Generic file transfer rectangles. Paths are copied/validated by the kernel;
   no Node, Window or framebuffer address crosses this interface. */
#define DROP_ZONES 32
#define DROP_SOURCE 1u
#define DROP_TARGET 2u
typedef struct {
  i32 x, y, w, h;
  u32 flags;
  char path[128];
} UiDropZone;
typedef struct {
  char path[128];
  u32 cut;
} FileClipboard;
typedef struct {
  u32 width, height;
} WindowSize;
#define USER_BASE 0x40000000u
#define USER_DYNAMIC_BASE 0x40400000u
#define USER_DYNAMIC_END 0x80000000u
#define ANON_MAX_REGIONS 32u
#define ANON_MAX_BYTES 0x1000000u
#define SNX_V1_IMAGE_BYTES 0x30000u
#define SNX_V2_IMAGE_BYTES 0x1000000u
#define USER_V1_ARGUMENTS (USER_BASE + SNX_V1_IMAGE_BYTES)
#define USER_V1_STACK(t) (USER_BASE + 0x40000u + (u32)(t) * 0x8000u)
#define USER_V1_END (USER_BASE + 0x60000u)
#define USER_V2_ARGUMENTS USER_DYNAMIC_END
#define USER_V2_STACK(t) (USER_V2_ARGUMENTS + 0x10000u + (u32)(t) * 0x8000u)
#define USER_V2_END (USER_V2_ARGUMENTS + 0x30000u)
#if defined(SINO_SNX_ABI) && SINO_SNX_ABI == 2
#define USER_BYTES SNX_V2_IMAGE_BYTES
#define USER_ARGUMENTS USER_V2_ARGUMENTS
#define USER_STACK(t) USER_V2_STACK(t)
#else
#define USER_BYTES SNX_V1_IMAGE_BYTES
#define USER_ARGUMENTS USER_V1_ARGUMENTS
#define USER_STACK(t) USER_V1_STACK(t)
#endif
#define NATIVE_MAGIC 0x31584e53u /* SNX1; deliberately not PE/Win32 */
typedef struct {
  u32 magic, abi, image_bytes, entry, data_offset, memory_bytes, reserved[2];
} NativeHeader;

static inline int native_header_valid(const NativeHeader* h, u32 file_bytes) {
  return h->magic == NATIVE_MAGIC && (h->abi == 1 || h->abi == 2) &&
         h->image_bytes == file_bytes && h->entry >= sizeof(NativeHeader) &&
         h->entry < h->data_offset && h->data_offset <= h->image_bytes &&
         !(h->data_offset & 4095) && h->memory_bytes >= h->image_bytes &&
         h->memory_bytes <= (h->abi == 2 ? SNX_V2_IMAGE_BYTES : SNX_V1_IMAGE_BYTES) &&
         !h->reserved[0] && !h->reserved[1];
}
#endif
