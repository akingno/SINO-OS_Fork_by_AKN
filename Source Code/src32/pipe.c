/* Bounded byte pipes. Kernel operations never sleep or retain user buffers.
   One object per endpoint; Dup/launch references keep that endpoint alive. */
#include "pipe.h"
#include "process.h"
#include "lib/kstring.h"
#include "mm/page.h"
typedef struct {
  u8* data;
  u32 capacity, head, used, readers, writers, ends;
} Pipe;
static u32 pipe_count;
static void destroy(Pipe* p) {
  page_free(p->data); page_free(p); pipe_count--;
}
void pipe_release(Object* endpoint) {
  Pipe* p = (Pipe*)endpoint->value;
  if (endpoint->type == OBJ_PIPE_WRITE) p->writers = 0; else p->readers = 0;
  process_pipe_wake(endpoint);
  if (!--p->ends) destroy(p);
}
/* Both endpoints share the pipe address. S_AGAIN guarantees a live peer. */
u32 pipe_peer(const Object* endpoint) {
  u32 type = endpoint->type == OBJ_PIPE_READ ? OBJ_PIPE_WRITE : OBJ_PIPE_READ;
  for (u32 i = 0; i < OBJECTS; i++)
    if (objects[i].refs && objects[i].type == type && objects[i].value == endpoint->value)
      return i + 1;
  return 0;
}
void pipe_limits(PipeLimits* out) {
  *out = (PipeLimits){sizeof(*out), 1, PIPE_COUNT, PIPE_BYTES, RUN_IO_MAX,
                      STREAM_WAIT_MAX, 31, 0};
}
int pipe_create(Handle* owner, u32 capacity, PipePair* out) {
  if (!capacity || capacity > PIPE_BYTES) return S_INVALID;
  if (pipe_count >= PIPE_COUNT) return S_LIMIT;
  Pipe* p = page_alloc();
  if (!p) return S_NOMEM;
  p->data = page_alloc();
  if (!p->data) { page_free(p); return S_NOMEM; }
  pipe_count++; p->capacity = capacity;
  int reader = object_new(OBJ_PIPE_READ, (int)p);
  if (reader < 0) { destroy(p); return reader; }
  p->ends = 1; p->readers = 1;
  int writer = object_new(OBJ_PIPE_WRITE, (int)p);
  if (writer < 0) { object_unref(reader); return writer; }
  p->ends = 2; p->writers = 1;
  int rh = handle_open(owner, reader, 1);
  if (rh < 0) { object_unref(reader); object_unref(writer); return rh; }
  int wh = handle_open(owner, writer, 2);
  if (wh < 0) { handle_close(owner, (u32)rh); object_unref(writer); return wh; }
  *out = (PipePair){(u32)rh, (u32)wh};
  return 0;
}
int pipe_acquire(Handle* owner, u32 handle, int writing) {
  Object* o = handle_get(owner, handle, writing ? OBJ_PIPE_WRITE : OBJ_PIPE_READ,
                         writing ? 2 : 1);
  if (!o) return S_BADHANDLE;
  o->refs++;
  return (int)(o - objects);
}
int pipe_ready(Object* o) {
  Pipe* p = (Pipe*)o->value;
  if (o->type == OBJ_PIPE_WRITE)
    return !p->readers ? STREAM_HANGUP : p->used < p->capacity ? STREAM_WRITABLE : 0;
  return (p->used || !p->writers ? STREAM_READABLE : 0) |
         (!p->writers ? STREAM_HANGUP | (!p->used ? STREAM_EOF : 0) : 0);
}
int pipe_transfer(Object* o, int reading, void* buffer, u32 size) {
  Pipe* p = (Pipe*)o->value;
  if (size > RUN_IO_MAX) return S_INVALID;
  if (!size) return 0; /* Not a peer-state/EOF probe. */
  u32 count;
  if (reading) {
    count = p->used;
    if (!count) return p->writers ? S_AGAIN : 0;
  } else {
    if (!p->readers) return S_CLOSED;
    count = p->capacity - p->used;
    if (!count) return S_AGAIN;
  }
  if (count > size) count = size;
  u32 offset = reading ? p->head : (p->head + p->used) % p->capacity;
  u8* bytes = buffer;
  for (u32 i = 0; i < count; i++) {
    if (reading) bytes[i] = p->data[offset]; else p->data[offset] = bytes[i];
    if (++offset == p->capacity) offset = 0;
  }
  if (reading) { p->head = offset; p->used -= count; }
  else p->used += count;
  return (int)count;
}
