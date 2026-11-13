/* Development-only v2 protocol; owns only tasks created by this session. */
#include "run_bridge.h"
#ifdef SINO_SHELL_BRIDGE
#include "task/task.h"
#include "klog.h"
#include "lib/kstring.h"
static Handle owner[HANDLES];
static u32 nonce[2], last_id, lease;
static int active;
static void close_session(void) {
  for (u32 i = 0; i < HANDLES; i++) if (owner[i].generation)
    process_cancel_owned(owner, (owner[i].cookie << 8) | (i + 1));
  handles_close(owner);
  active = 0;
}
void run_bridge_init(void) { memset(owner, 0, sizeof(owner)); active = 0; }
void run_bridge_abort(void) { close_session(); }
void run_bridge_tick(void) {
  if (active && (i32)(ticks - lease) >= 0) {
    close_session();
    pr_debug("RUN BRIDGE lease expired\n");
  }
}
u32 run_bridge_request(const u8* bytes, u32 size, u32 id, u8* out, int* status) {
  *status = S_INVALID;
  if (size < 12) return 0;
  u32 op = *(const u32*)bytes;
  const u32* incoming = (const u32*)(bytes + 4);
  memcpy(out, incoming, 8); /* Echo request identity, including errors. */
  const u8* body = bytes + 12;
  u32 length = size - 12;
  if (op == 1) {
    if (length || id != 1 || !(incoming[0] | incoming[1]) ||
        (incoming[0] == nonce[0] && incoming[1] == nonce[1])) return 8;
    close_session();
    nonce[0] = incoming[0]; nonce[1] = incoming[1];
    last_id = id; active = 1; lease = ticks + RUN_BRIDGE_LEASE;
    RunLimits limits; process_run_limits(&limits); memcpy(out + 8, &limits, sizeof(limits));
    *status = 0; return 8 + sizeof(limits);
  }
  if (!active || incoming[0] != nonce[0] || incoming[1] != nonce[1]) {
    *status = S_DENIED; return 8;
  }
  if (!id || id <= last_id) return 8;
  last_id = id;
  if (op == 2) {
    if (length < 208) return 8;
    const u32* header = (const u32*)body;
    u32 argc = header[1], argv_bytes = header[2], input_bytes = header[3];
    if (argc > RUN_ARGC || argv_bytes > RUN_ARGS || input_bytes > RUN_BYTES ||
        header[0] > RUN_TIMEOUT_MAX || length != 208 + argv_bytes + input_bytes) return 8;
    const char* path = (const char*)body + 16;
    u32 end = 0; while (end < 128 && path[end]) end++;
    if (end == 128) return 8;
    for (u32 i = end; i < 128; i++) if (path[i]) return 8;
    const u32* offsets = (const u32*)(body + 144);
    for (u32 i = argc; i < RUN_ARGC; i++) if (offsets[i]) return 8;
    RunSpec spec = {sizeof(RunSpec), 1, 0, (u32)path, argc, argv_bytes,
        (u32)(body + 208), (u32)offsets, input_bytes,
        (u32)(body + 208 + argv_bytes), header[0], 0};
    int handle = process_run_start(owner, &spec);
    *status = handle < 0 ? handle : 0;
    if (handle < 0) return 8;
    *(u32*)(out + 8) = (u32)handle; lease = ticks + RUN_BRIDGE_LEASE; return 12;
  }
  if (op == 6 && !length) { lease = ticks + RUN_BRIDGE_LEASE; *status = 0; return 8; }
  if ((op == 3 || op == 4 || op == 5) && length == 4) {
    u32 handle = *(const u32*)body;
    lease = ticks + RUN_BRIDGE_LEASE;
    if (op == 3) {
      RunResult result;
      *status = process_run_result(owner, handle, &result);
      if (*status) return 8;
      memcpy(out + 8, &result, sizeof(result)); return 8 + sizeof(result);
    }
    *status = process_cancel_owned(owner, handle);
    if (op == 5 && !*status) *status = handle_close(owner, handle);
    return 8;
  }
  if (op == 7 && length == 16) {
    const u32* read = (const u32*)body;
    if (read[3] > RUN_BYTES) return 8;
    lease = ticks + RUN_BRIDGE_LEASE;
    int count = process_run_read(owner, read[0], read[1], read[2], out + 8, read[3]);
    *status = count; return count < 0 ? 8 : 8 + (u32)count;
  }
  return 8;
}
#else
void run_bridge_init(void) {}
void run_bridge_abort(void) {}
void run_bridge_tick(void) {}
u32 run_bridge_request(const u8* bytes, u32 size, u32 id, u8* out, int* status) {
  (void)bytes; (void)size; (void)id; (void)out; *status = -1; return 0;
}
#endif
